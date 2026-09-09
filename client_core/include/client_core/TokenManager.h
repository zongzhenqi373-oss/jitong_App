// Token 管理（P5-T04）。
//
// 职责：
//   - 持有当前 access/refresh token 及其过期时间（epoch 秒）与 session_id；
//   - 判断 access_token 是否需要刷新（带提前量阈值，避免临界过期）；
//   - Refresh 的 Single-Flight：并发/重连场景下同一次刷新只发一次，request_id 复用并持久化，
//     使得"发出刷新→断线→重连"仍用同一个 request_id，服务端可幂等处理、不误判为复用攻击；
//   - 原子轮换：新 token 先落盘成功，再切换内存态；落盘失败则保持旧 token；
//   - 吊销：refresh 复用检测 / token 失效时，清空整个 token family（本地全部凭据作废）。
//
// 时间与持久化都通过接口注入，便于用 fake clock / 内存 KV 做确定性单测。

#ifndef CLIENT_CORE_TOKEN_MANAGER_H
#define CLIENT_CORE_TOKEN_MANAGER_H

#include "client_core/SecureString.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace im {
namespace account {

/** 单调/墙钟时间源（epoch 秒）。生产用系统时钟，测试用可控 fake clock。 */
class IClock {
public:
    virtual ~IClock() = default;
    virtual std::int64_t nowEpochSeconds() const = 0;
};

/** 键值持久化接口。生产由 Keystore-加密 KV 实现，测试用内存实现。 */
class ITokenStore {
public:
    using Puts = std::vector<std::pair<std::string, std::string>>;
    using Removes = std::vector<std::string>;

    virtual ~ITokenStore() = default;
    virtual bool get(const std::string& key, std::string& valueOut) const = 0;
    virtual bool put(const std::string& key, const std::string& value) = 0;
    virtual bool remove(const std::string& key) = 0;
    virtual bool clearAll() = 0;
    /** 全部变更成功，或存储保持原样。生产实现必须提供事务/版本化双槽语义。 */
    virtual bool applyAtomically(const Puts& puts, const Removes& removes) = 0;
};

/** 一次登录/刷新后得到的 token 会话。 */
struct TokenSession {
    SecureString accessToken;
    SecureString refreshToken;
    std::int64_t accessExpireAt = 0;   // epoch 秒
    std::int64_t refreshExpireAt = 0;  // epoch 秒
    std::string sessionId;
    std::string account;               // 归属账号（用于持久化命名空间）

    bool valid() const { return !accessToken.empty() && !refreshToken.empty(); }
};

class TokenManager {
public:
    // skewSeconds：判断过期时预留的提前量（默认 60s），避免"刚好到点"的竞态。
    TokenManager(std::shared_ptr<IClock> clock, std::shared_ptr<ITokenStore> store,
                 std::int64_t refreshSkewSeconds = 60);

    /** 冷启动时从持久化恢复；成功且未完全过期返回 true。 */
    bool loadFromStore(const std::string& account);

    /** 用一次登录结果建立会话，并原子落盘。落盘失败返回 false 且不改变内存态。 */
    bool adopt(TokenSession session);

    /** 一次刷新成功后轮换（保持 request_id 语义），原子落盘。 */
    bool rotate(SecureString newAccess, SecureString newRefresh, std::int64_t accessExpireAt,
                std::int64_t refreshExpireAt, const std::string& sessionId);

    bool hasSession() const { return m_session.valid(); }
    const std::string& sessionId() const { return m_session.sessionId; }
    const SecureString& accessToken() const { return m_session.accessToken; }
    const SecureString& refreshToken() const { return m_session.refreshToken; }
    std::int64_t accessExpireAt() const { return m_session.accessExpireAt; }

    /** access_token 是否需要刷新（已过期或将在 skew 内过期）。 */
    bool accessNeedsRefresh() const;
    /** access_token 是否仍可用（未过期，含 skew）。 */
    bool accessUsable() const;
    /** refresh_token 是否已过期（过期则只能重新密码登录）。 */
    bool refreshExpired() const;

    // ---- Refresh Single-Flight ----

    /**
     * 开始一次刷新：若已有进行中的刷新，返回其 request_id（复用，不新建）；否则生成并
     * 持久化一个新的 request_id 并标记 in-flight。返回用于 RefreshTokenRq 的 request_id。
     */
    /** 持久化失败返回空字符串，调用方不得发送刷新请求。 */
    std::string beginRefresh();
    bool refreshInFlight() const { return m_refreshInFlight; }
    /** 当前（可能是复用的）刷新 request_id。 */
    const std::string& currentRefreshRequestId() const { return m_refreshRequestId; }
    /** 刷新已形成明确终态，清除持久化的 request_id。失败时保持 in-flight 供重试。 */
    bool completeRefresh();
    /** 网络断开/超时：保留 request_id，但允许连接恢复后重发同一次刷新。 */
    void suspendRefresh();
    /** 登出/销毁：清除 pending；存储清理失败返回 false。 */
    bool cancelRefresh();

    /** 吊销：清空整个 token family（内存 + 持久化）。用于 refresh 复用/token 失效。 */
    /** 清空内存并尝试原子删除持久化 family；持久化删除失败返回 false。 */
    bool revokeFamily();

    // 供测试观察
    std::int64_t now() const { return m_clock->nowEpochSeconds(); }

private:
    std::string keyOf(const std::string& suffix) const;
    bool persist(bool clearPending); // 原子写入 session，可同时清除 pending refresh
    void clearMemory();

    std::shared_ptr<IClock> m_clock;
    std::shared_ptr<ITokenStore> m_store;
    std::int64_t m_refreshSkew;

    TokenSession m_session;
    bool m_refreshInFlight = false;
    std::string m_refreshRequestId;
    std::uint64_t m_requestSeq = 0;
};

} // namespace account
} // namespace im

#endif // CLIENT_CORE_TOKEN_MANAGER_H
