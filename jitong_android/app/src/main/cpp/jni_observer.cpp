#include "jni_observer.h"

#include <android/log.h>
#include <string>

namespace jt {
namespace {

constexpr const char* kTag = "JniObserver";

// 局部引用上限兜底：Attach 上来的线程默认局部引用表较小，
// 回调里创建的 jstring 等必须及时 DeleteLocalRef，否则长时间运行会溢出。
std::u16string utf8ToUtf16(const std::string& input)
{
    std::u16string out;
    out.reserve(input.size());
    const auto* bytes = reinterpret_cast<const unsigned char*>(input.data());
    std::size_t i = 0;
    while (i < input.size()) {
        std::uint32_t cp = 0;
        std::size_t count = 0;
        const unsigned char lead = bytes[i];
        if (lead < 0x80) { cp = lead; count = 1; }
        else if ((lead & 0xE0) == 0xC0) { cp = lead & 0x1F; count = 2; }
        else if ((lead & 0xF0) == 0xE0) { cp = lead & 0x0F; count = 3; }
        else if ((lead & 0xF8) == 0xF0) { cp = lead & 0x07; count = 4; }
        else { out.push_back(u'\uFFFD'); ++i; continue; }

        if (i + count > input.size()) { out.push_back(u'\uFFFD'); break; }
        bool valid = true;
        for (std::size_t j = 1; j < count; ++j) {
            if ((bytes[i + j] & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (bytes[i + j] & 0x3F);
        }
        const std::uint32_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};
        if (!valid || cp < minimum[count] || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            out.push_back(u'\uFFFD');
            ++i;
            continue;
        }
        if (cp <= 0xFFFF) {
            out.push_back(static_cast<char16_t>(cp));
        } else {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        }
        i += count;
    }
    return out;
}

class LocalString {
public:
    LocalString(JNIEnv* env, const std::string& utf8) : env_(env)
    {
        if (!env_) return;
        const std::u16string utf16 = utf8ToUtf16(utf8);
        ref_ = env_->NewString(reinterpret_cast<const jchar*>(utf16.data()),
                              static_cast<jsize>(utf16.size()));
    }
    ~LocalString()
    {
        if (ref_ && env_) env_->DeleteLocalRef(ref_);
    }
    jstring get() const { return ref_; }

    LocalString(const LocalString&) = delete;
    LocalString& operator=(const LocalString&) = delete;

private:
    JNIEnv* env_;
    jstring ref_ = nullptr;
};

} // namespace

JniObserver::JniObserver(JavaVM* vm, jobject sink) : vm_(vm)
{
    if (!vm_ || !sink) return;

    JNIEnv* env = nullptr;
    bool attached = false;
    if (vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (vm_->AttachCurrentThread(&env, nullptr) != JNI_OK || env == nullptr) return;
        attached = true;
    }

    // 事件回调可能来自 asio 网络线程，句柄必须长期有效 → 全局引用
    sink_ = env->NewGlobalRef(sink);
    if (!sink_) {
        if (attached) vm_->DetachCurrentThread();
        return;
    }

    jclass cls = env->GetObjectClass(sink_);
    if (cls) {
        m_onRegisterResult = env->GetMethodID(cls, "onRegisterResult", "(I)V");
        m_onLoginResult = env->GetMethodID(cls, "onLoginResult", "(II)V");
        m_onSelfInfo = env->GetMethodID(cls, "onSelfInfo", "(ILjava/lang/String;Ljava/lang/String;I)V");
        m_onFriendInfo = env->GetMethodID(cls, "onFriendInfo", "(ILjava/lang/String;II)V");
        m_onChatMessage = env->GetMethodID(cls, "onChatMessage", "(ILjava/lang/String;)V");
        m_onChatSendResult = env->GetMethodID(cls, "onChatSendResult", "(II)V");
        m_onAddFriendRequest = env->GetMethodID(cls, "onAddFriendRequest", "(ILjava/lang/String;)V");
        m_onAddFriendResult = env->GetMethodID(cls, "onAddFriendResult", "(ILjava/lang/String;)V");
        m_onFriendOffline = env->GetMethodID(cls, "onFriendOffline", "(I)V");
        m_onKickedOffline = env->GetMethodID(cls, "onKickedOffline", "(I)V");
        m_onConnectionClosed = env->GetMethodID(cls, "onConnectionClosed", "()V");
        m_onRoamMessages = env->GetMethodID(cls, "onRoamMessages", "(IIZJ)V");
        m_onFileCard = env->GetMethodID(
            cls, "onFileCard", "(ILjava/lang/String;Ljava/lang/String;JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;ZII)V");
        m_onFileProgress = env->GetMethodID(cls, "onFileProgress", "(Ljava/lang/String;III)V");
        env->DeleteLocalRef(cls);
    }

    if (env->ExceptionCheck() == JNI_TRUE) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    valid_ = sink_ != nullptr && m_onRegisterResult && m_onLoginResult && m_onSelfInfo &&
             m_onFriendInfo && m_onChatMessage && m_onChatSendResult &&
             m_onAddFriendRequest && m_onAddFriendResult && m_onFriendOffline &&
             m_onKickedOffline && m_onConnectionClosed && m_onRoamMessages &&
             m_onFileCard && m_onFileProgress;
    if (attached) vm_->DetachCurrentThread();
}

JniObserver::~JniObserver()
{
    if (!vm_ || !sink_) return;

    JNIEnv* env = nullptr;
    bool attached = false;
    if (vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (vm_->AttachCurrentThread(&env, nullptr) != JNI_OK || env == nullptr) {
            // 无法 Attach 时放弃释放全局引用，绝不让异常逃出析构函数
            sink_ = nullptr;
            return;
        }
        attached = true;
    }

    env->DeleteGlobalRef(sink_);
    sink_ = nullptr;

    if (attached) vm_->DetachCurrentThread();
}

bool JniObserver::withEnv(const std::function<void(JNIEnv*)>& body)
{
    if (!vm_ || !sink_) return false;

    JNIEnv* env = nullptr;
    bool attached = false;
    const jint rc = vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        // 回调来自非 Java 线程（asio 网络线程 / 心跳线程）：临时挂到 JVM
        if (vm_->AttachCurrentThread(&env, nullptr) != JNI_OK || env == nullptr) return false;
        attached = true;
    } else if (rc != JNI_OK || env == nullptr) {
        return false;
    }

    body(env);

    // Java 侧抛出的异常若不清理，后续 JNI 调用行为未定义
    if (env->ExceptionCheck() == JNI_TRUE) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }

    // 只 detach 自己 attach 的线程； detach 一个 Java 线程会直接崩溃
    if (attached) vm_->DetachCurrentThread();
    return true;
}

// ---------------- 标量回调 ----------------

void JniObserver::onRegisterResult(int result)
{
    if (!m_onRegisterResult) return;
    withEnv([&](JNIEnv* env) { env->CallVoidMethod(sink_, m_onRegisterResult, result); });
}

void JniObserver::onLoginResult(int result, int userId)
{
    if (!m_onLoginResult) return;
    withEnv([&](JNIEnv* env) { env->CallVoidMethod(sink_, m_onLoginResult, result, userId); });
}

void JniObserver::onChatSendResult(int friId, int result)
{
    if (!m_onChatSendResult) return;
    withEnv([&](JNIEnv* env) { env->CallVoidMethod(sink_, m_onChatSendResult, friId, result); });
}

void JniObserver::onFriendOffline(int userId)
{
    if (!m_onFriendOffline) return;
    withEnv([&](JNIEnv* env) { env->CallVoidMethod(sink_, m_onFriendOffline, userId); });
}

void JniObserver::onKickedOffline(int reason)
{
    if (!m_onKickedOffline) return;
    withEnv([&](JNIEnv* env) { env->CallVoidMethod(sink_, m_onKickedOffline, reason); });
}

void JniObserver::onConnectionClosed()
{
    if (!m_onConnectionClosed) return;
    withEnv([&](JNIEnv* env) { env->CallVoidMethod(sink_, m_onConnectionClosed); });
}

// ---------------- 带字符串的回调 ----------------

void JniObserver::onSelfInfo(const im::UserInfo& info)
{
    if (!m_onSelfInfo) return;
    withEnv([&](JNIEnv* env) {
        LocalString nick(env, info.nick);
        LocalString feeling(env, info.feeling);
        env->CallVoidMethod(sink_, m_onSelfInfo, info.id, nick.get(), feeling.get(), info.iconId);
    });
}

void JniObserver::onFriendInfo(const im::FriendInfo& info)
{
    if (!m_onFriendInfo) return;
    withEnv([&](JNIEnv* env) {
        LocalString nick(env, info.nick);
        env->CallVoidMethod(sink_, m_onFriendInfo, info.id, nick.get(), info.status, info.iconId);
    });
}

void JniObserver::onChatMessage(int fromId, const std::string& msgUtf8)
{
    if (!m_onChatMessage) return;
    withEnv([&](JNIEnv* env) {
        LocalString msg(env, msgUtf8);
        env->CallVoidMethod(sink_, m_onChatMessage, fromId, msg.get());
    });
}

void JniObserver::onAddFriendRequest(int fromId, const std::string& fromNickUtf8)
{
    if (!m_onAddFriendRequest) return;
    withEnv([&](JNIEnv* env) {
        LocalString nick(env, fromNickUtf8);
        env->CallVoidMethod(sink_, m_onAddFriendRequest, fromId, nick.get());
    });
}

void JniObserver::onAddFriendResult(int result, const std::string& destNickUtf8)
{
    if (!m_onAddFriendResult) return;
    withEnv([&](JNIEnv* env) {
        LocalString nick(env, destNickUtf8);
        env->CallVoidMethod(sink_, m_onAddFriendResult, result, nick.get());
    });
}

void JniObserver::onRoamMessages(int peerId, const std::vector<im::RoamMessage>& msgs, bool hasMore,
                                 std::int64_t minSeq)
{
    if (!m_onRoamMessages) return;
    // P8 之前只投递批次元信息；消息正文由内核落库后经 Query 订阅上抛，
    // 避免在 JNI 边界搬运整个列表（也避免回调里做业务合并）。
    const auto count = static_cast<int>(msgs.size());
    withEnv([&](JNIEnv* env) {
        env->CallVoidMethod(sink_, m_onRoamMessages, peerId, count,
                            hasMore ? JNI_TRUE : JNI_FALSE, static_cast<jlong>(minSeq));
    });
}

void JniObserver::onFileCard(int fromId, const std::string& fileId, const std::string& name,
                             std::int64_t size, const std::string& msgId,
                             const std::string& contentType, const std::string& sha256,
                             bool isImage, int imageWidth, int imageHeight)
{
    if (!m_onFileCard) return;
    withEnv([&](JNIEnv* env) {
        LocalString jFileId(env, fileId);
        LocalString jName(env, name);
        LocalString jMsgId(env, msgId);
        LocalString jContentType(env, contentType);
        LocalString jSha256(env, sha256);
        env->CallVoidMethod(sink_, m_onFileCard, fromId, jFileId.get(), jName.get(),
                            static_cast<jlong>(size), jMsgId.get(), jContentType.get(),
                            jSha256.get(), isImage ? JNI_TRUE : JNI_FALSE, imageWidth,
                            imageHeight);
    });
}

void JniObserver::onFileProgress(const std::string& fileId, int received, int total, int status)
{
    if (!m_onFileProgress) return;
    withEnv([&](JNIEnv* env) {
        LocalString jFileId(env, fileId);
        env->CallVoidMethod(sink_, m_onFileProgress, jFileId.get(), received, total, status);
    });
}

// ---------------- 尚未下沉的回调（P7/P10 阶段再接入） ----------------
// 这些路径当前没有对应的 Kotlin 侧消费方。保持空实现：
// 既不伪造数据，也不让未实现的纯虚导致编译失败。

void JniObserver::onImageMessage(int, const std::string&, int, int, const std::string&)
{
    // 已废弃：M7 起图片走 MediaCard，见 ClientCore.h 注释
}

void JniObserver::onRoamConversations(const std::vector<im::RoamMessage>&)
{
    // P8：会话列表订阅改走 observeConversations 后接入
}

void JniObserver::onFileOfferResult(const im::FileOfferInfo&)
{
    // P11：分片传输协议已由 HTTP 文件服务取代
}

void JniObserver::onFileChunk(const std::string&, int, const std::string&)
{
    // P11：同上
}

} // namespace jt
