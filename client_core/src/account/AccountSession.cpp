#include "client_core/AccountSession.h"

namespace im {
namespace account {

namespace {
// 与协议一致：各类结果 0 表示成功（LOGIN_SUCCESS / REFRESH_TOKEN_SUCCESS / LOGOUT_SUCCESS 均为 0）。
constexpr int kResultSuccess = 0;
constexpr int kLoginPassError = 2; // LOGIN_PASSERROR
constexpr int kLoginNotExist = 1;  // LOGIN_NOTEXIT

AuthError mapLoginError(int protoResult)
{
    if (protoResult == kLoginPassError || protoResult == kLoginNotExist) {
        return AuthError::InvalidCredentials;
    }
    return AuthError::ServerRejected;
}
} // namespace

AccountSession::AccountSession(IAuthTransport& transport, std::shared_ptr<IClock> clock,
                               std::shared_ptr<ITokenStore> store, const IP256Signer& signer,
                               std::string deviceId, std::shared_ptr<IJitter> jitter)
    : m_transport(transport),
      m_clock(clock),
      m_tokens(clock, std::move(store)),
      m_proof(signer, deviceId),
      m_reqBuilder(m_proof),
      m_reconnect(1000, 30000, 0.25, std::move(jitter))
{
}

void AccountSession::emit(const AccountEvent& e)
{
    if (m_sink) m_sink(e);
}

void AccountSession::runActions(const AuthStateMachine::Actions& actions)
{
    for (const auto& a : actions) {
        switch (a.type) {
            case ActionType::Connect:
                m_generation = m_sm.generation();
                m_transport.connect();
                break;
            case ActionType::SendPasswordLogin:
                // 状态机已按 authMethod 决定发密码登录；此处只负责发送并尽快清除明文。
                m_transport.sendPasswordLogin(m_account, m_pendingPassword.str(), m_sm.generation());
                m_pendingPassword.clear();
                break;
            case ActionType::SendTokenLogin: {
                const auto req = m_reqBuilder.buildTokenLogin(
                    m_transport.appSessionId(), m_tokens.accessToken().str(),
                    m_tokens.sessionId());
                if (req.ok) {
                    m_transport.sendTokenLogin(req.payload, m_sm.generation());
                } else {
                    // 设备签名失败：把本次认证判为失败（设备证明错误）
                    AuthStateMachine::Actions failActs;
                    m_sm.onAuthFailed(m_generation, AuthError::DeviceProofFailed, failActs);
                    runActions(failActs);
                }
                break;
            }
            case ActionType::SendRefresh: {
                const std::string reqId = m_tokens.beginRefresh();
                if (reqId.empty()) {
                    // 持久化 request_id 失败：不得发送刷新，判为刷新失败（走网络类错误，不吊销）
                    AuthStateMachine::Actions failActs;
                    m_sm.onRefreshFailed(m_generation, AuthError::Internal, failActs);
                    runActions(failActs);
                    break;
                }
                const auto req = m_reqBuilder.buildRefresh(
                    m_transport.appSessionId(), m_tokens.refreshToken().str(), reqId);
                if (req.ok) {
                    m_transport.sendRefresh(req.payload, m_sm.generation());
                } else {
                    onRefreshResult(1, AuthResult{}, m_sm.generation()); // 视为刷新失败
                }
                break;
            }
            case ActionType::SendLogout: {
                const auto req = m_reqBuilder.buildLogout(
                    m_transport.appSessionId(), m_tokens.refreshToken().str(), m_logoutAllDevices);
                if (req.ok) m_transport.sendLogout(req.payload, m_sm.generation());
                else onLogoutResult(kResultSuccess, m_sm.generation()); // 无凭据可登出，直接完成
                break;
            }
            case ActionType::Disconnect:
                m_transport.disconnect();
                break;
            case ActionType::ScheduleReconnect: {
                const std::int64_t delay = m_reconnect.nextDelayMs();
                m_transport.scheduleReconnect(delay);
                break;
            }
            case ActionType::CancelReconnect:
                m_reconnect.disable();
                m_transport.cancelReconnect();
                break;
            case ActionType::EmitEvent:
                emit(a.event);
                break;
            case ActionType::RejectOperation: {
                AccountEvent e;
                e.type = AccountEventType::LoginFailed;
                e.error = a.error;
                e.accountState = m_sm.state();
                emit(e);
                break;
            }
            case ActionType::None:
            default:
                break;
        }
    }
}

// ---------------- UI 意图 ----------------

std::uint64_t AccountSession::startWithPassword(const std::string& account,
                                                const std::string& password)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    AuthStateMachine::Actions acts;
    const auto op = m_sm.requestPasswordLogin(account, acts);
    if (op == 0 || acts.empty()) {
        runActions(acts);
        return op;
    }
    m_account = account;
    m_pendingPassword.assign(password);
    m_tokenLoginAfterRefresh = false;
    m_reconnect.reset();
    runActions(acts);
    return op;
}

std::uint64_t AccountSession::startWithSavedToken(const std::string& account)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_tokens.loadFromStore(account)) {
        return 0; // 无有效凭据，需要密码登录
    }
    if (!m_tokens.accessUsable() && m_tokens.refreshExpired()) {
        m_tokens.revokeFamily();
        return 0;
    }
    m_account = account;
    m_reconnect.reset();
    AuthStateMachine::Actions acts;
    std::uint64_t op = 0;
    if (!m_tokens.accessUsable()) {
        m_tokenLoginAfterRefresh = true;
        op = m_sm.requestRefreshForLogin(account, acts);
    } else {
        m_tokenLoginAfterRefresh = false;
        op = m_sm.requestTokenLogin(account, acts);
    }
    runActions(acts);
    return op;
}

void AccountSession::logout(bool allDevices)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_logoutAllDevices = allDevices;
    AuthStateMachine::Actions acts;
    m_sm.requestLogout(acts);
    m_generation = m_sm.generation();
    runActions(acts);
}

void AccountSession::cancel()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_pendingPassword.clear();
    m_tokenLoginAfterRefresh = false;
    m_tokens.cancelRefresh();
    AuthStateMachine::Actions acts;
    m_sm.cancelAuthentication(acts);
    m_generation = m_sm.generation();
    runActions(acts);
}

void AccountSession::tick()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    maybeAutoRefresh();
}

// ---------------- transport 回调 ----------------

void AccountSession::onConnectResult(bool ok)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    AuthStateMachine::Actions acts;
    if (ok) {
        m_reconnect.reset();
        if (m_reauthenticateAfterReconnect) {
            m_reauthenticateAfterReconnect = false;
            m_sm.requestTokenLoginOnConnected(acts);
            m_generation = m_sm.generation();
        } else {
            m_sm.onConnected(acts);
        }
    } else {
        m_sm.onDisconnected(/*intentional=*/false, acts);
    }
    runActions(acts);
}

void AccountSession::onDisconnected(bool intentional)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // 刷新在途时断线：保留 request_id，重连后 beginRefresh() 复用同一 id（幂等，不误判复用）。
    if (m_sm.state() == AccountState::Refreshing) {
        m_tokens.suspendRefresh();
    } else if (!intentional && m_sm.state() == AccountState::Authenticated) {
        m_reauthenticateAfterReconnect = true;
    }
    AuthStateMachine::Actions acts;
    m_sm.onDisconnected(intentional, acts);
    runActions(acts);
}

void AccountSession::onLoginResult(int protoResult, const AuthResult& result,
                                   std::uint32_t requestGeneration)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (requestGeneration == 0) requestGeneration = m_generation;
    if (requestGeneration != m_sm.generation()) return;
    AuthStateMachine::Actions acts;
    if (protoResult == kResultSuccess) {
        // 落盘 token（原子）。落盘失败视为内部错误。
        TokenSession sess;
        sess.account = m_account;
        sess.accessToken.assign(result.accessToken);
        sess.refreshToken.assign(result.refreshToken);
        sess.accessExpireAt = result.accessExpireAt;
        sess.refreshExpireAt = result.refreshExpireAt;
        sess.sessionId = result.sessionId;
        if (!m_tokens.adopt(std::move(sess))) {
            m_sm.onAuthFailed(requestGeneration, AuthError::Internal, acts);
            runActions(acts);
            return;
        }
        m_sm.onAuthSucceeded(requestGeneration, result.userId, acts);
    } else {
        m_sm.onAuthFailed(requestGeneration, mapLoginError(protoResult), acts);
    }
    runActions(acts);
}

void AccountSession::onTokenLoginResult(int protoResult, std::int32_t userId,
                                        std::int64_t accessExpireAt,
                                        std::uint32_t requestGeneration)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (requestGeneration == 0) requestGeneration = m_generation;
    if (requestGeneration != m_sm.generation()) return;
    AuthStateMachine::Actions acts;
    if (protoResult == kResultSuccess) {
        // token 登录不返回新 token，仅刷新过期时间（若给出）
        m_sm.onAuthSucceeded(requestGeneration, userId, acts);
        (void)accessExpireAt;
    } else {
        // token 登录失败：access 可能已失效 → 尝试用 refresh 刷新，否则要求重新密码登录
        m_sm.onAuthFailed(requestGeneration, AuthError::TokenExpired, acts);
    }
    runActions(acts);
}

void AccountSession::onRefreshResult(int protoResult, const AuthResult& result,
                                     std::uint32_t requestGeneration)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (requestGeneration == 0) requestGeneration = m_generation;
    if (requestGeneration != m_sm.generation()) return;
    AuthStateMachine::Actions acts;
    if (protoResult == kResultSuccess) {
        SecureString acc(result.accessToken);
        SecureString ref(result.refreshToken);
        if (!m_tokens.rotate(std::move(acc), std::move(ref), result.accessExpireAt,
                             result.refreshExpireAt, result.sessionId)) {
            m_sm.onRefreshFailed(requestGeneration, AuthError::Internal, acts);
            runActions(acts);
            return;
        }
        m_sm.onRefreshSucceeded(requestGeneration, acts);
        const bool continueLogin = m_tokenLoginAfterRefresh;
        m_tokenLoginAfterRefresh = false;
        runActions(acts);
        if (continueLogin) {
            acts.clear();
            m_sm.requestTokenLoginOnConnected(acts);
            m_generation = m_sm.generation();
            runActions(acts);
        }
        return;
    } else {
        // 刷新失败：refresh 复用/吊销 → 清 family 并被动登出（revokeFamily 同时清 pending）
        m_tokens.revokeFamily();
        m_sm.onRefreshFailed(requestGeneration, AuthError::TokenRevoked, acts);
    }
    runActions(acts);
}

void AccountSession::onLogoutResult(int protoResult, std::uint32_t requestGeneration)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (requestGeneration != 0 && requestGeneration != m_sm.generation()) return;
    (void)protoResult; // 无论服务端结果如何，本地都清理并登出
    m_tokens.revokeFamily();
    AuthStateMachine::Actions acts;
    m_sm.onLogoutCompleted(acts);
    runActions(acts);
}

void AccountSession::onKicked(int reason)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    (void)reason;
    m_tokens.revokeFamily();
    AuthStateMachine::Actions acts;
    m_sm.onKicked(AuthError::KickedByOtherDevice, acts);
    runActions(acts);
}

void AccountSession::onReconnectFired()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // 退避到点：若仍允许重连则重新连接。连接成功后状态机会按 authMethod（此时为
    // AccessToken）自动触发 SendTokenLogin，恢复服务端会话。
    if (!m_reconnect.enabled()) return;
    m_transport.connect();
}

void AccountSession::maybeAutoRefresh()
{
    if (m_sm.state() != AccountState::Authenticated) return;
    if (m_tokens.accessNeedsRefresh() && !m_tokens.refreshInFlight()) {
        AuthStateMachine::Actions acts;
        m_sm.requestRefresh(acts);
        m_generation = m_sm.generation();
        runActions(acts);
    }
}

} // namespace account
} // namespace im
