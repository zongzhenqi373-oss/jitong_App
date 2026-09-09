// AuthStateMachine 单元测试（P5-T01/T02）。
// 纯逻辑、无 IO：逐一驱动状态机的意图/信号，断言状态迁移与产出的动作序列。

#include "client_core/AuthStateMachine.h"

#include <iostream>
#include <string>
#include <vector>

using namespace im::account;

namespace {
int g_failures = 0;

void check(bool cond, const std::string& name)
{
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << std::endl;
    if (!cond) ++g_failures;
}

bool hasAction(const AuthStateMachine::Actions& acts, ActionType t)
{
    for (const auto& a : acts) if (a.type == t) return true;
    return false;
}

int countEvent(const AuthStateMachine::Actions& acts, AccountEventType t)
{
    int n = 0;
    for (const auto& a : acts)
        if (a.type == ActionType::EmitEvent && a.event.type == t) ++n;
    return n;
}

// 一个便捷宏：清空动作缓冲
#define ACTS AuthStateMachine::Actions acts; acts.clear()

void testPasswordLoginHappyPath()
{
    std::cout << "[1] 密码登录正常流程" << std::endl;
    AuthStateMachine sm;
    check(sm.state() == AccountState::LoggedOut, "初始 LoggedOut");

    ACTS;
    const auto op = sm.requestPasswordLogin("alice", acts);
    check(op != 0, "分配了 operationId");
    check(sm.state() == AccountState::Authenticating, "→ Authenticating");
    check(hasAction(acts, ActionType::Connect), "产出 Connect 动作");

    acts.clear();
    sm.onConnected(acts);
    check(hasAction(acts, ActionType::SendPasswordLogin), "连接后发送登录");

    acts.clear();
    sm.onAuthSucceeded(sm.generation(), 1001, acts);
    check(sm.state() == AccountState::Authenticated, "→ Authenticated");
    check(countEvent(acts, AccountEventType::LoginSucceeded) == 1, "LoginSucceeded 事件");
}

void testSingleFlightSameAccount()
{
    std::cout << "[2] Single-Flight：同账号重复登录复用 operationId" << std::endl;
    AuthStateMachine sm;
    ACTS;
    const auto op1 = sm.requestPasswordLogin("alice", acts);
    acts.clear();
    const auto op2 = sm.requestPasswordLogin("alice", acts);
    check(op1 == op2, "复用同一 operationId");
    check(acts.empty(), "不产生新动作（不重复 Connect）");
}

void testSingleFlightDifferentAccount()
{
    std::cout << "[3] Single-Flight：异账号请求被拒绝" << std::endl;
    AuthStateMachine sm;
    ACTS;
    sm.requestPasswordLogin("alice", acts);
    acts.clear();
    const auto op2 = sm.requestPasswordLogin("bob", acts);
    check(op2 == 0, "返回 0 表示拒绝");
    bool rejected = false;
    for (const auto& a : acts)
        if (a.type == ActionType::RejectOperation && a.error == AuthError::OperationInProgress)
            rejected = true;
    check(rejected, "产出 RejectOperation(OperationInProgress)");
    check(sm.state() == AccountState::Authenticating, "原操作不被打断");
}

void testTokenLoginKeepsMethod()
{
    std::cout << "[4] Token 登录保持认证方式" << std::endl;
    AuthStateMachine sm;
    ACTS;
    sm.requestTokenLogin("alice", acts);
    acts.clear();
    sm.onConnected(acts);
    check(hasAction(acts, ActionType::SendTokenLogin), "连接后发送 TokenLoginRq");
    check(!hasAction(acts, ActionType::SendPasswordLogin), "不会误发密码登录");
}

void testLoginRejectedWhileRefreshing()
{
    std::cout << "[5] Refreshing 纳入认证 Single-Flight" << std::endl;
    AuthStateMachine sm;
    ACTS;
    sm.requestPasswordLogin("alice", acts);
    sm.onAuthSucceeded(sm.generation(), 1001, acts);
    acts.clear();
    const auto refreshOp = sm.requestRefresh(acts);
    const auto generation = sm.generation();
    acts.clear();
    check(sm.requestTokenLogin("alice", acts) == 0, "刷新中登录被拒绝");
    check(hasAction(acts, ActionType::RejectOperation), "产生 OperationInProgress");
    check(sm.currentOperationId() == refreshOp && sm.generation() == generation,
          "不覆盖刷新 operation/generation");
}

void testRefreshReconnectResends()
{
    std::cout << "[6] Refresh 断线重连后重发" << std::endl;
    AuthStateMachine sm;
    ACTS;
    sm.requestPasswordLogin("alice", acts);
    sm.onAuthSucceeded(sm.generation(), 1001, acts);
    acts.clear();
    sm.requestRefresh(acts);
    acts.clear();
    sm.onDisconnected(false, acts);
    check(hasAction(acts, ActionType::ScheduleReconnect), "断线安排重连");
    acts.clear();
    sm.onConnected(acts);
    check(hasAction(acts, ActionType::SendRefresh), "安全连接恢复后重发 Refresh");
}

void testAuthFailed()
{
    std::cout << "[4] 认证失败 → LoggedOut 并断开" << std::endl;
    AuthStateMachine sm;
    ACTS;
    sm.requestPasswordLogin("alice", acts);
    acts.clear();
    sm.onAuthFailed(sm.generation(), AuthError::InvalidCredentials, acts);
    check(sm.state() == AccountState::LoggedOut, "→ LoggedOut");
    check(countEvent(acts, AccountEventType::LoginFailed) == 1, "LoginFailed 事件");
    check(hasAction(acts, ActionType::Disconnect), "产出 Disconnect");
}

void testKickedNoReconnect()
{
    std::cout << "[5] 被踢 → LoggedOutKicked，禁止自动重连" << std::endl;
    AuthStateMachine sm;
    ACTS;
    sm.requestPasswordLogin("alice", acts);
    sm.onConnected(acts);
    sm.onAuthSucceeded(sm.generation(), 1001, acts);
    acts.clear();
    sm.onKicked(AuthError::KickedByOtherDevice, acts);
    check(sm.state() == AccountState::LoggedOutKicked, "→ LoggedOutKicked");
    check(!sm.autoReconnectAllowed(), "autoReconnectAllowed=false");
    check(countEvent(acts, AccountEventType::Kicked) == 1, "Kicked 事件");

    acts.clear();
    sm.onDisconnected(/*intentional=*/false, acts);
    check(!hasAction(acts, ActionType::ScheduleReconnect), "被踢后断线不安排重连");
}

void testReconnectOnDrop()
{
    std::cout << "[6] 已认证态断线 → 安排退避重连" << std::endl;
    AuthStateMachine sm;
    ACTS;
    sm.requestPasswordLogin("alice", acts);
    sm.onConnected(acts);
    sm.onAuthSucceeded(sm.generation(), 1001, acts);
    acts.clear();
    sm.onDisconnected(/*intentional=*/false, acts);
    check(hasAction(acts, ActionType::ScheduleReconnect), "产出 ScheduleReconnect");
    check(sm.state() == AccountState::Authenticated, "账号态保持 Authenticated");

    acts.clear();
    sm.onDisconnected(/*intentional=*/true, acts);
    check(!hasAction(acts, ActionType::ScheduleReconnect), "主动断开不重连");
}

void testRefreshHappyAndRevoke()
{
    std::cout << "[7] 刷新成功 / 吊销转 LoggedOutKicked" << std::endl;
    {
        AuthStateMachine sm;
        ACTS;
        sm.requestPasswordLogin("alice", acts);
        sm.onConnected(acts);
        sm.onAuthSucceeded(sm.generation(), 1001, acts);
        acts.clear();
        const auto op = sm.requestRefresh(acts);
        check(op != 0 && sm.state() == AccountState::Refreshing, "→ Refreshing");
        check(hasAction(acts, ActionType::SendRefresh), "产出 SendRefresh");
        acts.clear();
        sm.onRefreshSucceeded(sm.generation(), acts);
        check(sm.state() == AccountState::Authenticated, "刷新成功回到 Authenticated");
        check(countEvent(acts, AccountEventType::RefreshSucceeded) == 1, "RefreshSucceeded 事件");
    }
    {
        AuthStateMachine sm;
        ACTS;
        sm.requestPasswordLogin("alice", acts);
        sm.onConnected(acts);
        sm.onAuthSucceeded(sm.generation(), 1001, acts);
        acts.clear();
        sm.requestRefresh(acts);
        acts.clear();
        sm.onRefreshFailed(sm.generation(), AuthError::TokenRevoked, acts);
        check(sm.state() == AccountState::LoggedOutKicked, "吊销 → LoggedOutKicked");
        check(!sm.autoReconnectAllowed(), "禁止自动重连");
        check(countEvent(acts, AccountEventType::Kicked) == 1, "吊销产出 Kicked 事件");
    }
}

void testStaleGenerationDropped()
{
    std::cout << "[8] generation 失配的迟到结果被丢弃" << std::endl;
    AuthStateMachine sm;
    ACTS;
    sm.requestPasswordLogin("alice", acts);
    const auto staleGen = sm.generation();
    // 模拟"重新发起了一次新登录"使 generation 前进
    // 先失败回到 LoggedOut，再登录 → generation++
    acts.clear();
    sm.onAuthFailed(staleGen, AuthError::NetworkUnreachable, acts);
    acts.clear();
    sm.requestPasswordLogin("alice", acts);
    check(sm.generation() != staleGen, "generation 已前进");

    acts.clear();
    sm.onAuthSucceeded(staleGen, 1001, acts); // 用旧 generation
    check(sm.state() == AccountState::Authenticating, "迟到成功被丢弃，仍在 Authenticating");
    check(countEvent(acts, AccountEventType::LoginSucceeded) == 0, "无 LoginSucceeded");

    acts.clear();
    sm.onAuthFailed(staleGen, AuthError::InvalidCredentials, acts);
    check(acts.empty(), "迟到失败同样静默丢弃，不污染当前 operation");
}

void testLogout()
{
    std::cout << "[9] 登出" << std::endl;
    AuthStateMachine sm;
    ACTS;
    sm.requestPasswordLogin("alice", acts);
    sm.onConnected(acts);
    sm.onAuthSucceeded(sm.generation(), 1001, acts);
    acts.clear();
    sm.requestLogout(acts);
    check(hasAction(acts, ActionType::SendLogout), "产出 SendLogout");
    acts.clear();
    sm.onLogoutCompleted(acts);
    check(sm.state() == AccountState::LoggedOut, "→ LoggedOut");
    check(!sm.autoReconnectAllowed(), "登出后禁止自动重连");
    check(countEvent(acts, AccountEventType::LoggedOut) == 1, "LoggedOut 事件");
}

} // namespace

int main()
{
    std::cout << "=== test_auth_state_machine ===" << std::endl;
    testPasswordLoginHappyPath();
    testSingleFlightSameAccount();
    testSingleFlightDifferentAccount();
    testTokenLoginKeepsMethod();
    testLoginRejectedWhileRefreshing();
    testRefreshReconnectResends();
    testAuthFailed();
    testKickedNoReconnect();
    testReconnectOnDrop();
    testRefreshHappyAndRevoke();
    testStaleGenerationDropped();
    testLogout();

    if (g_failures == 0) {
        std::cout << "test_auth_state_machine PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_auth_state_machine FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
