// P7-G3：账号级 Runtime 的 JNI 生命周期接口。
//
// 契约：
//   - Runtime 由 NativeSdkHandle 唯一持有；销毁在 destroyHandleLocked 中完成
//     （runtime → db → dbKeyBridge → core → observer）。
//   - 本文件只暴露**生命周期与状态**，不暴露 SQL、Socket、Token、DB key、
//     sqlite3* 或 C++ 裸对象地址——所有跨边界返回值都是基本类型或字符串。
//   - 取 runtime 指针时在 handle 锁内拷贝 shared_ptr，锁外调用，避免与 destroy 竞态。

#include <jni.h>

#include <memory>
#include <mutex>
#include <string>
#include <cstdint>
#include <vector>

#include "client_core/runtime/ClientRuntime.h"
#include "native_sdk_handle.h"
#include "jni_runtime_observer.h"

extern "C" JavaVM* jtGlobalJavaVm();

namespace {

std::string fromJString(JNIEnv* env,jstring value) {
    if(!value)return {}; const char* chars=env->GetStringUTFChars(value,nullptr);
    std::string out=chars?chars:""; if(chars)env->ReleaseStringUTFChars(value,chars); return out;
}

std::shared_ptr<im::runtime::ClientRuntime> acquireRuntime(jlong handle)
{
    auto h = jt::lookupHandle(handle);
    if (!h) return nullptr;
    std::lock_guard<std::mutex> lk(h->mutex);
    if (h->isDestroying()) return nullptr;
    return h->runtime;
}

std::shared_ptr<im::ClientCore> acquireCore(jlong handle)
{
    auto h=jt::lookupHandle(handle);if(!h)return nullptr;
    std::lock_guard<std::mutex> lk(h->mutex);
    return h->isDestroying()?nullptr:h->core;
}

std::shared_ptr<std::atomic<bool>> mediaOperation(jlong handle,jlong id)
{
    auto h=jt::lookupHandle(handle);if(!h||id<=0)return nullptr;
    std::lock_guard<std::mutex> lk(h->mutex);
    if(h->isDestroying())return nullptr;
    auto it=h->mediaOperations.find(static_cast<std::uint64_t>(id));
    return it==h->mediaOperations.end()?nullptr:it->second;
}

class ByteWriter {
public:
    void u32(std::uint32_t value) {
        for(int i=0;i<4;++i)m_bytes.push_back(static_cast<std::uint8_t>(value>>(i*8)));
    }
    void i32(std::int32_t value){u32(static_cast<std::uint32_t>(value));}
    void i64(std::int64_t value){
        const auto raw=static_cast<std::uint64_t>(value);
        for(int i=0;i<8;++i)m_bytes.push_back(static_cast<std::uint8_t>(raw>>(i*8)));
    }
    bool str(const std::string& value){
        if(value.size()>1024*1024)return false;
        u32(static_cast<std::uint32_t>(value.size()));
        m_bytes.insert(m_bytes.end(),value.begin(),value.end());return true;
    }
    jbyteArray array(JNIEnv* env) const {
        auto out=env->NewByteArray(static_cast<jsize>(m_bytes.size()));if(!out)return nullptr;
        if(!m_bytes.empty())env->SetByteArrayRegion(out,0,static_cast<jsize>(m_bytes.size()),
            reinterpret_cast<const jbyte*>(m_bytes.data()));return out;
    }
private:
    std::vector<std::uint8_t> m_bytes;
};

} // namespace

extern "C" {

// boolean nativeCreateRuntime(long handle, int ownerId)
JNIEXPORT jboolean JNICALL
Java_com_jitong_im_core_NativeBindings_nativeCreateRuntime(JNIEnv*, jclass, jlong handle,
                                                           jint ownerId)
{
    auto h = jt::lookupHandle(handle);
    if (!h || h->isDestroying()) return JNI_FALSE;
    if (ownerId <= 0) return JNI_FALSE;

    im::runtime::ClientRuntime::Config cfg;
    cfg.ownerId = static_cast<std::int64_t>(ownerId);
    cfg.completionCapacity = 256;

    auto rt = std::make_shared<im::runtime::ClientRuntime>(cfg);
    std::shared_ptr<im::account::AccountSession> account;
    std::shared_ptr<im::ClientCore> core;
    {
        std::lock_guard<std::mutex> lk(h->mutex);
        if (h->isDestroying()) return JNI_FALSE;
        // Do not replace/destroy a live runtime while holding the handle mutex.
        // Account changes require a new handle and explicit teardown of the old one.
        if (h->runtime) {
            return h->runtime->ownerId() == ownerId &&
                   h->runtime->state() != im::runtime::ClientRuntime::State::Destroyed
                       ? JNI_TRUE : JNI_FALSE;
        }
        // 复用句柄唯一 ClientCore（唯一 Socket/安全通道），不在 Runtime 创建第二条连接。
        if (!h->core || !rt->attachClientCore(h->core)) return JNI_FALSE;
        // 若账号库已打开则共享所有权（NativeDatabase::close 幂等，故销毁顺序不敏感）
        if (h->db && !rt->setDatabase(h->db)) return JNI_FALSE;
        h->runtime = rt;
        account = h->account;
        core = h->core;
    }

    // AccountSession 可能在 Runtime 创建前已经完成登录。账号事件不会重放，若只依赖
    // im_auth_jni.cpp 的实时回调，Runtime 的发送门会永久保持关闭。创建时补一次当前态
    // 对齐，使“先登录、后开库/建 Runtime”和“先建 Runtime、后登录”语义一致。
    // 这里必须在 handle 锁外读取/下发状态，避免状态查询或后续事件投递发生锁重入。
    if (account && core) {
        rt->setAccountAuthenticated(
            account->accountState() == im::account::AccountState::Authenticated &&
            core->isConnected());
    }
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_jitong_im_core_NativeBindings_nativeSetRuntimeEventSink(JNIEnv*,jclass,jlong handle,
                                                                  jobject sink)
{
    if(!sink)return JNI_FALSE;
    auto rt=acquireRuntime(handle);if(!rt)return JNI_FALSE;
    auto observer=std::make_shared<jt::JniRuntimeObserver>(jtGlobalJavaVm(),sink);
    if(!observer->valid())return JNI_FALSE;
    rt->setRuntimeEventSink(observer);return JNI_TRUE;
}

// long nativeStartRuntime(long handle)
JNIEXPORT jlong JNICALL
Java_com_jitong_im_core_NativeBindings_nativeStartRuntime(JNIEnv*, jclass, jlong handle)
{
    auto rt = acquireRuntime(handle);
    if (!rt) return 0;
    std::string err;
    if (!rt->start(&err)) return 0;
    return static_cast<jlong>(rt->generation());
}

// void nativeStopRuntime(long handle)
JNIEXPORT void JNICALL
Java_com_jitong_im_core_NativeBindings_nativeStopRuntime(JNIEnv*, jclass, jlong handle)
{
    if (auto rt = acquireRuntime(handle)) rt->stop();
}

// void nativeLogoutRuntime(long handle)
JNIEXPORT void JNICALL
Java_com_jitong_im_core_NativeBindings_nativeLogoutRuntime(JNIEnv*, jclass, jlong handle)
{
    if (auto rt = acquireRuntime(handle)) rt->logout();
}

// void nativeDestroyRuntime(long handle)
JNIEXPORT void JNICALL
Java_com_jitong_im_core_NativeBindings_nativeDestroyRuntime(JNIEnv*, jclass, jlong handle)
{
    auto h = jt::lookupHandle(handle);
    if (!h) return;
    std::shared_ptr<im::runtime::ClientRuntime> runtime;
    {
        std::lock_guard<std::mutex> lk(h->mutex);
        if (h->isDestroying()) return;
        runtime = std::move(h->runtime);
    }
    // 析构/停止线程/清协议 sink 绝不能发生在 handle 锁内。
    if (runtime) runtime->destroy();
}

// String nativeGetRuntimeState(long handle)
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeGetRuntimeState(JNIEnv* env, jclass, jlong handle)
{
    auto rt = acquireRuntime(handle);
    const char* name = rt ? rt->stateName() : "None";
    return env->NewStringUTF(name);
}

// String nativeRuntimeSendText(handle, conversationId, peerId, text, pinyin, initials)
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRuntimeSendText(JNIEnv* env,jclass,jlong handle,
    jlong conversationId,jlong peerId,jstring text,jstring pinyin,jstring initials)
{
    auto rt=acquireRuntime(handle);
    if(!rt)return env->NewStringUTF("err|runtime_unavailable");
    const auto result=rt->sendText(conversationId,peerId,fromJString(env,text),
                                   fromJString(env,pinyin),fromJString(env,initials));
    const std::string encoded=result.accepted
        ? "ok|"+result.operationId+"|"+result.msgId+"|"+std::to_string(result.localOrder)
        : "err|"+result.operationId+"|"+result.msgId+"|"+result.error;
    return env->NewStringUTF(encoded.c_str());
}

JNIEXPORT jlong JNICALL
Java_com_jitong_im_core_NativeBindings_nativeBeginMediaOperation(JNIEnv*,jclass,jlong handle)
{
    auto h=jt::lookupHandle(handle);if(!h)return 0;
    std::lock_guard<std::mutex> lk(h->mutex);
    if(h->isDestroying())return 0;
    const auto id=h->nextMediaOperation++;
    h->mediaOperations.emplace(id,std::make_shared<std::atomic<bool>>(false));
    return static_cast<jlong>(id);
}

JNIEXPORT void JNICALL
Java_com_jitong_im_core_NativeBindings_nativeCancelMediaOperation(JNIEnv*,jclass,jlong handle,jlong id)
{
    auto h=jt::lookupHandle(handle);if(!h||id<=0)return;
    std::lock_guard<std::mutex> lk(h->mutex);
    auto it=h->mediaOperations.find(static_cast<std::uint64_t>(id));
    if(it!=h->mediaOperations.end())it->second->store(true,std::memory_order_release);
}

JNIEXPORT void JNICALL
Java_com_jitong_im_core_NativeBindings_nativeEndMediaOperation(JNIEnv*,jclass,jlong handle,jlong id)
{
    auto h=jt::lookupHandle(handle);if(!h||id<=0)return;
    std::lock_guard<std::mutex> lk(h->mutex);
    h->mediaOperations.erase(static_cast<std::uint64_t>(id));
}

// String nativeRuntimeUploadMedia(handle,operation,path,receiver,isImage)
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRuntimeUploadMedia(JNIEnv* env,jclass,jlong handle,
    jlong operation,jstring path,jlong receiver,jboolean image)
{
    auto core=acquireCore(handle);auto flag=mediaOperation(handle,operation);std::string id;
#if defined(CLIENT_CORE_WITH_MEDIA)
    if(core&&flag&&receiver>0)id=core->uploadMedia(fromJString(env,path),static_cast<int>(receiver),
        image==JNI_TRUE,nullptr,[flag]{return flag->load(std::memory_order_acquire);});
#endif
    return env->NewStringUTF(id.c_str());
}

JNIEXPORT jboolean JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRuntimeDownloadMedia(JNIEnv* env,jclass,jlong handle,
    jlong operation,jstring fileId,jstring destination)
{
    auto core=acquireCore(handle);auto flag=mediaOperation(handle,operation);bool ok=false;
#if defined(CLIENT_CORE_WITH_MEDIA)
    if(core&&flag)ok=core->downloadMedia(fromJString(env,fileId),fromJString(env,destination),
        nullptr,[flag]{return flag->load(std::memory_order_acquire);});
#endif
    return ok?JNI_TRUE:JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRuntimeSendMedia(JNIEnv* env,jclass,jlong handle,
    jlong conversationId,jlong peerId,jint type,jstring fileId,jstring fileName,jlong fileSize,
    jstring contentType,jstring sha256,jint width,jint height,jstring localPath,
    jstring thumbId,jlong thumbSize,jstring thumbHash,jint thumbW,jint thumbH,
    jstring largeId,jlong largeSize,jstring largeHash,jint largeW,jint largeH)
{
    auto rt=acquireRuntime(handle);if(!rt)return env->NewStringUTF("err|runtime_unavailable");
    im::dto::MessageDto m;m.conversationId=conversationId;m.peerId=peerId;m.type=type;
    m.fileId=fromJString(env,fileId);m.fileName=fromJString(env,fileName);m.fileSize=fileSize;
    m.contentType=fromJString(env,contentType);m.sha256=fromJString(env,sha256);m.imgW=width;m.imgH=height;
    m.localPath=fromJString(env,localPath);m.thumbnailFileId=fromJString(env,thumbId);
    m.thumbnailSize=thumbSize;m.thumbnailSha256=fromJString(env,thumbHash);m.thumbnailW=thumbW;m.thumbnailH=thumbH;
    m.largeThumbnailFileId=fromJString(env,largeId);m.largeThumbnailSize=largeSize;
    m.largeThumbnailSha256=fromJString(env,largeHash);m.largeThumbnailW=largeW;m.largeThumbnailH=largeH;
    const auto result=rt->sendMedia(m);
    const std::string encoded=result.accepted?"ok|"+result.operationId+"|"+result.msgId+"|"+
        std::to_string(result.localOrder):"err|"+result.operationId+"|"+result.msgId+"|"+result.error;
    return env->NewStringUTF(encoded.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRuntimeMarkRead(JNIEnv* env,jclass,jlong handle,
                                                              jlong conversationId,jlong readSeq)
{
    auto rt=acquireRuntime(handle);if(!rt)return env->NewStringUTF("");
    const auto operation=rt->markRead(conversationId,readSeq);
    return env->NewStringUTF(operation.c_str());
}

JNIEXPORT jint JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRuntimeFlushOutbox(JNIEnv*,jclass,jlong handle,
                                                                 jlong nowSeconds,jint limit)
{
    auto rt=acquireRuntime(handle); return rt?rt->flushOutbox(nowSeconds,limit):0;
}

// long nativeRuntimeConsumeInvalidation(handle, domain)
JNIEXPORT jlong JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRuntimeConsumeInvalidation(JNIEnv*,jclass,
                                                                         jlong handle,jint domain)
{
    auto rt=acquireRuntime(handle);
    if(!rt||domain<0||domain>4)return 0;
    return static_cast<jlong>(rt->consumeInvalidation(
        static_cast<im::runtime::InvalidationBus::Domain>(domain)));
}

// String nativeRuntimeConsumeOperation(handle, operationId)
// "pending" means that the operation has no terminal record yet.  The error is
// the final field, so Kotlin can split with limit=3 without corrupting messages
// that happen to contain '|'.
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRuntimeConsumeOperation(JNIEnv* env,jclass,
    jlong handle,jstring operationId)
{
    auto rt=acquireRuntime(handle);
    if(!rt)return env->NewStringUTF("unavailable");
    im::runtime::CompletionRegistry::Record record;
    if(!rt->consumeOperation(fromJString(env,operationId),&record))
        return env->NewStringUTF("pending");
    const std::string encoded="done|"+std::to_string(static_cast<int>(record.result))+"|"+
                              record.errorMessage;
    return env->NewStringUTF(encoded.c_str());
}

JNIEXPORT jboolean JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRuntimeRequestRoamConversations(JNIEnv*,jclass,
                                                                              jlong handle)
{
    auto rt=acquireRuntime(handle);
    return rt&&rt->requestRoamConversations()?JNI_TRUE:JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRuntimeRequestRoamMessages(JNIEnv*,jclass,jlong handle,
    jlong peerId,jlong beforeSeq,jint limit)
{
    auto rt=acquireRuntime(handle);
    return rt&&rt->requestRoamMessages(peerId,beforeSeq,limit)?JNI_TRUE:JNI_FALSE;
}

// JTFD v1: magic/version/count + [friendId,nick,tel,avatar,signature,sex,online].
JNIEXPORT jbyteArray JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRuntimeLoadFriends(JNIEnv* env,jclass,jlong handle)
{
    auto rt=acquireRuntime(handle);if(!rt)return nullptr;
    std::vector<im::dto::FriendDto> friends;std::string error;
    if(!rt->loadFriends(&friends,&error)||friends.size()>100000)return nullptr;
    ByteWriter writer;writer.u32(0x4A544644u);writer.u32(1);writer.u32(friends.size());
    for(const auto& f:friends){writer.i64(f.friendId);
        if(!writer.str(f.nick)||!writer.str(f.tel)||!writer.str(f.avatar)||!writer.str(f.signature))
            return nullptr;
        writer.i32(f.sex);writer.i32(f.online?1:0);
    }
    return writer.array(env);
}

// JTFR v1: magic/version/count + [requestId,from,to,direction,state,message,createdAt].
JNIEXPORT jbyteArray JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRuntimeLoadFriendRequests(JNIEnv* env,jclass,
                                                                        jlong handle)
{
    auto rt=acquireRuntime(handle);if(!rt)return nullptr;
    std::vector<im::dto::FriendRequestDto> requests;std::string error;
    if(!rt->loadFriendRequests(&requests,&error)||requests.size()>100000)return nullptr;
    ByteWriter writer;writer.u32(0x4A544652u);writer.u32(1);writer.u32(requests.size());
    for(const auto& r:requests){if(!writer.str(r.requestId))return nullptr;
        writer.i64(r.fromUserId);writer.i64(r.toUserId);
        writer.i32(static_cast<std::int32_t>(r.direction));
        writer.i32(static_cast<std::int32_t>(r.state));
        if(!writer.str(r.message))return nullptr;writer.i64(r.createdAt);
    }
    return writer.array(env);
}

JNIEXPORT jboolean JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRuntimeRequestFriendRequests(JNIEnv*,jclass,jlong handle)
{auto rt=acquireRuntime(handle);return rt&&rt->requestFriendRequests()?JNI_TRUE:JNI_FALSE;}

JNIEXPORT jboolean JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRuntimeSendAddFriend(JNIEnv* env,jclass,jlong handle,
                                                                   jstring nick)
{auto rt=acquireRuntime(handle);return rt&&rt->sendAddFriendRequest(fromJString(env,nick))?
    JNI_TRUE:JNI_FALSE;}

JNIEXPORT jboolean JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRuntimeAnswerFriend(JNIEnv* env,jclass,jlong handle,
    jlong requesterId,jstring requesterNick,jboolean agree)
{auto rt=acquireRuntime(handle);return rt&&rt->answerFriendRequest(requesterId,
    fromJString(env,requesterNick),agree==JNI_TRUE)?JNI_TRUE:JNI_FALSE;}

JNIEXPORT jboolean JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRuntimeDeleteFriend(JNIEnv*,jclass,jlong handle,
                                                                  jlong friendId)
{auto rt=acquireRuntime(handle);return rt&&rt->deleteFriend(friendId)?JNI_TRUE:JNI_FALSE;}

} // extern "C"
