// 生产版认证传输适配器（P5-T06）。
//
// 把 AccountSession 需要的 IAuthTransport 接到真实的 im::ClientCore：
//   - connect() 在后台线程调用 ClientCore::connectToServer()（同步阻塞握手），完成后
//     异步回调 onConnectResult；同时把 ClientCore 的 IAuthProtocolSink 回调翻译成
//     AccountSession 的 IAuthTransportCallbacks。
//   - sendPasswordLogin 走 ClientCore::sendLogin；sendTokenLogin/Refresh/Logout 走
//     ClientCore::sendAuthRaw（已构造好的 payload）。
//   - scheduleReconnect 用一个一次性定时线程实现退避回调。
//
// 线程约定：AccountSession 的方法都在同一逻辑线程调用；ClientCore 的回调来自其 IO/后台
// 线程，本适配器负责把它们 marshal 回 AccountSession（此处用互斥 + 直接转发，AccountSession
// 内部不自带锁，故所有对 session 的回调都在持有 m_mutex 时串行进入）。

#ifndef CLIENT_CORE_CLIENT_CORE_AUTH_TRANSPORT_H
#define CLIENT_CORE_CLIENT_CORE_AUTH_TRANSPORT_H

#include "client_core/AccountSession.h"
#include "client_core/ClientCore.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace im {
namespace account {

/** 真实系统时钟（epoch 秒）。 */
class SystemClock : public IClock {
public:
    std::int64_t nowEpochSeconds() const override;
};

class ClientCoreAuthTransport : public IAuthTransport, public im::IAuthProtocolSink {
public:
    ClientCoreAuthTransport(im::ClientCore& core, std::string serverIp, std::uint16_t port);
    ~ClientCoreAuthTransport() override;

    // 关联 AccountSession（回调目标）。必须在使用前设置。
    void setSession(const std::shared_ptr<AccountSession>& session);
    void clearSession();

    // ---- IAuthTransport ----
    void connect() override;
    void disconnect() override;
    Bytes appSessionId() const override;
    void sendPasswordLogin(const std::string& account, const std::string& password,
                           std::uint32_t generation) override;
    void sendTokenLogin(const std::string& payload, std::uint32_t generation) override;
    void sendRefresh(const std::string& payload, std::uint32_t generation) override;
    void sendLogout(const std::string& payload, std::uint32_t generation) override;
    void scheduleReconnect(std::int64_t delayMs) override;
    void cancelReconnect() override;

    // ---- im::IAuthProtocolSink（来自 ClientCore 的回调） ----
    void onSecureChannelReady(bool ok) override;
    void onLoginRs(const im::AuthTokenPayload& p) override;
    void onTokenLoginRs(int result, int userId, std::int64_t accessExpireAt) override;
    void onRefreshTokenRs(const im::AuthTokenPayload& p) override;
    void onLogoutRs(int result) override;
    void onKicked(int reason) override;
    void onConnectionClosed() override;

private:
    static AuthResult toAuthResult(const im::AuthTokenPayload& p);
    std::shared_ptr<AccountSession> lockSession() const;
    void joinConnectThread();
    void joinReconnectThread();

    im::ClientCore& m_core;
    std::string m_ip;
    std::uint16_t m_port;
    std::weak_ptr<AccountSession> m_session;

    mutable std::mutex m_mutex; // 仅保护 weak session；绝不持锁执行外部回调
    std::thread m_connectThread;
    std::thread m_reconnectThread;
    std::atomic<bool> m_reconnectCancelled{false};
    std::atomic<bool> m_connecting{false};
    std::atomic<std::uint32_t> m_passwordGeneration{0};
    std::atomic<std::uint32_t> m_tokenGeneration{0};
    std::atomic<std::uint32_t> m_refreshGeneration{0};
    std::atomic<std::uint32_t> m_logoutGeneration{0};
};

} // namespace account
} // namespace im

#endif // CLIENT_CORE_CLIENT_CORE_AUTH_TRANSPORT_H
