#pragma once
// JNI 事件桥：把 C++ IClientEvents 回调投递到 Kotlin。
//
// 约束（执行规划 P2-T02）：
//   - 缓存 JavaVM*、jclass、jmethodID（避免每次回调 FindClass/GetMethodID）；
//   - 正确 New/DeleteGlobalRef；
//   - 只在「本线程由 JNI 临时 Attach」时才 Detach（避免 detach 掉 Java 线程）；
//   - 每次 CallVoidMethod 后检查并清理异常，绝不让异常跨越 JNI 边界回到 C++；
//   - 回调只投递事件，不做任何业务判断。

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <jni.h>

#include "client_core/ClientCore.h"

namespace jt {

class JniObserver final : public im::IClientEvents {
public:
    /**
     * @param vm   进程唯一的 JavaVM*
     * @param sink Kotlin 侧事件接收对象；本类会对其创建全局引用
     */
    JniObserver(JavaVM* vm, jobject sink);
    ~JniObserver() override;

    JniObserver(const JniObserver&) = delete;
    JniObserver& operator=(const JniObserver&) = delete;

    /** 所有必需回调签名和 GlobalRef 均成功建立。 */
    bool valid() const { return valid_; }

    // ---------------- IClientEvents ----------------
    void onRegisterResult(int result) override;
    void onLoginResult(int result, int userId) override;
    void onSelfInfo(const im::UserInfo& info) override;
    void onFriendInfo(const im::FriendInfo& info) override;
    void onChatMessage(int fromId, const std::string& msgUtf8) override;
    void onImageMessage(int fromId, const std::string& imageBytes, int w, int h,
                        const std::string& msgId) override;
    void onChatSendResult(int friId, int result) override;
    void onAddFriendRequest(int fromId, const std::string& fromNickUtf8) override;
    void onAddFriendResult(int result, const std::string& destNickUtf8) override;
    void onFriendOffline(int userId) override;
    void onKickedOffline(int reason) override;
    void onConnectionClosed() override;

    void onRoamConversations(const std::vector<im::RoamMessage>& convs) override;
    void onRoamMessages(int peerId, const std::vector<im::RoamMessage>& msgs, bool hasMore,
                        std::int64_t minSeq) override;
    void onFileOfferResult(const im::FileOfferInfo& info) override;
    void onFileChunk(const std::string& fileId, int chunkIndex, const std::string& data) override;
    void onFileProgress(const std::string& fileId, int received, int total, int status) override;
    void onFileCard(int fromId, const std::string& fileId, const std::string& name,
                    std::int64_t size, const std::string& msgId, const std::string& contentType,
                    const std::string& sha256, bool isImage, int imageWidth,
                    int imageHeight) override;

private:
    /**
     * 取得当前线程的 JNIEnv：若线程尚未绑定到 JVM 则临时 Attach，
     * 作用域结束时仅在「是本次 Attach 的」情况下 Detach。
     *
     * @return 是否成功取得 env；失败时不应继续调用 Java
     */
    bool withEnv(const std::function<void(JNIEnv*)>& body);

    JavaVM* vm_ = nullptr;
    jobject sink_ = nullptr; // GlobalRef
    bool valid_ = false;

    // 缓存的 methodID（不需要 Delete，随类卸载失效）
    jmethodID m_onRegisterResult = nullptr;
    jmethodID m_onLoginResult = nullptr;
    jmethodID m_onSelfInfo = nullptr;
    jmethodID m_onFriendInfo = nullptr;
    jmethodID m_onChatMessage = nullptr;
    jmethodID m_onChatSendResult = nullptr;
    jmethodID m_onAddFriendRequest = nullptr;
    jmethodID m_onAddFriendResult = nullptr;
    jmethodID m_onFriendOffline = nullptr;
    jmethodID m_onKickedOffline = nullptr;
    jmethodID m_onConnectionClosed = nullptr;
    jmethodID m_onRoamMessages = nullptr;
    jmethodID m_onFileCard = nullptr;
    jmethodID m_onFileProgress = nullptr;
};

} // namespace jt
