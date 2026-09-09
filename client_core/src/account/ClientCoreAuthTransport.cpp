#include "client_core/ClientCoreAuthTransport.h"

#include <algorithm>
#include <chrono>

namespace im {
namespace account {

std::int64_t SystemClock::nowEpochSeconds() const
{
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

ClientCoreAuthTransport::ClientCoreAuthTransport(im::ClientCore& core, std::string serverIp,
                                                 std::uint16_t port)
    : m_core(core), m_ip(std::move(serverIp)), m_port(port)
{
    m_core.setAuthProtocolSink(this);
}

ClientCoreAuthTransport::~ClientCoreAuthTransport()
{
    m_core.setAuthProtocolSink(nullptr);
    clearSession();
    m_reconnectCancelled.store(true);
    joinReconnectThread();
    joinConnectThread();
}

void ClientCoreAuthTransport::setSession(const std::shared_ptr<AccountSession>& session)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_session = session;
}

void ClientCoreAuthTransport::clearSession()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_session.reset();
}

std::shared_ptr<AccountSession> ClientCoreAuthTransport::lockSession() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_session.lock();
}

void ClientCoreAuthTransport::joinConnectThread()
{
    if (m_connectThread.joinable()) m_connectThread.join();
}
void ClientCoreAuthTransport::joinReconnectThread()
{
    if (m_reconnectThread.joinable()) m_reconnectThread.join();
}

AuthResult ClientCoreAuthTransport::toAuthResult(const im::AuthTokenPayload& p)
{
    AuthResult r;
    r.userId = p.userId;
    r.accessToken = p.accessToken;
    r.refreshToken = p.refreshToken;
    r.accessExpireAt = p.accessExpireAt;
    r.refreshExpireAt = p.refreshExpireAt;
    r.sessionId = p.sessionId;
    return r;
}

// ---------------- IAuthTransport ----------------

void ClientCoreAuthTransport::connect()
{
    // 已有连接线程在跑则先回收（连接是短暂阻塞操作）
    joinConnectThread();
    m_connecting.store(true);
    m_connectThread = std::thread([this]() {
        const bool ok = m_core.connectToServer(m_ip, m_port);
        m_connecting.store(false);
        if (auto session = lockSession()) session->onConnectResult(ok);
    });
}

void ClientCoreAuthTransport::disconnect()
{
    m_core.disconnect();
}

ClientCoreAuthTransport::Bytes ClientCoreAuthTransport::appSessionId() const
{
    return m_core.appSessionId();
}

void ClientCoreAuthTransport::sendPasswordLogin(const std::string& account,
                                                const std::string& password,
                                                std::uint32_t generation)
{
    m_passwordGeneration.store(generation);
    m_core.sendLogin(account, password);
}

void ClientCoreAuthTransport::sendTokenLogin(const std::string& payload, std::uint32_t generation)
{
    m_tokenGeneration.store(generation);
    m_core.sendAuthRaw(im::proto::DEF_PROT_TOKEN_LOGIN_RQ, payload);
}

void ClientCoreAuthTransport::sendRefresh(const std::string& payload, std::uint32_t generation)
{
    m_refreshGeneration.store(generation);
    m_core.sendAuthRaw(im::proto::DEF_PROT_TOKEN_REFRESH_RQ, payload);
}

void ClientCoreAuthTransport::sendLogout(const std::string& payload, std::uint32_t generation)
{
    m_logoutGeneration.store(generation);
    m_core.sendAuthRaw(im::proto::DEF_PROT_LOGOUT_RQ, payload);
}

void ClientCoreAuthTransport::scheduleReconnect(std::int64_t delayMs)
{
    if (delayMs < 0) { // 约定：<0 表示取消
        cancelReconnect();
        return;
    }
    cancelReconnect();
    m_reconnectCancelled.store(false);
    m_reconnectThread = std::thread([this, delayMs]() {
        // 分片睡眠以便及时响应取消
        const std::int64_t step = 50;
        std::int64_t slept = 0;
        while (slept < delayMs) {
            if (m_reconnectCancelled.load()) return;
            const std::int64_t chunk = std::min<std::int64_t>(step, delayMs - slept);
            std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
            slept += chunk;
        }
        if (m_reconnectCancelled.load()) return;
        if (auto session = lockSession()) session->onReconnectFired();
    });
}

void ClientCoreAuthTransport::cancelReconnect()
{
    m_reconnectCancelled.store(true);
    joinReconnectThread();
}

// ---------------- IAuthProtocolSink（ClientCore → 适配器 → AccountSession） ----------------

void ClientCoreAuthTransport::onSecureChannelReady(bool ok)
{
    // connect() 的后台线程已通过 connectToServer() 的返回值反映握手结果，此处无需重复。
    (void)ok;
}

void ClientCoreAuthTransport::onLoginRs(const im::AuthTokenPayload& p)
{
    const auto generation = m_passwordGeneration.exchange(0);
    if (auto session = lockSession()) session->onLoginResult(p.result, toAuthResult(p), generation);
}

void ClientCoreAuthTransport::onTokenLoginRs(int result, int userId, std::int64_t accessExpireAt)
{
    const auto generation = m_tokenGeneration.exchange(0);
    if (auto session = lockSession()) {
        session->onTokenLoginResult(result, userId, accessExpireAt, generation);
    }
}

void ClientCoreAuthTransport::onRefreshTokenRs(const im::AuthTokenPayload& p)
{
    const auto generation = m_refreshGeneration.exchange(0);
    if (auto session = lockSession()) session->onRefreshResult(p.result, toAuthResult(p), generation);
}

void ClientCoreAuthTransport::onLogoutRs(int result)
{
    const auto generation = m_logoutGeneration.exchange(0);
    if (auto session = lockSession()) session->onLogoutResult(result, generation);
}

void ClientCoreAuthTransport::onKicked(int reason)
{
    if (auto session = lockSession()) session->onKicked(reason);
}

void ClientCoreAuthTransport::onConnectionClosed()
{
    // 视为非主动断开；被踢/登出场景状态机自身已禁用重连，onDisconnected 不会误触发重连。
    if (auto session = lockSession()) session->onDisconnected(/*intentional=*/false);
}

} // namespace account
} // namespace im
