// TokenManager 单元测试（P5-T04）。
// 用 fake clock + 内存 KV 做确定性验证：有效期判断、Refresh Single-Flight、request_id 持久化、
// 原子轮换（含落盘失败回滚）、family 吊销。

#include "client_core/TokenManager.h"

#include <iostream>
#include <map>
#include <memory>
#include <string>

using namespace im::account;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& name)
{
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << std::endl;
    if (!cond) ++g_failures;
}

class FakeClock : public IClock {
public:
    std::int64_t t = 1000000;
    std::int64_t nowEpochSeconds() const override { return t; }
};

class MemStore : public ITokenStore {
public:
    std::map<std::string, std::string> kv;
    bool failWrites = false; // 用于测试原子回滚
    bool failAtomic = false;
    bool get(const std::string& k, std::string& v) const override
    {
        auto it = kv.find(k);
        if (it == kv.end()) return false;
        v = it->second;
        return true;
    }
    bool put(const std::string& k, const std::string& v) override
    {
        if (failWrites) return false;
        kv[k] = v;
        return true;
    }
    bool remove(const std::string& k) override { kv.erase(k); return true; }
    bool clearAll() override { kv.clear(); return true; }
    bool applyAtomically(const Puts& puts, const Removes& removes) override
    {
        if (failWrites || failAtomic) return false;
        auto next = kv;
        for (const auto& item : puts) next[item.first] = item.second;
        for (const auto& key : removes) next.erase(key);
        kv.swap(next);
        return true;
    }
};

TokenSession makeSession(const std::string& account, std::int64_t accExp, std::int64_t refExp)
{
    TokenSession s;
    s.account = account;
    s.accessToken.assign("access-abc");
    s.refreshToken.assign("refresh-xyz");
    s.accessExpireAt = accExp;
    s.refreshExpireAt = refExp;
    s.sessionId = "sess-1";
    return s;
}

void testAdoptPersistAndReload()
{
    std::cout << "[1] adopt 落盘 + 冷启动恢复" << std::endl;
    auto clock = std::make_shared<FakeClock>();
    auto store = std::make_shared<MemStore>();
    {
        TokenManager tm(clock, store);
        check(tm.adopt(makeSession("alice", clock->t + 3600, clock->t + 86400)), "adopt 成功");
        check(tm.hasSession(), "有会话");
    }
    // 新实例从 store 恢复
    TokenManager tm2(clock, store);
    check(tm2.loadFromStore("alice"), "冷启动恢复成功");
    check(tm2.accessToken().str() == "access-abc", "access 恢复正确");
    check(tm2.sessionId() == "sess-1", "session_id 恢复正确");
}

void testExpiryJudgement()
{
    std::cout << "[2] 有效期判断（含 skew）" << std::endl;
    auto clock = std::make_shared<FakeClock>();
    auto store = std::make_shared<MemStore>();
    TokenManager tm(clock, store, /*skew=*/60);
    tm.adopt(makeSession("alice", clock->t + 3600, clock->t + 86400));

    check(tm.accessUsable(), "远未过期→可用");
    check(!tm.accessNeedsRefresh(), "远未过期→不需刷新");

    clock->t += 3600 - 30; // 距过期 30s，小于 skew=60
    check(tm.accessNeedsRefresh(), "进入 skew 窗口→需要刷新");
    check(!tm.accessUsable(), "进入 skew 窗口→不再算可用");

    clock->t += 100; // 已过期
    check(tm.accessNeedsRefresh(), "已过期→需要刷新");
    check(!tm.refreshExpired(), "refresh 仍有效");

    clock->t += 86400; // refresh 也过期
    check(tm.refreshExpired(), "refresh 过期");
}

void testRefreshSingleFlight()
{
    std::cout << "[3] Refresh Single-Flight + request_id 持久化复用" << std::endl;
    auto clock = std::make_shared<FakeClock>();
    auto store = std::make_shared<MemStore>();
    TokenManager tm(clock, store);
    tm.adopt(makeSession("alice", clock->t + 3600, clock->t + 86400));

    const std::string id1 = tm.beginRefresh();
    check(!id1.empty() && tm.refreshInFlight(), "首次 beginRefresh 生成 id 并置 in-flight");
    const std::string id2 = tm.beginRefresh();
    check(id1 == id2, "in-flight 期间复用同一 request_id");

    // 模拟重连：新实例从 store 恢复，应复用同一个 request_id
    TokenManager tm2(clock, store);
    tm2.loadFromStore("alice");
    check(tm2.refreshInFlight(), "恢复后仍标记 in-flight");
    check(tm2.currentRefreshRequestId() == id1, "重连复用持久化的 request_id");

    check(tm.completeRefresh(), "明确完成时清除 request_id");
    check(!tm.refreshInFlight(), "completeRefresh 清除 in-flight");
    std::string leftover;
    check(!store->get("tok:alice:refresh_request_id", leftover), "request_id 已从持久化清除");
}

void testRefreshPersistenceFailureAndSuspend()
{
    std::cout << "[4] Refresh 持久化失败 + 断线保留 request_id" << std::endl;
    auto clock = std::make_shared<FakeClock>();
    auto store = std::make_shared<MemStore>();
    TokenManager tm(clock, store);
    tm.adopt(makeSession("alice", clock->t + 3600, clock->t + 86400));

    store->failAtomic = true;
    check(tm.beginRefresh().empty(), "request_id 落盘失败时不允许开始刷新");
    check(!tm.refreshInFlight(), "持久化失败不进入 in-flight");
    store->failAtomic = false;

    const auto requestId = tm.beginRefresh();
    tm.suspendRefresh();
    TokenManager recovered(clock, store);
    check(recovered.loadFromStore("alice"), "断线后凭据仍可恢复");
    check(recovered.beginRefresh() == requestId, "断线重连复用原 request_id");
}

void testRotateAtomic()
{
    std::cout << "[5] 原子轮换（落盘成功切换 / 失败回滚）" << std::endl;
    auto clock = std::make_shared<FakeClock>();
    auto store = std::make_shared<MemStore>();
    TokenManager tm(clock, store);
    tm.adopt(makeSession("alice", clock->t + 3600, clock->t + 86400));

    check(tm.rotate(SecureString("access-2"), SecureString("refresh-2"),
                    clock->t + 7200, clock->t + 172800, "sess-1"),
          "rotate 落盘成功");
    check(tm.accessToken().str() == "access-2", "内存切换为新 access");

    // 落盘失败 → 回滚
    const auto before = store->kv;
    store->failAtomic = true;
    check(!tm.rotate(SecureString("access-3"), SecureString("refresh-3"),
                     clock->t + 9999, clock->t + 999999, "sess-1"),
          "rotate 落盘失败返回 false");
    check(tm.accessToken().str() == "access-2", "落盘失败后保持旧 access（回滚）");
    check(store->kv == before, "原子提交失败后持久化快照完全不变");
}

void testRevokeFamily()
{
    std::cout << "[6] 吊销清空 family" << std::endl;
    auto clock = std::make_shared<FakeClock>();
    auto store = std::make_shared<MemStore>();
    TokenManager tm(clock, store);
    tm.adopt(makeSession("alice", clock->t + 3600, clock->t + 86400));
    tm.beginRefresh();

    tm.revokeFamily();
    check(!tm.hasSession(), "吊销后无会话");
    check(store->kv.empty(), "持久化全部清空");

    TokenManager tm2(clock, store);
    check(!tm2.loadFromStore("alice"), "吊销后无法恢复");
}

void testExpiredRefreshOnLoadRevokes()
{
    std::cout << "[7] 冷启动发现 refresh 过期→自动吊销" << std::endl;
    auto clock = std::make_shared<FakeClock>();
    auto store = std::make_shared<MemStore>();
    {
        TokenManager tm(clock, store);
        tm.adopt(makeSession("alice", clock->t + 3600, clock->t + 100));
    }
    clock->t += 200; // refresh 过期
    TokenManager tm2(clock, store);
    check(!tm2.loadFromStore("alice"), "refresh 过期→加载失败");
    check(store->kv.empty(), "过期凭据被清空");
}

void testCorruptExpiryRejected()
{
    std::cout << "[8] 损坏过期时间 fail-close" << std::endl;
    for (const std::string bad : {std::string(), std::string("abc"), std::string("123x"),
                                  std::string("0")}) {
        auto clock = std::make_shared<FakeClock>();
        auto store = std::make_shared<MemStore>();
        TokenManager seed(clock, store);
        seed.adopt(makeSession("alice", clock->t + 3600, clock->t + 86400));
        store->kv["tok:alice:refresh_exp"] = bad;
        TokenManager loaded(clock, store);
        check(!loaded.loadFromStore("alice"), "拒绝损坏 refresh_exp: " + bad);
        check(store->kv.empty(), "损坏记录被清理");
    }
}

} // namespace

int main()
{
    std::cout << "=== test_token_manager ===" << std::endl;
    testAdoptPersistAndReload();
    testExpiryJudgement();
    testRefreshSingleFlight();
    testRefreshPersistenceFailureAndSuspend();
    testRotateAtomic();
    testRevokeFamily();
    testExpiredRefreshOnLoadRevokes();
    testCorruptExpiryRejected();

    if (g_failures == 0) {
        std::cout << "test_token_manager PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_token_manager FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
