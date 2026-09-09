// 账号会话编排（P5 接线层）。
//
// AccountSession 是认证域的"厚内核"编排者：把 AuthStateMachine（决策）、TokenManager（凭据）、
// DeviceProofService（设备签名）、ReconnectPolicy（退避）、AuthRequestBuilder（请求构造）组装成
// 一个完整的账号生命周期驱动器。UI 只调用意图方法（startWithPassword / startWithToken /
// logout / cancel）并订阅 AccountEvent；连接/握手/自动登录/自动刷新/断线重连/被踢处理全部由
// 本类编排。
//
// 与网络解耦：真正的 socket/TLS/应用握手/协议收发由注入的 IAuthTransport 完成。生产用
// ClientCoreAuthTransport（适配 im::ClientCore），测试用 FakeAuthTransport，从而可对整条
// 认证链路做确定性单测（含竞态：登录进行中断线、刷新中被踢等）。
//
// 线程模型：所有方法约定在同一个逻辑线程（或由调用方串行化）调用——与 ClientCore 的
// IO 线程模型一致。本类不自带锁；如需跨线程由适配层负责 marshal。

#ifndef CLIENT_CORE_ACCOUNT_SESSION_H
#define CLIENT_CORE_ACCOUNT_SESSION_H

#include "client_core/AccountTypes.h"
#include "client_core/AuthRequestBuilder.h"
#include "client_core/AuthStateMachine.h"
#include "client_core/DeviceProofService.h"
#include "client_core/ReconnectPolicy.h"
#include "client_core/TokenManager.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace im {
namespace account {

/** 一次登录成功后服务端返回的 token 数据（由 transport 解析 LoginRs 后回填）。 */
struct AuthResult {
    std::int32_t userId = 0;
    std::string accessToken;
    std::string refreshToken;
    std::int64_t accessExpireAt = 0;
    std::int64_t refreshExpireAt = 0;
    std::string sessionId;
};

/**
 * 认证传输抽象：AccountSession 通过它驱动网络。所有方法都是"发起"，结果通过
 * IAuthTransportCallbacks 异步回调。实现方保证在同一逻辑线程回调。
 */
class IAuthTransport {
public:
    using Bytes = std::vector<unsigned char>;
    virtual ~IAuthTransport() = default;

    /** 发起连接（TCP/TLS/应用安全握手）。结果经 onConnectResult 回调。 */
    virtual void connect() = 0;
    /** 主动断开。 */
    virtual void disconnect() = 0;

    /** 应用安全通道 sessionId（握手成功后有效），用于设备证明签名。 */
    virtual Bytes appSessionId() const = 0;

    /** 发送密码登录（payload 由 AccountSession 用 ClientCore 现有方式构造或交给 transport）。 */
    virtual void sendPasswordLogin(const std::string& account, const std::string& password,
                                   std::uint32_t generation) = 0;
    /** 发送已构造好的 Token 登录 / 刷新 / 登出请求字节（AuthRequestBuilder 产出）。 */
    virtual void sendTokenLogin(const std::string& payload, std::uint32_t generation) = 0;
    virtual void sendRefresh(const std::string& payload, std::uint32_t generation) = 0;
    virtual void sendLogout(const std::string& payload, std::uint32_t generation) = 0;

    /** 安排一个一次性延时回调（用于退避重连）。delayMs<0 表示取消已安排的重连。 */
    virtual void scheduleReconnect(std::int64_t delayMs) = 0;
    virtual void cancelReconnect() = 0;
};

/** transport → AccountSession 的回调。 */
class IAuthTransportCallbacks {
public:
    virtual ~IAuthTransportCallbacks() = default;
    virtual void onConnectResult(bool ok) = 0;
    virtual void onDisconnected(bool intentional) = 0;
    virtual void onLoginResult(int protoResult, const AuthResult& result,
                               std::uint32_t requestGeneration = 0) = 0;
    virtual void onTokenLoginResult(int protoResult, std::int32_t userId,
                                    std::int64_t accessExpireAt,
                                    std::uint32_t requestGeneration = 0) = 0;
    virtual void onRefreshResult(int protoResult, const AuthResult& result,
                                 std::uint32_t requestGeneration = 0) = 0;
    virtual void onLogoutResult(int protoResult, std::uint32_t requestGeneration = 0) = 0;
    virtual void onKicked(int reason) = 0;
    virtual void onReconnectFired() = 0; // 退避定时到点
};

class AccountSession : public IAuthTransportCallbacks {
public:
    using EventSink = std::function<void(const AccountEvent&)>;

    AccountSession(IAuthTransport& transport, std::shared_ptr<IClock> clock,
                   std::shared_ptr<ITokenStore> store, const IP256Signer& signer,
                   std::string deviceId, std::shared_ptr<IJitter> jitter = nullptr);

    void setEventSink(EventSink sink)
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_sink = std::move(sink);
    }

    // ---- UI 意图 ----

    /** 密码登录（首次/换账号）。返回 operationId（0 表示被拒）。 */
    std::uint64_t startWithPassword(const std::string& account, const std::string& password);

    /** 冷启动自动登录：若持久化有未过期凭据则用 token 登录，否则返回 0（需密码登录）。 */
    std::uint64_t startWithSavedToken(const std::string& account);

    /** 登出。 */
    void logout(bool allDevices = false);

    /** 取消进行中的认证（连点/放弃）。 */
    void cancel();

    /**
     * 心跳/周期驱动钩子：由适配层在每次收到服务端数据或定时器触发时调用。
     * 若 access_token 将过期且已认证，会自动发起一次刷新（Refresh Single-Flight）。
     */
    void tick();

    // ---- 观测 ----
    AccountState accountState() const
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_sm.state();
    }
    const TokenManager& tokens() const { return m_tokens; }

    // ---- IAuthTransportCallbacks ----
    void onConnectResult(bool ok) override;
    void onDisconnected(bool intentional) override;
    void onLoginResult(int protoResult, const AuthResult& result,
                       std::uint32_t requestGeneration = 0) override;
    void onTokenLoginResult(int protoResult, std::int32_t userId,
                            std::int64_t accessExpireAt,
                            std::uint32_t requestGeneration = 0) override;
    void onRefreshResult(int protoResult, const AuthResult& result,
                         std::uint32_t requestGeneration = 0) override;
    void onLogoutResult(int protoResult, std::uint32_t requestGeneration = 0) override;
    void onKicked(int reason) override;
    void onReconnectFired() override;

private:
    // 执行状态机产出的动作序列
    void runActions(const AuthStateMachine::Actions& actions);
    void emit(const AccountEvent& e);
    void maybeAutoRefresh(); // access 将过期时自动发起刷新

    IAuthTransport& m_transport;
    std::shared_ptr<IClock> m_clock;
    AuthStateMachine m_sm;
    TokenManager m_tokens;
    DeviceProofService m_proof;
    AuthRequestBuilder m_reqBuilder;
    ReconnectPolicy m_reconnect;
    EventSink m_sink;

    std::string m_account;
    SecureString m_pendingPassword; // 仅密码登录在途时短暂持有并在使用后清零
    std::uint32_t m_generation = 0; // 快照 m_sm.generation()，回调时校验
    bool m_logoutAllDevices = false;
    bool m_tokenLoginAfterRefresh = false;
    bool m_reauthenticateAfterReconnect = false;
    mutable std::recursive_mutex m_mutex; // 串行化 JNI、网络、定时器入口；允许同步 transport 回调
};

} // namespace account
} // namespace im

#endif // CLIENT_CORE_ACCOUNT_SESSION_H
