#pragma once
// IM 服务端：asio Reactor + IO/业务两阶段并发
//
// 并发模型（对齐设计文档 3.1）：
//   io_context + acceptor
//     → N 个 IO 线程（Reactor 多路复用）
//     → 每连接 Session + strand（同连接读写串行，连接内无锁）
//     → 业务按"会话一致性哈希"投递到业务 strand 池（thread_pool 执行器），
//       同一连接的业务严格按序、跨连接并行
//   数据库：SQLite 连接池（WAL + busy_timeout）
//
// 关停：SIGINT/SIGTERM 或 stop() → 停 accept → 停 io → join 全部线程。

#include <asio.hpp>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <asio/ssl.hpp>

#include "Database.h"
#include "Presence.h"
#include "auth/TokenService.h"
#include "crypto/AppCrypto.h"

namespace imsrv {

class Session;
class Dispatcher;
class HttpFileServer;

class Server {
public:
    Server(std::uint16_t port,
            int ioThreadCount,
            int dbWorkers,
           std::string dbPath,
           std::string uploadDir,
           std::string certPath,
           std::string keyPath,
           std::uint16_t httpPort,
           std::string appIdentityKeyPath = {},
           std::uint32_t appIdentityKeyId = 1
        );
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start();
    void run();  // 阻塞直到 stop()
    void stop();

    // Session 回调（业务 strand 上执行）
    void dispatchPacket(const std::shared_ptr<Session>& s, std::uint32_t type, std::string payload);
    // Session 回调（连接消亡，io strand 上执行）
    void onSessionClosed(const std::shared_ptr<Session>& s);

    Database& db() { return m_db; }
    Presence& presence() { return m_presence; }
    const std::string& uploadDir() const { return m_uploadDir; }
    HttpFileServer* httpFileServer() { return m_httpFileServer.get(); }
    const crypto::Bytes& appIdentityPrivateKey() const { return m_appIdentityPrivateKey; }
    std::uint32_t appIdentityKeyId() const { return m_appIdentityKeyId; }

    // 会话的业务 strand（构造 Session 时分配，同会话固定）
    using BizStrand = asio::strand<asio::thread_pool::executor_type>;
    std::shared_ptr<BizStrand> nextBizStrand();
    
    TokenService& tokenService()
    {
        return m_tokenService;
    }

private:
    void doAccept();
    void hbScanLoop();

    std::uint16_t m_port;
    int m_ioThreadCount;
    int m_dbWorkers;
    std::string m_dbPath;
    std::string m_uploadDir;
    std::string m_certPath;
    std::string m_keyPath;
    std::uint16_t m_httpPort;
    std::string m_appIdentityKeyPath;
    std::uint32_t m_appIdentityKeyId;
    crypto::Bytes m_appIdentityPrivateKey;
    asio::ssl::context m_sslContext;

    asio::io_context m_io;
    asio::ip::tcp::acceptor m_acceptor;
    asio::signal_set m_signals;
    std::vector<std::thread> m_ioThreads;
    std::unique_ptr<asio::thread_pool> m_dbPool;
    std::vector<std::shared_ptr<BizStrand>> m_bizStrands;
    std::atomic<size_t> m_nextBiz{0};

    Database m_db;
    TokenService m_tokenService;
    Presence m_presence;
    std::unique_ptr<Dispatcher> m_dispatcher;
    std::unique_ptr<HttpFileServer> m_httpFileServer;

    std::thread m_hbThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_stopRequested{false}; // 信号处理器只置位（它在 IO 线程上执行，不能直接 join 线程）
};

} // namespace imsrv
