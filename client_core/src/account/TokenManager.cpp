#include "client_core/TokenManager.h"

namespace im {
namespace account {

namespace {
constexpr const char* kAccess = "access";
constexpr const char* kRefresh = "refresh";
constexpr const char* kAccessExp = "access_exp";
constexpr const char* kRefreshExp = "refresh_exp";
constexpr const char* kSession = "session_id";
constexpr const char* kRefreshReqId = "refresh_request_id";

bool parsePositiveInt(const std::string& s, std::int64_t& out)
{
    try {
        std::size_t consumed = 0;
        const auto value = static_cast<std::int64_t>(std::stoll(s, &consumed));
        if (consumed != s.size() || value <= 0) return false;
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace

TokenManager::TokenManager(std::shared_ptr<IClock> clock, std::shared_ptr<ITokenStore> store,
                           std::int64_t refreshSkewSeconds)
    : m_clock(std::move(clock)), m_store(std::move(store)), m_refreshSkew(refreshSkewSeconds)
{
}

std::string TokenManager::keyOf(const std::string& suffix) const
{
    // 按账号命名空间隔离，避免多账号互相覆盖
    return "tok:" + m_session.account + ":" + suffix;
}

bool TokenManager::loadFromStore(const std::string& account)
{
    m_session = TokenSession{};
    m_session.account = account;

    std::string access, refresh, accExp, refExp, sess;
    const bool ok = m_store->get(keyOf(kAccess), access) &&
                    m_store->get(keyOf(kRefresh), refresh) &&
                    m_store->get(keyOf(kAccessExp), accExp) &&
                    m_store->get(keyOf(kRefreshExp), refExp) &&
                    m_store->get(keyOf(kSession), sess);
    if (!ok) {
        // 无记录和半写/损坏记录都归一为无凭据，并清理可能残留的其它字段。
        revokeFamily();
        return false;
    }
    std::int64_t accessExpireAt = 0;
    std::int64_t refreshExpireAt = 0;
    if (access.empty() || refresh.empty() || sess.empty() ||
        !parsePositiveInt(accExp, accessExpireAt) ||
        !parsePositiveInt(refExp, refreshExpireAt)) {
        revokeFamily();
        return false;
    }
    m_session.accessToken.assign(std::move(access));
    m_session.refreshToken.assign(std::move(refresh));
    m_session.accessExpireAt = accessExpireAt;
    m_session.refreshExpireAt = refreshExpireAt;
    m_session.sessionId = std::move(sess);

    // 恢复可能残留的 refresh request_id（支持"发出刷新→崩溃/重启"后仍复用同一 id）
    std::string reqId;
    if (m_store->get(keyOf(kRefreshReqId), reqId) && !reqId.empty()) {
        m_refreshRequestId = reqId;
        m_refreshInFlight = true;
    }

    // refresh 已过期则视为无有效会话
    if (refreshExpired()) {
        revokeFamily();
        return false;
    }
    return m_session.valid();
}

bool TokenManager::persist(bool clearPending)
{
    ITokenStore::Puts puts = {
        {keyOf(kAccess), m_session.accessToken.str()},
        {keyOf(kRefresh), m_session.refreshToken.str()},
        {keyOf(kAccessExp), std::to_string(m_session.accessExpireAt)},
        {keyOf(kRefreshExp), std::to_string(m_session.refreshExpireAt)},
        {keyOf(kSession), m_session.sessionId},
    };
    ITokenStore::Removes removes;
    if (clearPending) removes.push_back(keyOf(kRefreshReqId));
    return m_store->applyAtomically(puts, removes);
}

bool TokenManager::adopt(TokenSession session)
{
    // 先保存旧态，落盘成功才切换（原子轮换）
    TokenSession previous = std::move(m_session);
    m_session = std::move(session);
    if (!persist(/*clearPending=*/true)) {
        m_session = std::move(previous); // 回滚
        return false;
    }
    m_refreshInFlight = false;
    m_refreshRequestId.clear();
    return true;
}

bool TokenManager::rotate(SecureString newAccess, SecureString newRefresh,
                          std::int64_t accessExpireAt, std::int64_t refreshExpireAt,
                          const std::string& sessionId)
{
    TokenSession previous = m_session; // 拷贝旧态用于回滚
    m_session.accessToken = std::move(newAccess);
    m_session.refreshToken = std::move(newRefresh);
    m_session.accessExpireAt = accessExpireAt;
    m_session.refreshExpireAt = refreshExpireAt;
    if (!sessionId.empty()) m_session.sessionId = sessionId;
    if (!persist(/*clearPending=*/true)) {
        m_session = std::move(previous);
        return false;
    }
    m_refreshInFlight = false;
    m_refreshRequestId.clear();
    return true;
}

bool TokenManager::accessNeedsRefresh() const
{
    if (!m_session.valid()) return false;
    return m_clock->nowEpochSeconds() + m_refreshSkew >= m_session.accessExpireAt;
}

bool TokenManager::accessUsable() const
{
    if (!m_session.valid()) return false;
    return m_clock->nowEpochSeconds() + m_refreshSkew < m_session.accessExpireAt;
}

bool TokenManager::refreshExpired() const
{
    if (m_session.refreshExpireAt <= 0) return true;
    return m_clock->nowEpochSeconds() >= m_session.refreshExpireAt;
}

std::string TokenManager::beginRefresh()
{
    if (m_refreshInFlight && !m_refreshRequestId.empty()) {
        return m_refreshRequestId; // 复用：Single-Flight
    }
    // 生成新的 request_id：account + session + 单调序号 + 时间，保证跨重启唯一且稳定
    const std::string requestId = "rf-" + m_session.account + "-" + m_session.sessionId + "-" +
                         std::to_string(m_requestSeq + 1) + "-" +
                         std::to_string(m_clock->nowEpochSeconds());
    if (!m_store->applyAtomically({{keyOf(kRefreshReqId), requestId}}, {})) return {};
    ++m_requestSeq;
    m_refreshRequestId = requestId;
    m_refreshInFlight = true;
    return m_refreshRequestId;
}

bool TokenManager::completeRefresh()
{
    if (!m_store->applyAtomically({}, {keyOf(kRefreshReqId)})) return false;
    m_refreshInFlight = false;
    m_refreshRequestId.clear();
    return true;
}

void TokenManager::suspendRefresh()
{
    // pending requestId 必须保留；重连后的 beginRefresh() 会复用它。
}

bool TokenManager::cancelRefresh()
{
    return completeRefresh();
}

bool TokenManager::revokeFamily()
{
    // 清空该账号下所有持久化凭据 + request_id
    const bool removed = m_store->applyAtomically(
        {}, {keyOf(kAccess), keyOf(kRefresh), keyOf(kAccessExp), keyOf(kRefreshExp),
             keyOf(kSession), keyOf(kRefreshReqId)});
    clearMemory();
    return removed;
}

void TokenManager::clearMemory()
{
    const std::string account = m_session.account;
    m_session = TokenSession{};
    m_session.account = account;
    m_refreshInFlight = false;
    m_refreshRequestId.clear();
}

} // namespace account
} // namespace im
