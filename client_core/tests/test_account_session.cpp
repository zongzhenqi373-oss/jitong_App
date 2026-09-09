// AccountSession 单元测试（P5 接线层）。
//
// 用 FakeAuthTransport + fake clock/KV/软件签名器驱动整条认证链路，验证编排正确：
// 密码登录成功(落盘)、密码错误、Token 冷启动登录、自动刷新、刷新吊销→被动登出、
// 被踢、登出、登录中断线、连点单飞。

#include "client_core/AccountSession.h"
#include "transport/DeviceProof.h"
#include "im.pb.h"

#include <iostream>
#include <map>
#include <memory>
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

class FakeClock : public IClock {
public:
    std::int64_t t = 1000000;
    std::int64_t nowEpochSeconds() const override { return t; }
};

class MemStore : public ITokenStore {
public:
    std::map<std::string, std::string> kv;
    bool failAtomic = false;
    bool get(const std::string& k, std::string& v) const override
    {
        auto it = kv.find(k);
        if (it == kv.end()) return false;
        v = it->second; return true;
    }
    bool put(const std::string& k, const std::string& v) override { kv[k] = v; return true; }
    bool remove(const std::string& k) override { kv.erase(k); return true; }
    bool clearAll() override { kv.clear(); return true; }
    bool applyAtomically(const Puts& puts, const Removes& removes) override
    {
        if (failAtomic) return false;
        auto next = kv;
        for (const auto& p : puts) next[p.first] = p.second;
        for (const auto& r : removes) next.erase(r);
        kv.swap(next);
        return true;
    }
};

class SoftSigner : public IP256Signer {
public:
    SoftSigner() { m_key.generate(); }
    Bytes publicKeyDer() const override { return m_key.publicKeyDer(); }
    Bytes sign(const Bytes& m) const override { return m_key.sign(m); }
private:
    im::transport::DeviceProofKey m_key;
};

// 记录 AccountSession 发出的所有请求，并可被测试直接驱动回调。
class FakeAuthTransport : public IAuthTransport {
public:
    IAuthTransportCallbacks* cb = nullptr;
    int connectCalls = 0;
    int passwordLogins = 0;
    int tokenLogins = 0;
    int refreshes = 0;
    int logouts = 0;
    int disconnects = 0;
    std::int64_t lastReconnectDelay = -2;
    int cancelReconnects = 0;
    bool autoConnectOk = true; // connect() 是否自动回调成功
    std::string lastAccount;
    std::string lastPassword;
    std::string lastLogoutPayload;

    void connect() override
    {
        ++connectCalls;
        if (autoConnectOk && cb) cb->onConnectResult(true);
    }
    void disconnect() override { ++disconnects; }
    Bytes appSessionId() const override { return Bytes(16, 0x5A); }
    void sendPasswordLogin(const std::string& account, const std::string& password,
                           std::uint32_t) override
    {
        ++passwordLogins;
        lastAccount = account;
        lastPassword = password;
    }
    void sendTokenLogin(const std::string&, std::uint32_t) override { ++tokenLogins; }
    void sendRefresh(const std::string&, std::uint32_t) override { ++refreshes; }
    void sendLogout(const std::string& payload, std::uint32_t) override
    {
        ++logouts;
        lastLogoutPayload = payload;
    }
    void scheduleReconnect(std::int64_t delayMs) override { lastReconnectDelay = delayMs; }
    void cancelReconnect() override { ++cancelReconnects; }
};

AuthResult makeResult(std::int32_t uid, std::int64_t now)
{
    AuthResult r;
    r.userId = uid;
    r.accessToken = "acc-tok";
    r.refreshToken = "ref-tok";
    r.accessExpireAt = now + 900;    // 15 分钟
    r.refreshExpireAt = now + 86400; // 1 天
    r.sessionId = "sess-abc";
    return r;
}

struct Fixture {
    std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
    std::shared_ptr<MemStore> store = std::make_shared<MemStore>();
    SoftSigner signer;
    FakeAuthTransport tp;
    std::unique_ptr<AccountSession> sess;
    std::vector<AccountEvent> events;

    Fixture()
    {
        sess = std::make_unique<AccountSession>(tp, clock, store, signer, "dev-1");
        tp.cb = sess.get();
        sess->setEventSink([this](const AccountEvent& e) { events.push_back(e); });
    }
    int countEvent(AccountEventType t) const
    {
        int n = 0;
        for (auto& e : events) if (e.type == t) ++n;
        return n;
    }
};

void testPasswordLoginSuccess()
{
    std::cout << "[1] 密码登录成功：连接→发登录→落盘→Authenticated" << std::endl;
    Fixture f;
    f.sess->startWithPassword("alice", "pw");
    check(f.tp.connectCalls == 1, "发起连接");
    check(f.tp.passwordLogins == 1, "连接后发密码登录");

    f.tp.cb->onLoginResult(0, makeResult(1001, f.clock->t));
    check(f.sess->accountState() == AccountState::Authenticated, "→ Authenticated");
    check(f.countEvent(AccountEventType::LoginSucceeded) == 1, "LoginSucceeded 事件");
    check(f.sess->tokens().hasSession(), "token 已落盘");
    check(f.store->kv.count("tok:alice:access") == 1, "持久化含 access");
}

void testPasswordLoginWrongPassword()
{
    std::cout << "[2] 密码错误 → LoginFailed(InvalidCredentials) 并断开" << std::endl;
    Fixture f;
    f.sess->startWithPassword("alice", "bad");
    f.tp.cb->onLoginResult(2 /*LOGIN_PASSERROR*/, AuthResult{});
    check(f.sess->accountState() == AccountState::LoggedOut, "→ LoggedOut");
    bool found = false;
    for (auto& e : f.events)
        if (e.type == AccountEventType::LoginFailed && e.error == AuthError::InvalidCredentials)
            found = true;
    check(found, "LoginFailed(InvalidCredentials)");
    check(f.tp.disconnects >= 1, "认证失败后断开");
}

void testTokenColdStart()
{
    std::cout << "[3] 冷启动 Token 登录" << std::endl;
    Fixture f;
    // 预置持久化 token（未过期）
    f.store->kv["tok:alice:access"] = "acc-tok";
    f.store->kv["tok:alice:refresh"] = "ref-tok";
    f.store->kv["tok:alice:access_exp"] = std::to_string(f.clock->t + 900);
    f.store->kv["tok:alice:refresh_exp"] = std::to_string(f.clock->t + 86400);
    f.store->kv["tok:alice:session_id"] = "sess-abc";

    const auto op = f.sess->startWithSavedToken("alice");
    check(op != 0, "有有效凭据→发起 token 登录");
    check(f.tp.tokenLogins == 1, "发送 TokenLogin（非密码登录）");
    check(f.tp.passwordLogins == 0, "未发密码登录");

    f.tp.cb->onTokenLoginResult(0, 1001, f.clock->t + 900);
    check(f.sess->accountState() == AccountState::Authenticated, "token 登录成功→Authenticated");
}

void testTokenColdStartNoCreds()
{
    std::cout << "[4] 冷启动无凭据 → 返回 0（需密码登录）" << std::endl;
    Fixture f;
    const auto op = f.sess->startWithSavedToken("nobody");
    check(op == 0, "无凭据返回 0");
    check(f.tp.connectCalls == 0, "不发起连接");
}

void testAutoRefreshSuccess()
{
    std::cout << "[5] access 将过期 → tick 自动刷新成功轮换" << std::endl;
    Fixture f;
    f.sess->startWithPassword("alice", "pw");
    f.tp.cb->onLoginResult(0, makeResult(1001, f.clock->t));

    // 时间推进到临近过期
    f.clock->t += 900 - 30; // 距过期 30s < skew 60
    f.sess->tick();
    check(f.tp.refreshes == 1, "tick 触发一次刷新");
    check(f.sess->accountState() == AccountState::Refreshing, "→ Refreshing");

    // 再 tick 不应重复发（Single-Flight）
    f.sess->tick();
    check(f.tp.refreshes == 1, "刷新在途不重复发");

    AuthResult r = makeResult(1001, f.clock->t);
    r.accessToken = "acc-tok-2";
    f.tp.cb->onRefreshResult(0, r);
    check(f.sess->accountState() == AccountState::Authenticated, "刷新成功→Authenticated");
    check(f.sess->tokens().accessToken().str() == "acc-tok-2", "token 已轮换");
}

void testRefreshRevokeKicksOut()
{
    std::cout << "[6] 刷新失败(吊销) → 清 family + 被动登出" << std::endl;
    Fixture f;
    f.sess->startWithPassword("alice", "pw");
    f.tp.cb->onLoginResult(0, makeResult(1001, f.clock->t));
    f.clock->t += 900;
    f.sess->tick();
    f.tp.cb->onRefreshResult(1 /*fail*/, AuthResult{});
    check(f.sess->accountState() == AccountState::LoggedOutKicked, "→ LoggedOutKicked");
    check(!f.sess->tokens().hasSession(), "family 已清空");
    check(f.store->kv.empty(), "持久化清空");
}

void testKicked()
{
    std::cout << "[7] 被踢下线" << std::endl;
    Fixture f;
    f.sess->startWithPassword("alice", "pw");
    f.tp.cb->onLoginResult(0, makeResult(1001, f.clock->t));
    f.tp.cb->onKicked(0);
    check(f.sess->accountState() == AccountState::LoggedOutKicked, "→ LoggedOutKicked");
    check(f.countEvent(AccountEventType::Kicked) == 1, "Kicked 事件");
    // 被踢后断线不重连
    f.tp.lastReconnectDelay = -2;
    f.tp.cb->onDisconnected(false);
    check(f.tp.lastReconnectDelay == -2, "被踢后断线不安排重连");
}

void testLoginDropReconnect()
{
    std::cout << "[8] 已认证断线 → 安排退避重连" << std::endl;
    Fixture f;
    f.sess->startWithPassword("alice", "pw");
    f.tp.cb->onLoginResult(0, makeResult(1001, f.clock->t));
    f.tp.cb->onDisconnected(/*intentional=*/false);
    check(f.tp.lastReconnectDelay == 1000, "首次退避 1000ms");
    f.tp.cb->onReconnectFired();
    check(f.tp.tokenLogins == 1, "重连安全通道就绪后重新 Token Login");
}

void testLogout()
{
    std::cout << "[9] 登出清理" << std::endl;
    Fixture f;
    f.sess->startWithPassword("alice", "pw");
    f.tp.cb->onLoginResult(0, makeResult(1001, f.clock->t));
    f.sess->logout(false);
    check(f.tp.logouts == 1, "发送 Logout");
    f.tp.cb->onLogoutResult(0);
    check(f.sess->accountState() == AccountState::LoggedOut, "→ LoggedOut");
    check(f.store->kv.empty(), "凭据清空");
    check(f.countEvent(AccountEventType::LoggedOut) == 1, "LoggedOut 事件");
}

void testConnectDuringLoginDropped()
{
    std::cout << "[10] 认证中连接失败 → 登录失败" << std::endl;
    Fixture f;
    f.tp.autoConnectOk = false; // connect 不自动成功
    f.sess->startWithPassword("alice", "pw");
    check(f.tp.connectCalls == 1 && f.tp.passwordLogins == 0, "已发起连接但未发登录");
    f.tp.cb->onConnectResult(false);
    check(f.sess->accountState() == AccountState::LoggedOut, "连接失败→LoggedOut");
    check(f.countEvent(AccountEventType::LoginFailed) == 1, "LoginFailed 事件");
}

void testRejectedLoginDoesNotOverwriteCredential()
{
    std::cout << "[11] 异账号防重入不污染在途凭据" << std::endl;
    Fixture f;
    f.tp.autoConnectOk = false;
    const auto a = f.sess->startWithPassword("alice", "alice-pw");
    const auto b = f.sess->startWithPassword("bob", "bob-pw");
    check(a != 0 && b == 0, "账号 B 被 Single-Flight 拒绝");
    f.tp.cb->onConnectResult(true);
    check(f.tp.lastAccount == "alice" && f.tp.lastPassword == "alice-pw",
          "仍发送账号 A 的原始凭据");
}

void testColdStartRefreshThenTokenLogin()
{
    std::cout << "[12] 冷启动 access 过期时先 Refresh 再 Token Login" << std::endl;
    Fixture f;
    f.store->kv["tok:alice:access"] = "expired-access";
    f.store->kv["tok:alice:refresh"] = "valid-refresh";
    f.store->kv["tok:alice:access_exp"] = std::to_string(f.clock->t - 1);
    f.store->kv["tok:alice:refresh_exp"] = std::to_string(f.clock->t + 86400);
    f.store->kv["tok:alice:session_id"] = "sess-abc";

    check(f.sess->startWithSavedToken("alice") != 0, "发起自动恢复");
    check(f.tp.refreshes == 1 && f.tp.tokenLogins == 0, "先发送 Refresh");
    auto refreshed = makeResult(1001, f.clock->t);
    refreshed.accessToken = "new-access";
    f.tp.cb->onRefreshResult(0, refreshed);
    check(f.tp.tokenLogins == 1, "轮换成功后继续 Token Login");
}

void testRefreshPersistFailureNotReportedSuccess()
{
    std::cout << "[13] Refresh 落盘失败不能报告成功" << std::endl;
    Fixture f;
    f.sess->startWithPassword("alice", "pw");
    f.tp.cb->onLoginResult(0, makeResult(1001, f.clock->t));
    f.clock->t += 900;
    f.sess->tick();
    f.store->failAtomic = true;
    auto refreshed = makeResult(1001, f.clock->t);
    refreshed.accessToken = "must-not-adopt";
    f.tp.cb->onRefreshResult(0, refreshed);
    check(f.countEvent(AccountEventType::RefreshSucceeded) == 0, "没有 RefreshSucceeded");
    check(f.sess->tokens().accessToken().str() == "acc-tok", "仍保留旧内存 Token");
}

void testCancelInvalidatesLateLogin()
{
    std::cout << "[14] Cancel 后迟到 LoginRs 不得恢复认证" << std::endl;
    Fixture f;
    f.tp.autoConnectOk = false;
    f.sess->startWithPassword("alice", "pw");
    f.sess->cancel();
    f.tp.cb->onLoginResult(0, makeResult(1001, f.clock->t));
    check(f.sess->accountState() == AccountState::LoggedOut, "迟到响应被 generation 丢弃");
    check(f.countEvent(AccountEventType::LoginSucceeded) == 0, "没有 LoginSucceeded");
}

void testLogoutAllDevicesForwarded()
{
    std::cout << "[15] logout(allDevices) 参数透传" << std::endl;
    Fixture f;
    f.sess->startWithPassword("alice", "pw");
    f.tp.cb->onLoginResult(0, makeResult(1001, f.clock->t));
    f.sess->logout(true);
    im::proto::LogoutRq rq;
    check(rq.ParseFromString(f.tp.lastLogoutPayload) && rq.logout_all_devices(),
          "LogoutRq.logout_all_devices=true");
}

} // namespace

int main()
{
    std::cout << "=== test_account_session ===" << std::endl;
    testPasswordLoginSuccess();
    testPasswordLoginWrongPassword();
    testTokenColdStart();
    testTokenColdStartNoCreds();
    testAutoRefreshSuccess();
    testRefreshRevokeKicksOut();
    testKicked();
    testLoginDropReconnect();
    testLogout();
    testConnectDuringLoginDropped();
    testRejectedLoginDoesNotOverwriteCredential();
    testColdStartRefreshThenTokenLogin();
    testRefreshPersistFailureNotReportedSuccess();
    testCancelInvalidatesLateLogin();
    testLogoutAllDevicesForwarded();

    if (g_failures == 0) {
        std::cout << "test_account_session PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_account_session FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
