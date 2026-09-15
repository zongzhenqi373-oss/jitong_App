// 网络层压测：在线消息路由端到端（A→B 消息投递延迟 + 吞吐）。
// 进程内起真 im_server + 两个 client_core 真客户端回环（与 test_e2e 同源）。
//
// 场景：
//   注册/登录 → A 流水线发 messages 条 → B 侧按内容内嵌时间戳统计投递延迟
//   输出：QPS + P50/P95/P99（B 侧接收延迟）+ A 侧 ACK 总数
//
// 用法：
//   load_client [messages] [msgSizeBytes]
//   默认 messages=10000, msgSize=256

#include <cassert>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Server.h"
#include "client_core/ClientCore.h"
#include "client_core/Protocol.h"

#include <openssl/evp.h>
#include <openssl/pem.h>

using namespace im;
using namespace im::proto;

namespace {

constexpr std::uint16_t LOAD_PORT = 24681;
const char* LOAD_DB = "/tmp/im_server_load.db";
const char* LOAD_UPLOADS = "/tmp/im_server_load_uploads";
const std::string LOAD_CERT = IM_SERVER_TEST_CERT;
const std::string LOAD_KEY = IM_SERVER_TEST_KEY;
const std::string LOAD_APP_IDENTITY_KEY = IM_SERVER_TEST_APP_IDENTITY_KEY;
const std::string LOAD_SERVERNAME = "im.example.com";

class LatencyStats {
public:
    void add(double ms) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_samples.push_back(ms);
    }
    size_t count() const {
        std::lock_guard<std::mutex> lk(m_mu);
        return m_samples.size();
    }
    double percentile(double p) const {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_samples.empty()) return 0.0;
        std::vector<double> v = m_samples;
        std::sort(v.begin(), v.end());
        return v[static_cast<size_t>(p * (v.size() - 1))];
    }
    double max() const {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_samples.empty()) return 0.0;
        return *std::max_element(m_samples.begin(), m_samples.end());
    }

private:
    mutable std::mutex m_mu;
    std::vector<double> m_samples;
};

/** 压测用事件回调：只关心登录结果、接收消息（B 侧）、ACK（A 侧）。 */
struct LoadEvents : IClientEvents {
    std::mutex mtx;
    std::condition_variable cv;
    int registerResult = -1;
    int loginResult = -1;
    int loginUserId = 0;
    bool gotRegister = false;
    bool gotLogin = false;
    bool closed = false;

    LatencyStats recvLat;      // B 侧投递延迟
    std::int64_t recvCount = 0;
    std::int64_t ackCount = 0;

    void notify() { cv.notify_all(); }

    template <typename Pred>
    bool waitFor(Pred pred, int timeoutMs = 15000)
    {
        std::unique_lock<std::mutex> l(mtx);
        return cv.wait_for(l, std::chrono::milliseconds(timeoutMs), pred);
    }

    void onRegisterResult(int r) override
    {
        std::lock_guard<std::mutex> l(mtx);
        registerResult = r;
        gotRegister = true;
        notify();
    }
    void onLoginResult(int r, int uid) override
    {
        std::lock_guard<std::mutex> l(mtx);
        loginResult = r;
        loginUserId = uid;
        gotLogin = true;
        notify();
    }
    void onSelfInfo(const UserInfo&) override {}
    void onFriendInfo(const FriendInfo&) override {}
    void onChatMessage(int fromId, const std::string& msgUtf8) override
    {
        (void)fromId;
        // 内容内嵌发送时刻纳秒：<sendNs>:<padding>
        const auto colon = msgUtf8.find(':');
        const std::int64_t sendNs = (colon == std::string::npos)
            ? 0 : std::stoll(msgUtf8.substr(0, colon));
        const auto sendT = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(sendNs));
        const double delayMs = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - sendT).count();
        std::lock_guard<std::mutex> l(mtx);
        recvLat.add(delayMs);
        ++recvCount;
        notify();
    }
    void onImageMessage(int, const std::string&, int, int, const std::string&) override {}
    void onChatSendResult(int friId, int r) override
    {
        (void)friId;
        (void)r;
        std::lock_guard<std::mutex> l(mtx);
        ++ackCount;
        notify();
    }
    void onAddFriendRequest(int, const std::string&) override {}
    void onAddFriendResult(int, const std::string&) override {}
    void onFriendOffline(int) override {}
    void onKickedOffline(int) override {}
    void onConnectionClosed() override
    {
        std::lock_guard<std::mutex> l(mtx);
        closed = true;
        notify();
    }
};

std::vector<unsigned char> ed25519PublicFromPemFile(const std::string& path)
{
    std::FILE* fp = std::fopen(path.c_str(), "r");
    if (!fp) return {};
    EVP_PKEY* pkey = PEM_read_PrivateKey(fp, nullptr, nullptr, nullptr);
    std::fclose(fp);
    if (!pkey) return {};
    std::vector<unsigned char> pub(32, 0);
    std::size_t len = 32;
    const bool ok = EVP_PKEY_get_raw_public_key(pkey, pub.data(), &len) > 0 && len == 32;
    EVP_PKEY_free(pkey);
    return ok ? pub : std::vector<unsigned char>{};
}

im::ClientConfig makeConfig()
{
    im::ClientConfig cfg;
    cfg.tlsServerName = LOAD_SERVERNAME;
    cfg.caFile = LOAD_CERT;
    const auto pub = ed25519PublicFromPemFile(LOAD_APP_IDENTITY_KEY);
    assert(pub.size() == 32 && "压测身份公钥派生失败");
    cfg.identityKeys[1] = pub;
    return cfg;
}

} // namespace

int main(int argc, char** argv)
{
    const int messages = argc > 1 ? std::atoi(argv[1]) : 10000;
    const int msgSize = argc > 2 ? std::atoi(argv[2]) : 256;
    // 第 3 个参数 "rtt"：串行往返模式（发一条等 B 收到再发下一条，测单条延迟底线）；
    // 默认流水线模式（连续发，测吞吐天花板）。
    const bool rttMode = argc > 3 && std::string(argv[3]) == "rtt";
    const std::string padding(msgSize, 'x');
    // 第 4 个参数 "conn:<N>"：并发连接 + 注册 + 登录压测（N 个客户端）。
    int connCount = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]).rfind("conn:", 0) == 0) {
            connCount = std::atoi(std::string(argv[i]).substr(5).c_str());
        }
    }

    std::remove(LOAD_DB);

    imsrv::Server server(LOAD_PORT, 2, 2, LOAD_DB, LOAD_UPLOADS, LOAD_CERT, LOAD_KEY,
                         static_cast<std::uint16_t>(LOAD_PORT + 1), LOAD_APP_IDENTITY_KEY, 1);
    assert(server.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const im::ClientConfig cfg = makeConfig();

    // ---- 并发连接 + 注册 + 登录压测 ----
    if (connCount > 0) {
        const int N = connCount;
        std::vector<std::unique_ptr<ClientCore>> clients;
        std::vector<std::unique_ptr<LoadEvents>> events;
        clients.reserve(N);
        events.reserve(N);

        // 1) 并发建连（TLS + 应用层握手）
        const auto c0 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; ++i) {
            auto ev = std::make_unique<LoadEvents>();
            auto cl = std::make_unique<ClientCore>(cfg);
            cl->setEventSink(ev.get());
            cl->connectToServer("127.0.0.1", LOAD_PORT);
            events.push_back(std::move(ev));
            clients.push_back(std::move(cl));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(600));

        // 2) 并发注册（唯一 tel）
        const auto r0 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; ++i) {
            clients[i]->sendRegister("压测" + std::to_string(i),
                                     "139" + std::to_string(10000000 + i), "pass1234");
        }
        for (int i = 0; i < N; ++i) {
            if (!events[i]->waitFor([&] { return events[i]->gotRegister; }, 60000)) {
                std::cerr << "注册超时 i=" << i << "\n";
            }
        }
        const auto r1 = std::chrono::steady_clock::now();

        // 3) 并发登录
        const auto l0 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; ++i) {
            clients[i]->sendLogin("139" + std::to_string(10000000 + i), "pass1234");
        }
        for (int i = 0; i < N; ++i) {
            if (!events[i]->waitFor([&] { return events[i]->gotLogin; }, 60000)) {
                std::cerr << "登录超时 i=" << i << "\n";
            }
        }
        const auto l1 = std::chrono::steady_clock::now();

        const double regMs = std::chrono::duration<double, std::milli>(r1 - r0).count();
        const double loginMs = std::chrono::duration<double, std::milli>(l1 - l0).count();
        std::cout << "# conn_test N=" << N << "\n";
        std::cout << "scene,ops,total_ms,ops_per_s\n";
        std::cout << "NET1_register," << N << "," << (long long)regMs << ","
                  << (long long)(regMs > 0 ? N * 1000.0 / regMs : 0) << "\n";
        std::cout << "NET2_login," << N << "," << (long long)loginMs << ","
                  << (long long)(loginMs > 0 ? N * 1000.0 / loginMs : 0) << "\n";

        for (auto& c : clients) c->disconnect();
        server.stop();
        std::remove(LOAD_DB);
        return 0;
    }

    ClientCore a(cfg);
    ClientCore b(cfg);
    LoadEvents ea, eb;
    a.setEventSink(&ea);
    b.setEventSink(&eb);

    a.connectToServer("127.0.0.1", LOAD_PORT);
    b.connectToServer("127.0.0.1", LOAD_PORT);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    // 直接用种子用户「张三 / 李四」（seedIfEmpty 已播种双向好友关系），密码 123456。
    a.sendLogin("13800000001", "123456");
    b.sendLogin("13800000002", "123456");
    assert(ea.waitFor([&] { return ea.gotLogin && ea.loginResult == LOGIN_SUCCESS; }));
    assert(eb.waitFor([&] { return eb.gotLogin && eb.loginResult == LOGIN_SUCCESS; }));
    const int idA = ea.loginUserId;
    const int idB = eb.loginUserId;

    // 压测：A 发 messages 条（内容内嵌发送时刻纳秒）
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < messages; ++i) {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        a.sendChatMessage(idB, std::to_string(now) + ":" + padding);
        if (rttMode) {
            // 串行往返：等这条被 B 收到后再发下一条（测单条延迟底线，不含流水线排队）
            if (!eb.waitFor([&] { return eb.recvCount >= i + 1; }, 30000)) break;
        }
    }

    // 等 B 收齐 messages 条
    const bool recvOk = eb.waitFor([&] { return eb.recvCount >= messages; }, 120000);
    const auto t1 = std::chrono::steady_clock::now();
    // 等 A 收齐 ACK
    ea.waitFor([&] { return ea.ackCount >= messages; }, 120000);

    const double totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double qps = totalMs > 0 ? (messages * 1000.0 / totalMs) : 0.0;

    std::cout << "# load_client messages=" << messages << " msgSize=" << msgSize
              << " mode=" << (rttMode ? "rtt" : "pipeline")
              << " idA=" << idA << " idB=" << idB << "\n";
    std::cout << "recv_ok=" << (recvOk ? 1 : 0) << " recv_count=" << eb.recvCount
              << " ack_count=" << ea.ackCount << "\n";
    std::cout << "scene,ops,total_ms,ops_per_s,p50_ms,p95_ms,p99_ms,max_ms\n";
    std::cout << "NET3_chat_route," << messages << "," << (long long)totalMs << ","
              << (long long)qps << "," << eb.recvLat.percentile(0.50) << ","
              << eb.recvLat.percentile(0.95) << "," << eb.recvLat.percentile(0.99) << ","
              << eb.recvLat.max() << "\n";

    a.disconnect();
    b.disconnect();
    server.stop();
    std::remove(LOAD_DB);
    return 0;
}
