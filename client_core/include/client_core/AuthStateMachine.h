// 认证状态机（P5-T01/T02）。
//
// 纯逻辑、无 IO、无线程、无时间依赖：给定当前状态 + 一个"意图/信号"，产出**下一状态**和
// **内核应执行的动作列表**。真正的网络收发、加密、定时器由 AccountSession 依据 Action 执行。
// 这样把"决策"与"副作用"彻底分离，状态机可被完整地确定性单测。
//
// Single-Flight 语义（对齐 QQNT / Kotlin 现状基线）：
//   - 同一账号在 Authenticating 期间重复请求登录 → 复用同一 operationId，不产生新动作。
//   - 不同账号在有操作进行时请求登录 → 拒绝，产出 RejectOperation(OperationInProgress)。
//   - 被踢 / token 吊销后进入 LoggedOutKicked → 禁止自动重连，必须显式重新登录。
//   - generation：每次开始一个新的登录/刷新操作时 generation 自增；迟到的旧结果用
//     generation 校验丢弃（OperationSuperseded）。

#ifndef CLIENT_CORE_AUTH_STATE_MACHINE_H
#define CLIENT_CORE_AUTH_STATE_MACHINE_H

#include "client_core/AccountTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace im {
namespace account {

enum class AuthMethod {
    None,
    Password,
    AccessToken,
};

/** 状态机要求内核执行的动作类型。 */
enum class ActionType {
    None,
    Connect,              // 建立连接（TCP/TLS/应用握手）
    SendPasswordLogin,    // 发送 LoginRq（凭据在 pendingCredential）
    SendTokenLogin,       // 发送 TokenLoginRq（用当前 access_token）
    SendRefresh,          // 发送 RefreshTokenRq（用当前 refresh_token）
    SendLogout,           // 发送 LogoutRq
    Disconnect,           // 主动断开
    ScheduleReconnect,    // 启动带退避的自动重连
    CancelReconnect,      // 取消待重连
    EmitEvent,            // 向上层投递一个事件（见 event 字段）
    RejectOperation,      // 拒绝一次操作（error 有效）
};

struct Action {
    ActionType type = ActionType::None;
    std::uint64_t operationId = 0;
    AuthError error = AuthError::None;
    AccountEvent event;   // 当 type == EmitEvent 时有效
};

/** 触发状态机的输入。分为两类：来自 UI 的意图、来自内核的信号。 */
class AuthStateMachine {
public:
    using Actions = std::vector<Action>;

    AuthStateMachine() = default;

    AccountState state() const { return m_state; }
    std::uint64_t currentOperationId() const { return m_operationId; }
    std::uint32_t generation() const { return m_generation; }
    bool autoReconnectAllowed() const { return m_autoReconnectAllowed; }
    AuthMethod authMethod() const { return m_authMethod; }

    // ---- 意图（UI → 内核）----

    /**
     * 请求用密码登录。account 为账号标识（用于 Single-Flight 判定是否"同一账号"）。
     * 返回本次操作对应的 operationId（可能是复用的旧 id）。产出动作写入 out。
     */
    std::uint64_t requestPasswordLogin(const std::string& account, Actions& out);

    /** 请求用已保存的 token 登录（冷启动自动登录 / 断线恢复）。 */
    std::uint64_t requestTokenLogin(const std::string& account, Actions& out);

    /** 冷启动发现 access 不可用但 refresh 有效：先刷新，再继续 Token 登录。 */
    std::uint64_t requestRefreshForLogin(const std::string& account, Actions& out);

    /** Refresh 成功后在当前安全连接上继续 Token 登录，不重复 Connect。 */
    std::uint64_t requestTokenLoginOnConnected(Actions& out);

    /** 请求刷新 token（通常由内核在 access 将过期时自触发，也可外部触发）。 */
    std::uint64_t requestRefresh(Actions& out);

    /** 请求登出。 */
    std::uint64_t requestLogout(Actions& out);

    /** 取消当前认证并使所有旧 generation 回调失效。 */
    void cancelAuthentication(Actions& out);

    // ---- 信号（内核 → 状态机）----

    void onConnected(Actions& out);
    void onDisconnected(bool intentional, Actions& out);

    /** 一次认证（密码/Token 登录）成功。generation 用于丢弃迟到的旧结果。 */
    void onAuthSucceeded(std::uint32_t generation, std::int32_t userId, Actions& out);
    void onAuthFailed(std::uint32_t generation, AuthError error, Actions& out);

    void onRefreshSucceeded(std::uint32_t generation, Actions& out);
    void onRefreshFailed(std::uint32_t generation, AuthError error, Actions& out);

    /** 被服务端踢下线 / token 吊销 —— 进入 LoggedOutKicked，禁止自动重连。 */
    void onKicked(AuthError reason, Actions& out);

    void onLogoutCompleted(Actions& out);

private:
    void transitionTo(AccountState next, Actions& out);
    void emit(AccountEventType type, std::uint64_t opId, AuthError error, std::int32_t userId,
              Actions& out);
    bool isStaleGeneration(std::uint32_t generation) const { return generation != m_generation; }
    bool authenticationInFlight() const
    {
        return m_state == AccountState::Authenticating || m_state == AccountState::Refreshing;
    }
    std::uint64_t requestLogin(const std::string& account, AuthMethod method, Actions& out);

    AccountState m_state = AccountState::LoggedOut;
    std::uint64_t m_operationId = 0;   // 当前进行中的操作 id（0 表示无）
    std::uint64_t m_nextOperationId = 1;
    std::uint32_t m_generation = 0;    // 每开始一个新认证/刷新操作自增
    std::string m_account;             // 当前操作/会话绑定的账号
    AuthMethod m_authMethod = AuthMethod::None;
    bool m_autoReconnectAllowed = true;
    AccountState m_stateBeforeRefresh = AccountState::Authenticated;
};

} // namespace account
} // namespace im

#endif // CLIENT_CORE_AUTH_STATE_MACHINE_H
