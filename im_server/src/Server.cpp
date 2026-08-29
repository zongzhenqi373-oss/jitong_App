#include "Server.h"
#include "Session.h"
#include "Dispatcher.h"
#include "HttpFileServer.h"
#include "Log.h"
#include "client_core/Protocol.h"
#include "im.pb.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <ctime>

namespace imsrv {

using asio::ip::tcp;
using im::proto::DEF_PROT_FRIEND_OFFLINE;
using im::proto::DEF_PROT_LOGOUT_RQ;
using im::proto::DEF_PROT_TOKEN_REFRESH_RQ;

Server::Server(std::uint16_t port, int ioThreadCount, int dbWorkers,
               std::string dbPath, std::string uploadDir,std::string certPath,
               std::string keyPath, std::uint16_t httpPort,
               std::string appIdentityKeyPath, std::uint32_t appIdentityKeyId)
    : m_port(port)
    , m_ioThreadCount(ioThreadCount > 0 ? ioThreadCount : 4)
    , m_dbWorkers(dbWorkers > 0 ? dbWorkers : 2)
    , m_dbPath(std::move(dbPath))
    , m_uploadDir(std::move(uploadDir))
    , m_certPath(std::move(certPath))
    , m_keyPath(std::move(keyPath))
    , m_httpPort(httpPort)
    , m_appIdentityKeyPath(std::move(appIdentityKeyPath))
    , m_appIdentityKeyId(appIdentityKeyId)
    , m_sslContext(asio::ssl::context::tls_server)
    , m_acceptor(m_io)
    , m_signals(m_io)
    , m_tokenService(m_db)
{
}

Server::~Server()
{
    stop();
}

bool Server::start()
{
    if (m_appIdentityKeyPath.empty() || m_appIdentityKeyId == 0) {
        log("[APP-SEC] 应用身份私钥路径或key_id未配置，拒绝启动");
        return false;
    }
    FILE* identityFile = std::fopen(m_appIdentityKeyPath.c_str(), "rb");
    if (!identityFile) {
        log("[APP-SEC] 无法打开应用身份私钥 path=", m_appIdentityKeyPath);
        return false;
    }
    EVP_PKEY* loadedIdentity = PEM_read_PrivateKey(identityFile, nullptr, nullptr, nullptr);
    std::fclose(identityFile);
    if (!loadedIdentity || EVP_PKEY_base_id(loadedIdentity) != EVP_PKEY_ED25519) {
        if (loadedIdentity) EVP_PKEY_free(loadedIdentity);
        log("[APP-SEC] 应用身份私钥不是有效Ed25519 PEM，拒绝启动");
        return false;
    }
    std::size_t identitySize = 0;
    if (EVP_PKEY_get_raw_private_key(loadedIdentity, nullptr, &identitySize) != 1 || identitySize != 32) {
        EVP_PKEY_free(loadedIdentity);
        log("[APP-SEC] 无法导出Ed25519私钥种子，拒绝启动");
        return false;
    }
    m_appIdentityPrivateKey.resize(identitySize);
    if (EVP_PKEY_get_raw_private_key(loadedIdentity, m_appIdentityPrivateKey.data(), &identitySize) != 1) {
        EVP_PKEY_free(loadedIdentity);
        crypto::secureClear(m_appIdentityPrivateKey);
        log("[APP-SEC] 读取Ed25519私钥失败，拒绝启动");
        return false;
    }
    EVP_PKEY_free(loadedIdentity);
    log("[APP-SEC] 已加载应用身份密钥 keyId=", m_appIdentityKeyId);

    SSL_CTX* native = m_sslContext.native_handle();

    if (SSL_CTX_set_min_proto_version(native, TLS1_3_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(native, TLS1_3_VERSION) != 1) {
        log("[server] 无法限制TLS版本为1.3");
        return false;
    }

    // 不启用TLS 1.3的0-RTT。
    // 当前不调用SSL_CTX_set_max_early_data()，默认即不接受early data。
    // TLS 1.3普通1-RTT数据具备连接级防重放能力，但0-RTT不保证跨连接防重放，因此登录、刷新、发消息都不应使用0-RTT。

    m_sslContext.set_options(
    asio::ssl::context::default_workarounds |
    asio::ssl::context::no_sslv2 |
    asio::ssl::context::no_sslv3 |
    asio::ssl::context::no_tlsv1 |
    asio::ssl::context::no_tlsv1_1 |
    asio::ssl::context::no_tlsv1_2 |
    asio::ssl::context::no_compression
    );

    asio::error_code tlsEc;

    m_sslContext.use_certificate_chain_file(m_certPath, tlsEc);
    if (tlsEc) {
        log("[server] 加载TLS证书失败 path=", m_certPath, " error=", tlsEc.message());
        return false;
    }

    m_sslContext.use_private_key_file(
        m_keyPath,
        asio::ssl::context::pem,
        tlsEc
    );
    if (tlsEc) {
        log("[server] 加载TLS私钥失败 path=", m_keyPath, " error=", tlsEc.message());
        return false;
    }

    if (SSL_CTX_check_private_key(native) != 1) {
        log("[server] TLS证书与私钥不匹配");
        return false;
    }

    // 数据目录与上传目录
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(m_dbPath).parent_path(), ec);
    std::filesystem::create_directories(m_uploadDir + "/img", ec);
    std::filesystem::create_directories(m_uploadDir + "/file", ec);
    std::filesystem::create_directories(m_uploadDir + "/file/tmp", ec);

    // 清理超 24h 未完成的半成品 .part（断点续传窗口外）
    {
        std::error_code ec;
        const auto tmpDir = std::filesystem::path(m_uploadDir) / "file" / "tmp";
        if (std::filesystem::exists(tmpDir, ec)) {
            const auto now = std::filesystem::file_time_type::clock::now();
            for (auto& e : std::filesystem::directory_iterator(tmpDir, ec)) {
                std::error_code fec;
                if (!e.is_regular_file(fec) || fec) continue;
                if (e.path().extension() != ".part") continue;
                auto mtime = std::filesystem::last_write_time(e, ec);
                if (!ec && (now - mtime) > std::chrono::hours(24))
                    std::filesystem::remove(e.path(), ec);
            }
        }
    }

    if (!m_db.open(m_dbPath, m_dbWorkers)) {
        log("[server] 打开数据库失败: ", m_dbPath);
        return false;
    }
    m_db.seedIfEmpty();

    m_httpFileServer = std::make_unique<HttpFileServer>(*this, m_httpPort, m_certPath, m_keyPath);
    // 文件服务先完成构造，消息 Handler 才能以明确依赖注入的方式引用它；
    // Dispatcher 自身只做协议路由，不再拿完整 Server& 作为服务定位器。
    m_dispatcher = std::make_unique<Dispatcher>(
        m_db, m_presence, m_tokenService, *m_httpFileServer);
    if (!m_httpFileServer->start()) {
        log("[server] HTTP 文件服务启动失败 port=", m_httpPort);
        return false;
    }

    // 业务线程池 + 业务 strand 池
    m_dbPool = std::make_unique<asio::thread_pool>(m_dbWorkers);
    for (int i = 0; i < m_dbWorkers; ++i) {
        m_bizStrands.push_back(std::make_shared<BizStrand>(asio::make_strand(m_dbPool->get_executor())));
    }

    // acceptor
    asio::error_code ec2;
    m_acceptor.open(tcp::v4(), ec2);
    if (ec2) { log("[server] open acceptor 失败: ", ec2.message()); return false; }
    m_acceptor.set_option(tcp::acceptor::reuse_address(true), ec2);
    m_acceptor.bind(tcp::endpoint(tcp::v4(), m_port), ec2);
    if (ec2) { log("[server] bind 失败: ", ec2.message()); return false; }
    m_acceptor.listen(100, ec2);
    if (ec2) { log("[server] listen 失败: ", ec2.message()); return false; }

    // 信号优雅关停：处理器在 IO 线程上执行，只置位标志；
    // 真正的 stop()（含 join 线程）由 run() 的调用方线程执行，避免 IO 线程 join 自身
    m_signals.add(SIGINT);
    m_signals.add(SIGTERM);
    m_signals.async_wait([this](const asio::error_code&, int) { m_stopRequested = true; });

    doAccept();
    m_running = true;

    // IO 线程池
    for (int i = 0; i < m_ioThreadCount; ++i) {
        m_ioThreads.emplace_back([this, i]() {
            log("[server] IO 线程 ", i, " 启动");
            m_io.run();
        });
    }

    // 心跳超时扫描线程（10s 周期，90s 超时）
    m_hbThread = std::thread(&Server::hbScanLoop, this);

    log("[server] 启动成功 port=", m_port,
        " httpPort=", m_httpPort,
        " ioThreads=", m_ioThreadCount,
        " dbWorkers=", m_dbWorkers,
        " db=", m_dbPath);
    return true;
}

void Server::run()
{
    // 等待停止信号（主线程阻塞，IO 在 m_ioThreads 上跑）；返回后调用方应调 stop()
    while (!m_stopRequested.load() && !m_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

void Server::stop()
{
    if (m_stop.exchange(true)) return;
    log("[server] 正在关停...");

    // 先停 running 标志：阻止 accept 错误路径重新武装监听、心跳扫描线程退出
    m_running = false;

    if (m_httpFileServer) m_httpFileServer->stop();

    asio::error_code ec;
    m_acceptor.close(ec);
    m_signals.cancel(ec);

    if (m_dbPool) {
        m_dbPool->stop();
        m_dbPool->join();
    }
    m_io.stop();
    for (auto& t : m_ioThreads) {
        if (t.joinable()) t.join();
    }
    if (m_hbThread.joinable()) m_hbThread.join();
    crypto::secureClear(m_appIdentityPrivateKey);
    log("[server] 已关停");
}

std::shared_ptr<Server::BizStrand> Server::nextBizStrand()
{
    return m_bizStrands[m_nextBiz.fetch_add(1) % m_bizStrands.size()];
}

void Server::doAccept()
{
    m_acceptor.async_accept(
        [this](const asio::error_code& ec, tcp::socket socket) {
            if (ec) {
                if (m_running.load()) doAccept(); // 非关停错误继续监听
                return;
            }
            try {
                log("[server] 新连接: ", socket.remote_endpoint().address().to_string());
            } catch (...) {}
            auto session = std::make_shared<Session>(std::move(socket), *this, m_sslContext, nextBizStrand());
            session->start();
            doAccept();
        });
}

void Server::dispatchPacket(const std::shared_ptr<Session>& s, std::uint32_t type, std::string payload)
{
    // 任何有效包都刷新活跃时间（心跳判定依据）
    if (s->userId() > 0) m_presence.touch(s->userId());

    //服务端入口检查access token到期时间：必须在真正处理这个包之前拦截，
    //否则过期后的第一个业务包仍会被完整执行（消息落库/转发）才被关连接
    const bool authMaintenance =
        type == DEF_PROT_TOKEN_REFRESH_RQ ||
        type == DEF_PROT_LOGOUT_RQ;

    if(s->authenticated() && !authMaintenance && s->accessExpiresAt() >0 && s->accessExpiresAt() < std::time(nullptr)) {
        log(
            "[认证] Access token 过期 uid=", s->userId()
        );
        s->close();
        return;
    }

    m_dispatcher->handle(s, type, payload);
}

void Server::onSessionClosed(const std::shared_ptr<Session>& s)
{
    const int uid = s->userId();
    if (uid <= 0) return; // 未登录连接无需广播

    // 摘在线映射（仅当 map 中就是本 session，互踢场景不会误摘新连接）
    m_presence.offline(uid, s);

    // 广播好友下线（投递到业务 strand 执行 db 查询 + 转发，与正常业务同通道保序）
    asio::post(*m_bizStrands[uid % m_bizStrands.size()], [this, uid]() {
        im::proto::FriendOffline pkt;
        pkt.set_offlineid(uid);
        const std::string payload = pkt.SerializeAsString();
        for (const auto& f : m_db.getFriends(uid)) {
            if (auto fs = m_presence.get(f.id)) {
                fs->deliver(DEF_PROT_FRIEND_OFFLINE, payload);
            }
        }
    });
}

void Server::hbScanLoop()
{
    while (m_running.load()) {
        for (int i = 0; i < 100 && m_running.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!m_running.load()) break;

        auto stale = m_presence.scanStale(90);
        for (auto& kv : stale) {
            log("[server] 心跳超时，强制下线 id=", kv.first);
            kv.second->close(); // 关闭后 onSessionClosed 负责摘映射并广播
        }
    }
}

} // namespace imsrv
