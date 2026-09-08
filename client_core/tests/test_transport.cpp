// Transport 测试：真实 loopback TLS 上的消息边界、生命周期与重连（P3-T04）。
//
// 覆盖：
//   - 半包（Header 1 字节 / Body 分片）、粘包
//   - 非法包长（0 / 超上限 / 小端）→ 立即断线且只通知一次
//   - 服务端在 Header、Body、写入过程中关闭
//   - 连续重连 50 次，回调次数与状态不异常
//   - 发送过程中 close，队列项全部收到回执，不悬挂
//   - 旧连接迟到回调不污染新连接（generation）

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

#include "TcpTransport.h"
#include "client_core/Protocol.h"

using namespace im;
using asio::ip::tcp;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++g_failures;
        std::cerr << "  [FAIL] " << what << std::endl;
    } else {
        std::cout << "  [ok]   " << what << std::endl;
    }
}

std::string frameOf(proto::protType type, const std::string& payload)
{
    return FrameCodec::encodeFrame(type, payload);
}

/**
 * 测试用 TLS 服务端。所有 socket 操作都 post 到同一个 io 线程，
 * 保证并发读写安全；测试通过 post() 注入写动作。
 */
class TestServer {
public:
    TestServer()
        : m_ssl(asio::ssl::context::tls_server)
    {
        m_ssl.use_certificate_chain_file(CLIENT_CORE_TEST_CERT);
        m_ssl.use_private_key_file(CLIENT_CORE_TEST_KEY, asio::ssl::context::pem);
    }

    // R2-F02：允许指定证书链文件与私钥（用于多级链 pinning 用例）。
    TestServer(const std::string& certChainFile, const std::string& keyFile)
        : m_ssl(asio::ssl::context::tls_server)
    {
        m_ssl.use_certificate_chain_file(certChainFile);
        m_ssl.use_private_key_file(keyFile, asio::ssl::context::pem);
    }

    ~TestServer() { stop(); }

    std::uint16_t start(bool loopAccept = false)
    {
        m_loopAccept = loopAccept;
        m_acceptor = std::make_unique<tcp::acceptor>(m_io, tcp::endpoint(tcp::v4(), 0));
        const auto port = static_cast<std::uint16_t>(m_acceptor->local_endpoint().port());
        m_thread = std::thread([this]() {
            startAccept();
            m_io.run();
        });
        return port;
    }

    void stop()
    {
        // 必须用 async_accept（见下）：同步 accept 阻塞在内核里，io.stop() 唤不醒，
        // stop() 会永远卡在 join 上。
        m_io.stop();
        if (m_thread.joinable()) m_thread.join();
    }

    /** 在 server io 线程执行动作（保证与读写同一线程）。 */
    void post(std::function<void()> fn) { asio::post(m_io, std::move(fn)); }

    void writeRaw(const std::string& bytes)
    {
        asio::post(m_io, [this, bytes]() {
            if (!m_sock) return;
            asio::error_code ec;
            asio::write(*m_sock, asio::buffer(bytes), ec);
        });
    }

    void writeFrame(proto::protType type, const std::string& payload)
    {
        writeRaw(frameOf(type, payload));
    }

    void closeConnection()
    {
        asio::post(m_io, [this]() {
            if (!m_sock) return;
            asio::error_code ec;
            m_sock->lowest_layer().shutdown(tcp::socket::shutdown_both, ec);
            m_sock->lowest_layer().close(ec);
        });
    }

    bool waitReady(std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (m_ready.load()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    std::atomic<int> acceptCount{0};
    std::atomic<std::size_t> bytesReceived{0};

    // R2-F03：真解帧压测支持。启用后，服务端把接收字节流按 FrameCodec 线格式
    // ([4B BE len][4B LE type][payload]) 逐帧解析，并从 payload 前 4 字节读出唯一
    // 编号，累计到 receivedIds（受 m_idMutex 保护）。用于验证并发发送不丢帧/不串写。
    void enableFrameParsing() { m_parseFrames = true; }
    std::size_t frameCount()
    {
        std::lock_guard<std::mutex> lk(m_idMutex);
        return m_receivedIds.size();
    }
    // 返回 [0, expected) 里缺失与重复的编号数量。
    void idIntegrity(std::uint32_t expected, std::size_t& missing, std::size_t& duplicated)
    {
        std::lock_guard<std::mutex> lk(m_idMutex);
        missing = 0;
        for (std::uint32_t i = 0; i < expected; ++i) {
            if (m_receivedIds.find(i) == m_receivedIds.end()) ++missing;
        }
        duplicated = m_duplicateCount;
    }

private:
    /**
     * 必须用 async_accept：同步 accept 会阻塞在内核里，io_context::stop() 无法唤醒，
     * 析构/ stop() 会永远卡在 join 上（重连 50 次用例踩过这个坑）。
     */
    void startAccept()
    {
        m_acceptor->async_accept([this](const asio::error_code& ec, tcp::socket raw) {
            if (ec) return;
            if (m_loopAccept) {
                acceptCount.fetch_add(1);
                // 完成 TLS 握手后立即关闭，用于重连压力测试
                auto sock =
                    std::make_shared<asio::ssl::stream<tcp::socket>>(std::move(raw), m_ssl);
                asio::error_code hsec;
                sock->handshake(asio::ssl::stream_base::server, hsec);
                asio::error_code cec;
                sock->lowest_layer().close(cec);
                startAccept(); // 继续接受下一个连接
            } else {
                handshakeAndRead(std::move(raw));
            }
        });
    }

    void handshakeAndRead(tcp::socket raw)
    {
        asio::error_code ec;
        m_sock = std::make_shared<asio::ssl::stream<tcp::socket>>(std::move(raw), m_ssl);
        m_sock->handshake(asio::ssl::stream_base::server, ec);
        if (ec) return;
        m_ready = true;
        startRead();
    }

    void startRead()
    {
        auto buf = std::make_shared<std::vector<char>>(4096);
        m_sock->async_read_some(asio::buffer(buf->data(), buf->size()),
            [this, buf](const asio::error_code& ec, std::size_t n) {
                if (ec) return;
                bytesReceived.fetch_add(n);
                if (m_parseFrames) parseFrames(buf->data(), n);
                startRead();
            });
    }

    // 累积字节并按线格式逐帧解析，提取 payload 前 4 字节的唯一编号。
    void parseFrames(const char* data, std::size_t n)
    {
        m_rxBuf.append(data, n);
        for (;;) {
            if (m_rxBuf.size() < 8) return; // 不足头部
            const auto* p = reinterpret_cast<const unsigned char*>(m_rxBuf.data());
            const std::uint32_t bodyLen = (static_cast<std::uint32_t>(p[0]) << 24) |
                                          (static_cast<std::uint32_t>(p[1]) << 16) |
                                          (static_cast<std::uint32_t>(p[2]) << 8) |
                                          static_cast<std::uint32_t>(p[3]);
            if (bodyLen < 4) { // 非法长度，丢弃剩余
                m_rxBuf.clear();
                return;
            }
            const std::size_t frameTotal = 4 + bodyLen; // 4B 长度前缀 + body
            if (m_rxBuf.size() < frameTotal) return; // 半包，等更多
            // body = [4B LE type][payload]；payload 前 4B 是唯一编号（LE）
            const std::size_t payloadLen = bodyLen - 4;
            if (payloadLen >= 4) {
                const auto* q = reinterpret_cast<const unsigned char*>(m_rxBuf.data()) + 8;
                const std::uint32_t id = static_cast<std::uint32_t>(q[0]) |
                                         (static_cast<std::uint32_t>(q[1]) << 8) |
                                         (static_cast<std::uint32_t>(q[2]) << 16) |
                                         (static_cast<std::uint32_t>(q[3]) << 24);
                std::lock_guard<std::mutex> lk(m_idMutex);
                if (!m_receivedIds.insert(id).second) ++m_duplicateCount;
            }
            m_rxBuf.erase(0, frameTotal);
        }
    }

    asio::io_context m_io;
    asio::ssl::context m_ssl;
    std::unique_ptr<tcp::acceptor> m_acceptor;
    std::shared_ptr<asio::ssl::stream<tcp::socket>> m_sock;
    std::thread m_thread;
    std::atomic<bool> m_ready{false};
    bool m_loopAccept = false;

    // R2-F03 解帧压测状态（仅在 m_io 线程访问 m_rxBuf；m_receivedIds 受 mutex 保护）
    bool m_parseFrames = false;
    std::string m_rxBuf;
    std::mutex m_idMutex;
    std::set<std::uint32_t> m_receivedIds;
    std::size_t m_duplicateCount = 0;
};

// ---------------- 1. 基本连接与收发 ----------------

void testBasicRoundTrip()
{
    std::cout << "[1] 基本连接与收发" << std::endl;
    TestServer server;
    const auto port = server.start();

    TcpTransport transport("im.example.com", CLIENT_CORE_TEST_CERT);
    std::atomic<int> packets{0};
    transport.setPacketHandler([&](proto::protType type, const char*, std::size_t) {
        if (type == proto::DEF_PROT_HEARTBEAT_RS) packets.fetch_add(1);
    });

    check(transport.connect("127.0.0.1", port), "连接成功");
    check(server.waitReady(), "服务端完成握手");
    check(transport.state() == TcpTransport::State::Connected, "状态为 Connected");

    std::string body;
    body.resize(4);
    proto::encodeType32(proto::DEF_PROT_HEARTBEAT_RQ, body.data());
    check(transport.send(body.data(), body.size()) == TcpTransport::SendResult::Ok, "发送入队");

    server.writeFrame(proto::DEF_PROT_HEARTBEAT_RS, "");
    for (int i = 0; i < 100 && packets.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(packets.load() == 1, "收到 1 个包");
    transport.close();
    check(transport.state() == TcpTransport::State::Closed, "关闭后状态为 Closed");
    server.stop();
}

// ---------------- 2. 半包：逐字节 ----------------

void testByteByByteFrame()
{
    std::cout << "[2] 半包：逐字节到达" << std::endl;
    TestServer server;
    const auto port = server.start();

    TcpTransport transport("im.example.com", CLIENT_CORE_TEST_CERT);
    std::atomic<int> count{0};
    proto::protType lastType = 0;
    transport.setPacketHandler([&](proto::protType type, const char* p, std::size_t len) {
        lastType = type;
        if (len == 16) count.fetch_add(1);
    });

    check(transport.connect("127.0.0.1", port), "连接成功");
    check(server.waitReady(), "服务端就绪");

    const std::string frame = frameOf(proto::DEF_PROT_CHAT_INFO_RQ, std::string(16, 'm'));
    // 逐字节写入：服务端每次 asio::write 一个字节
    std::string pending = frame;
    while (!pending.empty()) {
        const std::string one = pending.substr(0, 1);
        pending.erase(0, 1);
        server.writeRaw(one);
    }

    for (int i = 0; i < 200 && count.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(count.load() == 1, "逐字节到达后解出恰好 1 帧");
    check(lastType == proto::DEF_PROT_CHAT_INFO_RQ, "协议号正确");
    transport.close();
    server.stop();
}

// ---------------- 3. 粘包 ----------------

void testStickyFrames()
{
    std::cout << "[3] 粘包：两帧一次到达" << std::endl;
    TestServer server;
    const auto port = server.start();

    TcpTransport transport("im.example.com", CLIENT_CORE_TEST_CERT);
    std::atomic<int> count{0};
    transport.setPacketHandler([&](proto::protType, const char*, std::size_t) {
        count.fetch_add(1);
    });

    check(transport.connect("127.0.0.1", port), "连接成功");
    check(server.waitReady(), "服务端就绪");

    const std::string two =
        frameOf(proto::DEF_PROT_HEARTBEAT_RS, "") + frameOf(proto::DEF_PROT_FRIEND_OFFLINE, "x");
    server.writeRaw(two);

    for (int i = 0; i < 200 && count.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(count.load() == 2, "一次到达解出 2 帧（不串包）");
    transport.close();
    server.stop();
}

// ---------------- 4. 非法包长 ----------------

void testInvalidLength(const std::string& label, std::function<std::string()> makeBytes)
{
    TestServer server;
    const auto port = server.start();

    TcpTransport transport("im.example.com", CLIENT_CORE_TEST_CERT);
    std::atomic<int> closes{0};
    std::atomic<int> packets{0};
    transport.setCloseHandler([&]() { closes.fetch_add(1); });
    transport.setPacketHandler([&](proto::protType, const char*, std::size_t) {
        packets.fetch_add(1);
    });

    if (!transport.connect("127.0.0.1", port) || !server.waitReady()) {
        check(false, label + " 前置连接失败");
        server.stop();
        return;
    }

    server.writeRaw(makeBytes());
    for (int i = 0; i < 200 && closes.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(closes.load() == 1, label + " → 关闭通知恰好 1 次");
    check(!transport.isOpen(), label + " → isOpen=false");
    transport.close();
    server.stop();
}

void testInvalidLengths()
{
    std::cout << "[4] 非法包长一律断线" << std::endl;

    testInvalidLength("body_len=0", []() {
        std::string s(4, '\0');
        return s;
    });
    testInvalidLength("body_len=3（小于协议号）", []() {
        std::string s(4, '\0');
        FrameCodec::encodeLength(3, s.data());
        return s;
    });
    testInvalidLength("body_len>10MiB", []() {
        std::string s(4, '\0');
        FrameCodec::encodeLength(10u * 1024u * 1024u + 1u, s.data());
        return s;
    });
    testInvalidLength("包长误写为小端", []() {
        // 真实长度 8 写成小端：大端解读为 0x08000000 = 134217728
        std::string s(4, '\0');
        s[0] = 0x08;
        return s;
    });
}

// ---------------- 5. 服务端中途关闭 ----------------

void testServerClosesMidStream()
{
    std::cout << "[5] 服务端中途关闭" << std::endl;
    TestServer server;
    const auto port = server.start();

    TcpTransport transport("im.example.com", CLIENT_CORE_TEST_CERT);
    std::atomic<int> closes{0};
    transport.setCloseHandler([&]() { closes.fetch_add(1); });

    check(transport.connect("127.0.0.1", port), "连接成功");
    check(server.waitReady(), "服务端就绪");

    // 先发一个合法 Header（声明 100 字节 body），再关闭 → Body 未到齐就断
    std::string hdr(4, '\0');
    FrameCodec::encodeLength(100, hdr.data());
    server.writeRaw(hdr);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    server.closeConnection();

    for (int i = 0; i < 200 && closes.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(closes.load() == 1, "Body 未到齐时服务端关闭 → 通知 1 次");
    transport.close();
    check(closes.load() == 1, "再次 close 不重复通知");
    server.stop();
}

// ---------------- 6. 重连 50 次 ----------------

void testReconnect50()
{
    std::cout << "[6] 连续重连 50 次" << std::endl;
    TestServer server;
    const auto port = server.start(true); // loop accept

    TcpTransport transport("im.example.com", CLIENT_CORE_TEST_CERT);
    int ok = 0;
    for (int i = 0; i < 50; ++i) {
        if (transport.connect("127.0.0.1", port)) {
            ++ok;
            transport.close();
        }
        // 每次重连都必须能在**同一实例**上成功
    }
    check(ok == 50, "50 次重连全部成功（实际 " + std::to_string(ok) + "）");
    check(server.acceptCount.load() >= 50, "服务端接受连接数 >= 50");
    check(transport.state() == TcpTransport::State::Closed, "结束后状态为 Closed");
    server.stop();
}

// ---------------- 7. 发送过程中 close ----------------

void testCloseDuringSend()
{
    std::cout << "[7] 发送过程中 close" << std::endl;
    TestServer server;
    const auto port = server.start();

    TcpTransport transport("im.example.com", CLIENT_CORE_TEST_CERT);
    std::atomic<int> closes{0};
    transport.setCloseHandler([&]() { closes.fetch_add(1); });
    check(transport.connect("127.0.0.1", port), "连接成功");
    check(server.waitReady(), "服务端就绪");

    std::atomic<int> okCount{0};
    std::atomic<int> cancelCount{0};
    std::string body(512, 'z');
    proto::encodeType32(proto::DEF_PROT_CHAT_INFO_RQ, body.data());

    // 连续提交多帧，然后立刻 close
    for (int i = 0; i < 32; ++i) {
        transport.send(body.data(), body.size(), [&](TcpTransport::SendResult r) {
            if (r == TcpTransport::SendResult::Ok) okCount.fetch_add(1);
            else cancelCount.fetch_add(1);
        });
    }
    transport.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const int total = okCount.load() + cancelCount.load();
    check(total == 32, "所有发送项都收到且仅收到一次回执（实际 " + std::to_string(total) + "）");
    check(closes.load() == 1, "close 通知 1 次");
    server.stop();
}

// ---------------- 8. 未连接时发送 ----------------

void testSendWhenNotConnected()
{
    std::cout << "[8] 未连接与超大包" << std::endl;
    TcpTransport transport("im.example.com", CLIENT_CORE_TEST_CERT);
    std::string body(64, 'q');
    proto::encodeType32(proto::DEF_PROT_LOGIN_RQ, body.data());
    check(transport.send(body.data(), body.size()) == TcpTransport::SendResult::NotConnected,
          "未连接发送返回 NotConnected");

    std::string huge(FrameCodec::kMaxBodyLen, 'x');
    check(transport.send(huge.data(), huge.size()) == TcpTransport::SendResult::NotConnected,
          "未连接时优先返回 NotConnected");
}

// ---------------- 9. 关闭后 generation 递增 ----------------

void testGenerationIncreases()
{
    std::cout << "[9] 连接代号递增" << std::endl;
    TestServer server;
    const auto port = server.start(true);

    TcpTransport transport("im.example.com", CLIENT_CORE_TEST_CERT);
    const auto g0 = transport.generation();
    transport.connect("127.0.0.1", port);
    const auto g1 = transport.generation();
    transport.close();
    const auto g2 = transport.generation();
    // connect() 内部会先 close() 收尾上一次连接，因此 generation 必然递增
    check(g1 > g0, "connect 内部先 close，generation 递增（旧 handler 失效）");
    check(g2 > g1, "close 后 generation 再次递增");
    server.stop();
}

// ---------------- 10. SPKI pinning + 多级证书链（R2-F02） ----------------

// 从 PEM 证书文件计算 SPKI SHA-256 base64（与 TcpTransport 内部同一语义：公钥 DER 的哈希）。
std::string spkiPinOfFile(const std::string& pemCertPath)
{
    FILE* fp = std::fopen(pemCertPath.c_str(), "rb");
    if (!fp) return {};
    X509* cert = PEM_read_X509(fp, nullptr, nullptr, nullptr);
    std::fclose(fp);
    if (!cert) return {};

    unsigned char* der = nullptr;
    const int len = i2d_X509_PUBKEY(X509_get_X509_PUBKEY(cert), &der);
    std::string pin;
    if (len > 0 && der) {
        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256(der, static_cast<std::size_t>(len), digest);
        // base64
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO* mem = BIO_new(BIO_s_mem());
        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
        b64 = BIO_push(b64, mem);
        BIO_write(b64, digest, SHA256_DIGEST_LENGTH);
        BIO_flush(b64);
        BUF_MEM* bptr = nullptr;
        BIO_get_mem_ptr(b64, &bptr);
        if (bptr && bptr->length) pin.assign(bptr->data, bptr->length);
        BIO_free_all(b64);
    }
    if (der) OPENSSL_free(der);
    X509_free(cert);
    return pin;
}

// 用指定 CA + 可选 pin 连接一个发送 fullchain 的服务端；返回是否连接成功。
bool connectWithChain(const std::string& caFile, const std::vector<std::string>& pins,
                      bool setPins)
{
    TestServer server(CLIENT_CORE_TEST_CHAIN_FULLCHAIN, CLIENT_CORE_TEST_CHAIN_LEAF_KEY);
    const auto port = server.start();

    TcpTransport transport("im.example.com", caFile);
    if (setPins) transport.setSpkiPins(pins);
    const bool ok = transport.connect("127.0.0.1", port);
    transport.close();
    server.stop();
    return ok;
}

void testSpkiPinningChain()
{
    std::cout << "[10] SPKI pinning + 多级证书链" << std::endl;

    const std::string rootCa = CLIENT_CORE_TEST_CHAIN_ROOT_CRT;
    const std::string leafPin = spkiPinOfFile(CLIENT_CORE_TEST_CHAIN_LEAF_CRT);
    check(!leafPin.empty(), "能算出 leaf 的 SPKI pin");

    // 10.1 正确 leaf pin + 信任 root：修复前会因回调对 depth>0 用 root 比对 leaf pin 而误拒；
    //      修复后只在 depth==0 pin，应连接成功。
    check(connectWithChain(rootCa, {leafPin}, /*setPins=*/true),
          "多级链 + 正确 leaf pin → 连接成功（depth>0 不再误拒）");

    // 10.2 错误 pin：应 fail-close
    check(!connectWithChain(rootCa, {"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="}, true),
          "多级链 + 错误 pin → 连接失败");

    // 10.3 启用 pinning 但 pin 集为空：一律拒绝，绝不退化成不校验
    check(!connectWithChain(rootCa, {}, /*setPins=*/true),
          "启用 pinning 但空 pin 集 → 连接失败（不退化）");

    // 10.4 不启用 pinning（仅 CA 链 + 主机名）：多级链也应成功
    check(connectWithChain(rootCa, {}, /*setPins=*/false),
          "多级链 + 不启用 pinning → 连接成功");
}

// ---------------- 11. 并发压测：8 线程 × 1250 帧真解帧（R2-F03） ----------------

void testConcurrentSendIntegrity()
{
    std::cout << "[11] 并发压测 8x1250=10000 帧，服务端真解帧校验唯一编号" << std::endl;

    constexpr int kThreads = 8;
    constexpr int kPerThread = 1250;
    constexpr std::uint32_t kTotal = kThreads * kPerThread; // 10000

    TestServer server;
    server.enableFrameParsing();
    const auto port = server.start();

    TcpTransport transport("im.example.com", CLIENT_CORE_TEST_CERT);
    check(transport.connect("127.0.0.1", port), "连接成功");
    check(server.waitReady(), "服务端完成握手");

    // 每个帧的 payload 前 4 字节写入全局唯一编号（LE），后接固定填充。
    // 8 个线程各发 1250 个不重叠编号；验证单写队列在高并发下不丢帧、不串写、不重复。
    std::atomic<int> notQueued{0};
    auto worker = [&](int t) {
        for (int i = 0; i < kPerThread; ++i) {
            const std::uint32_t id = static_cast<std::uint32_t>(t) * kPerThread + i;
            std::string body;
            body.resize(4 + 4 + 8); // type(4) + id(4) + 填充(8)
            proto::encodeType32(proto::DEF_PROT_HEARTBEAT_RQ, body.data());
            body[4] = static_cast<char>(id & 0xFF);
            body[5] = static_cast<char>((id >> 8) & 0xFF);
            body[6] = static_cast<char>((id >> 16) & 0xFF);
            body[7] = static_cast<char>((id >> 24) & 0xFF);
            std::memcpy(&body[8], "PADDING!", 8);
            if (transport.send(body.data(), body.size()) != TcpTransport::SendResult::Ok) {
                notQueued.fetch_add(1);
            }
        }
    };

    std::vector<std::thread> pool;
    for (int t = 0; t < kThreads; ++t) pool.emplace_back(worker, t);
    for (auto& th : pool) th.join();

    check(notQueued.load() == 0, "全部 10000 帧成功入队");

    // 等待服务端收齐（有界超时）
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (server.frameCount() < kTotal &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::size_t missing = 0, duplicated = 0;
    server.idIntegrity(kTotal, missing, duplicated);
    std::cout << "    收到帧数=" << server.frameCount()
              << " 缺失=" << missing << " 重复=" << duplicated << std::endl;
    check(server.frameCount() == kTotal, "服务端解出恰好 10000 帧");
    check(missing == 0, "无缺失编号（不丢帧）");
    check(duplicated == 0, "无重复编号（不串写/不重发）");

    transport.close();
    server.stop();
}

// ---------------- 12. 生命周期压力：反复创建/连接/关闭（R2-F03） ----------------

void testLifecycleStress()
{
    std::cout << "[12] 生命周期压力：100 次创建/连接/关闭，多种关闭时机" << std::endl;

    int failures = 0;
    for (int iter = 0; iter < 100; ++iter) {
        TestServer server;
        const auto port = server.start();
        TcpTransport transport("im.example.com", CLIENT_CORE_TEST_CERT);

        const bool connected = transport.connect("127.0.0.1", port);
        if (!connected) { ++failures; server.stop(); continue; }

        // 交替：立刻关 / 发一帧后关 / 服务端先关
        switch (iter % 3) {
            case 0:
                transport.close();
                break;
            case 1: {
                std::string body(4, 0);
                proto::encodeType32(proto::DEF_PROT_HEARTBEAT_RQ, body.data());
                transport.send(body.data(), body.size());
                transport.close();
                break;
            }
            case 2:
                server.closeConnection();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                transport.close();
                break;
        }
        if (transport.state() != TcpTransport::State::Closed) ++failures;
        server.stop();
    }
    check(failures == 0, "100 次生命周期循环无失败、无卡死");
}

} // namespace

int main()
{
    std::cout << "=== test_transport ===" << std::endl;
    testBasicRoundTrip();
    testByteByByteFrame();
    testStickyFrames();
    testInvalidLengths();
    testServerClosesMidStream();
    testReconnect50();
    testCloseDuringSend();
    testSendWhenNotConnected();
    testGenerationIncreases();
    testSpkiPinningChain();
    testConcurrentSendIntegrity();
    testLifecycleStress();

    if (g_failures == 0) {
        std::cout << "test_transport PASSED" << std::endl;
        return 0;
    }
    std::cerr << "test_transport FAILED: " << g_failures << " 项" << std::endl;
    return 1;
}
