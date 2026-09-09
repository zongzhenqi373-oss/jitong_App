// 账号认证域的核心类型（P5）。
//
// 这些类型描述"客户端账号会话"的可观察状态、内核向上层抛出的事件、以及各类错误码。
// 全部为纯数据/枚举，不含任何 IO 或线程，便于在单元测试里以确定性方式驱动状态机。
//
// 设计原则（对齐 QQNT「瘦 UI / 厚内核」）：
//   - UI 只消费 AccountState / ConnectionState 和 AccountEvent，不自行判断 token 是否过期、
//     不自行决定何时刷新或重连——这些都由 C++ 内核编排。
//   - 每个由 UI 发起的认证操作都带一个单调递增的 operationId，用于把异步结果对回请求、
//     并实现登录/刷新的 Single-Flight（同一账号重复点击复用同一 operationId）。

#ifndef CLIENT_CORE_ACCOUNT_TYPES_H
#define CLIENT_CORE_ACCOUNT_TYPES_H

#include <cstdint>
#include <string>

namespace im {
namespace account {

/** 账号级状态（与"连接"正交：连接可断开重连，但账号可能仍处于已认证态待恢复）。 */
enum class AccountState {
    LoggedOut,        // 无有效凭据，需要密码登录
    Authenticating,   // 正在进行密码登录 / Token 登录（首次建立会话）
    Authenticated,    // 已认证，拥有有效会话（可能连接暂断、等待恢复）
    Refreshing,       // access_token 过期或将过期，正在刷新
    LoggedOutKicked,  // 被服务端踢下线 / Token 失效 / refresh 复用检测 —— 禁止自动重连
};

/** 连接级状态（传输 + 应用安全通道）。 */
enum class ConnectionState {
    Disconnected,   // 未连接
    Connecting,     // TCP/TLS/应用握手进行中
    Connected,      // 握手完成，安全通道就绪（尚未认证）
    Reconnecting,   // 断线后按退避策略等待/重试中
};

/** 认证域错误码（覆盖 UI 需要区分展示/处理的所有情形）。 */
enum class AuthError {
    None = 0,
    NetworkUnreachable,      // 连不上 / TLS 握手失败
    InvalidCredentials,      // 密码错误 / 账号不存在
    TokenExpired,            // access/refresh token 已过期
    TokenRevoked,            // token family 被吊销（含 refresh 复用检测）
    DeviceProofFailed,       // 设备签名生成/校验失败
    KickedByOtherDevice,     // 被同账号其它设备登录踢下线
    OperationInProgress,     // 已有其它账号的认证操作在进行（Single-Flight 拒绝）
    OperationSuperseded,     // 本操作被更新的操作取代（generation 失配）
    ServerRejected,          // 服务端明确拒绝（其它业务原因）
    Internal,                // 内核内部错误
};

const char* toString(AccountState s);
const char* toString(ConnectionState s);
const char* toString(AuthError e);

/** 内核向上层（UI）投递的事件类型。 */
enum class AccountEventType {
    StateChanged,       // AccountState 变化
    ConnectionChanged,  // ConnectionState 变化
    LoginSucceeded,     // 一次登录操作成功（operationId 对应）
    LoginFailed,        // 一次登录操作失败（error 有效）
    RefreshSucceeded,   // token 刷新成功
    RefreshFailed,      // token 刷新失败
    Kicked,             // 被踢下线
    LoggedOut,          // 主动登出完成
};

/** 事件负载：一个扁平结构，避免 variant，方便跨 JNI/FFI 序列化。 */
struct AccountEvent {
    AccountEventType type;
    std::uint64_t operationId = 0;   // 关联的操作（StateChanged 等无操作时为 0）
    AccountState accountState = AccountState::LoggedOut;
    ConnectionState connectionState = ConnectionState::Disconnected;
    AuthError error = AuthError::None;
    std::int32_t userId = 0;         // 登录成功后有效
    std::string message;             // 可选的人类可读补充信息
};

} // namespace account
} // namespace im

#endif // CLIENT_CORE_ACCOUNT_TYPES_H
