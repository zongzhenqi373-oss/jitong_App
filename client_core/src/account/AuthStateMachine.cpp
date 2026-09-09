#include "client_core/AuthStateMachine.h"

namespace im {
namespace account {

namespace {
Action makeEvent(AccountEventType type, std::uint64_t opId, AccountState acc, AuthError err,
                 std::int32_t userId)
{
    Action a;
    a.type = ActionType::EmitEvent;
    a.operationId = opId;
    a.error = err;
    a.event.type = type;
    a.event.operationId = opId;
    a.event.accountState = acc;
    a.event.error = err;
    a.event.userId = userId;
    return a;
}
} // namespace

void AuthStateMachine::emit(AccountEventType type, std::uint64_t opId, AuthError error,
                            std::int32_t userId, Actions& out)
{
    out.push_back(makeEvent(type, opId, m_state, error, userId));
}

void AuthStateMachine::transitionTo(AccountState next, Actions& out)
{
    if (next == m_state) return;
    m_state = next;
    emit(AccountEventType::StateChanged, m_operationId, AuthError::None, 0, out);
}

// ---------------- 意图 ----------------

std::uint64_t AuthStateMachine::requestPasswordLogin(const std::string& account, Actions& out)
{
    return requestLogin(account, AuthMethod::Password, out);
}

std::uint64_t AuthStateMachine::requestTokenLogin(const std::string& account, Actions& out)
{
    return requestLogin(account, AuthMethod::AccessToken, out);
}

std::uint64_t AuthStateMachine::requestLogin(const std::string& account, AuthMethod method,
                                             Actions& out)
{
    if (authenticationInFlight()) {
        if (m_state == AccountState::Authenticating && account == m_account &&
            method == m_authMethod) {
            return m_operationId;
        }
        Action a;
        a.type = ActionType::RejectOperation;
        a.error = AuthError::OperationInProgress;
        out.push_back(a);
        return 0;
    }
    if (m_state == AccountState::Authenticated && account == m_account &&
        method == AuthMethod::AccessToken) {
        return m_operationId;
    }

    m_account = account;
    m_authMethod = method;
    m_operationId = m_nextOperationId++;
    ++m_generation;
    m_autoReconnectAllowed = true;
    transitionTo(AccountState::Authenticating, out);

    Action connect;
    connect.type = ActionType::Connect;
    connect.operationId = m_operationId;
    out.push_back(connect);
    return m_operationId;
}

std::uint64_t AuthStateMachine::requestRefresh(Actions& out)
{
    // 只有在已认证态才允许刷新；刷新中重复请求复用当前操作。
    if (m_state == AccountState::Refreshing) {
        return m_operationId;
    }
    if (m_state != AccountState::Authenticated) {
        Action a;
        a.type = ActionType::RejectOperation;
        a.error = AuthError::OperationInProgress;
        out.push_back(a);
        return 0;
    }
    m_stateBeforeRefresh = m_state;
    m_operationId = m_nextOperationId++;
    ++m_generation;
    transitionTo(AccountState::Refreshing, out);

    Action refresh;
    refresh.type = ActionType::SendRefresh;
    refresh.operationId = m_operationId;
    out.push_back(refresh);
    return m_operationId;
}

std::uint64_t AuthStateMachine::requestRefreshForLogin(const std::string& account, Actions& out)
{
    if (authenticationInFlight()) {
        Action rejected;
        rejected.type = ActionType::RejectOperation;
        rejected.error = AuthError::OperationInProgress;
        out.push_back(rejected);
        return 0;
    }
    m_account = account;
    m_authMethod = AuthMethod::AccessToken;
    m_operationId = m_nextOperationId++;
    ++m_generation;
    m_autoReconnectAllowed = true;
    transitionTo(AccountState::Refreshing, out);
    // 冷启动尚无安全连接，先建连；onConnected() 会按 Refreshing 状态发送刷新。
    Action connect;
    connect.type = ActionType::Connect;
    connect.operationId = m_operationId;
    out.push_back(connect);
    return m_operationId;
}

std::uint64_t AuthStateMachine::requestTokenLoginOnConnected(Actions& out)
{
    if (m_state != AccountState::Authenticated) return 0;
    m_authMethod = AuthMethod::AccessToken;
    m_operationId = m_nextOperationId++;
    ++m_generation;
    transitionTo(AccountState::Authenticating, out);
    Action login;
    login.type = ActionType::SendTokenLogin;
    login.operationId = m_operationId;
    out.push_back(login);
    return m_operationId;
}

std::uint64_t AuthStateMachine::requestLogout(Actions& out)
{
    ++m_generation; // 使登录/刷新在途响应立即失效
    m_operationId = m_nextOperationId++;
    m_autoReconnectAllowed = false;
    Action cancel;
    cancel.type = ActionType::CancelReconnect;
    out.push_back(cancel);

    Action logout;
    logout.type = ActionType::SendLogout;
    logout.operationId = m_operationId;
    out.push_back(logout);
    return m_operationId;
}

void AuthStateMachine::cancelAuthentication(Actions& out)
{
    ++m_generation;
    m_operationId = 0;
    m_authMethod = AuthMethod::None;
    m_autoReconnectAllowed = false;
    Action cancel;
    cancel.type = ActionType::CancelReconnect;
    out.push_back(cancel);
    transitionTo(AccountState::LoggedOut, out);
    Action disconnect;
    disconnect.type = ActionType::Disconnect;
    out.push_back(disconnect);
}

// ---------------- 信号 ----------------

void AuthStateMachine::onConnected(Actions& out)
{
    // 连接就绪后，若正处于认证态，触发对应的登录发送。
    if (m_state == AccountState::Authenticating) {
        Action a;
        a.type = m_authMethod == AuthMethod::AccessToken ? ActionType::SendTokenLogin
                                                         : ActionType::SendPasswordLogin;
        a.operationId = m_operationId;
        out.push_back(a);
    } else if (m_state == AccountState::Refreshing) {
        Action a;
        a.type = ActionType::SendRefresh;
        a.operationId = m_operationId;
        out.push_back(a);
    }
}

void AuthStateMachine::onDisconnected(bool intentional, Actions& out)
{
    if (intentional) {
        return; // 主动断开，不触发重连
    }
    // 被踢/登出后禁止自动重连
    if (!m_autoReconnectAllowed || m_state == AccountState::LoggedOutKicked) {
        return;
    }
    // 已认证但连接断了：安排带退避的重连（保持账号态不变，等恢复）
    if (m_state == AccountState::Authenticated || m_state == AccountState::Refreshing) {
        Action a;
        a.type = ActionType::ScheduleReconnect;
        a.operationId = m_operationId;
        out.push_back(a);
    } else if (m_state == AccountState::Authenticating) {
        // 认证过程中断线 → 本次登录失败
        emit(AccountEventType::LoginFailed, m_operationId, AuthError::NetworkUnreachable, 0, out);
        transitionTo(AccountState::LoggedOut, out);
    }
}

void AuthStateMachine::onAuthSucceeded(std::uint32_t generation, std::int32_t userId, Actions& out)
{
    if (isStaleGeneration(generation)) {
        return; // 迟到的旧结果，丢弃
    }
    if (m_state != AccountState::Authenticating) {
        return;
    }
    transitionTo(AccountState::Authenticated, out);
    emit(AccountEventType::LoginSucceeded, m_operationId, AuthError::None, userId, out);
}

void AuthStateMachine::onAuthFailed(std::uint32_t generation, AuthError error, Actions& out)
{
    if (isStaleGeneration(generation)) {
        return; // 旧 operation 的失败不能污染当前 operation
    }
    if (m_state != AccountState::Authenticating) {
        return;
    }
    emit(AccountEventType::LoginFailed, m_operationId, error, 0, out);
    transitionTo(AccountState::LoggedOut, out);
    // 认证失败后主动断开，不进入重连
    Action disc;
    disc.type = ActionType::Disconnect;
    out.push_back(disc);
}

void AuthStateMachine::onRefreshSucceeded(std::uint32_t generation, Actions& out)
{
    if (isStaleGeneration(generation)) {
        return;
    }
    if (m_state != AccountState::Refreshing) {
        return;
    }
    transitionTo(AccountState::Authenticated, out);
    emit(AccountEventType::RefreshSucceeded, m_operationId, AuthError::None, 0, out);
}

void AuthStateMachine::onRefreshFailed(std::uint32_t generation, AuthError error, Actions& out)
{
    if (isStaleGeneration(generation)) {
        return;
    }
    if (m_state != AccountState::Refreshing) {
        return;
    }
    emit(AccountEventType::RefreshFailed, m_operationId, error, 0, out);
    // refresh 失败若是 token 吊销/复用 → 被动登出，禁止重连
    if (error == AuthError::TokenRevoked || error == AuthError::TokenExpired) {
        m_autoReconnectAllowed = false;
        transitionTo(AccountState::LoggedOutKicked, out);
        emit(AccountEventType::Kicked, m_operationId, error, 0, out);
    } else {
        // 其它错误（如网络）：回到已认证态，靠重连/下次刷新恢复
        transitionTo(AccountState::Authenticated, out);
    }
}

void AuthStateMachine::onKicked(AuthError reason, Actions& out)
{
    m_autoReconnectAllowed = false;
    Action cancel;
    cancel.type = ActionType::CancelReconnect;
    out.push_back(cancel);
    transitionTo(AccountState::LoggedOutKicked, out);
    emit(AccountEventType::Kicked, m_operationId, reason, 0, out);
    Action disc;
    disc.type = ActionType::Disconnect;
    out.push_back(disc);
}

void AuthStateMachine::onLogoutCompleted(Actions& out)
{
    m_autoReconnectAllowed = false;
    m_account.clear();
    m_authMethod = AuthMethod::None;
    transitionTo(AccountState::LoggedOut, out);
    emit(AccountEventType::LoggedOut, m_operationId, AuthError::None, 0, out);
    m_operationId = 0;
}

} // namespace account
} // namespace im
