#include "TcpTransport.h"

#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <utility>

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/tls1.h>
#include <openssl/x509.h>

namespace im {
namespace {

constexpr std::size_t kReadChunk = 64 * 1024;

/** TLS 握手与连接的总超时。 */
constexpr auto kConnectTimeout = std::chrono::seconds(15);

std::string base64Encode(const unsigned char* data, std::size_t len)
{
    if (len == 0) return {};
    std::string out(((len + 2) / 3) * 4 + 1, '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data,
                                  static_cast<int>(len));
    if (n <= 0) return {};
    out.resize(static_cast<std::size_t>(n));
    return out;
}

/** 从证书计算 SPKI SHA-256 的 base64（与 Android TlsPinning 同一语义：公钥而非整证书）。 */
std::string spkiSha256Base64(X509* cert)
{
    if (!cert) return {};
    X509_PUBKEY* pubKey = X509_get_X509_PUBKEY(cert);
    if (!pubKey) return {};

    unsigned char* der = nullptr;
    const int derLen = i2d_X509_PUBKEY(pubKey, &der);
    if (derLen <= 0 || der == nullptr) return {};

    unsigned char digest[SHA256_DIGEST_LENGTH] = {};
    SHA256(der, static_cast<std::size_t>(derLen), digest);
    OPENSSL_free(der);

    return base64Encode(digest, SHA256_DIGEST_LENGTH);
}

} // namespace

TcpTransport::TcpTransport(std::string serverName, std::string caFile)
    : m_sslContext(asio::ssl::context::tls_client)
    , m_serverName(std::move(serverName))
    , m_caFile(std::move(caFile))
{
    SSL_CTX* native = m_sslContext.native_handle();

    // 只允许 TLS 1.3：不允许协商降级
    if (SSL_CTX_set_min_proto_version(native, TLS1_3_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(native, TLS1_3_VERSION) != 1) {
        throw std::runtime_error("无法启用TLS 1.3");
    }

    m_sslContext.set_verify_mode(asio::ssl::verify_peer);
    if (!m_caFile.empty()) {
        m_sslContext.load_verify_file(m_caFile);
    } else {
        // 未指定 CA 文件时回退到系统默认信任库，而不是跳过证书校验。
        // 注意：这**不是** verify_none —— 证书链、有效期与主机名校验仍然全部生效。
        // 生产环境应通过 setSpkiPins + 随包 CA 进一步收紧。
        m_sslContext.set_default_verify_paths();
    }

    m_readBuf.resize(kReadChunk);
}

TcpTransport::~TcpTransport()
{
    close();
}

void TcpTransport::setSpkiPins(std::vector<std::string> pins)
{
    m_spkiPins = std::move(pins);
    m_pinningEnabled = true;
}

void TcpTransport::resetConnectionState()
{
    m_notified = false;
    m_closing = false;
    m_writing = false;
    m_writeQueue.clear();
    m_frameReader.reset();
}

void TcpTransport::startIoThread()
{
    m_thread = std::thread([this]() {
        m_ioThreadId = std::this_thread::get_id();
        m_io.run();
    });
}

void TcpTransport::stopIoThread()
{
    if (m_thread.joinable()) {
        if (std::this_thread::get_id() == m_ioThreadId) {
            // 绝不在 IO 线程 join 自己
            m_thread.detach();
            return;
        }
        m_thread.join();
    }
    m_ioThreadId = std::thread::id{};
}

bool TcpTransport::connect(const std::string& host, std::uint16_t port)
{
    if (m_open.load()) return false;

    // 重连：先收尾上一个连接（会 join 旧 IO 线程），再重启 io_context
    close();
    m_io.restart();
    resetConnectionState();

    const auto gen = m_generation.load();
    m_state = State::Resolving;

    // 每次连接创建全新的 socket 与 TLS stream，不复用已关闭对象
    m_stream = std::make_unique<SslStream>(m_io, m_sslContext);
    if (!SSL_set_tlsext_host_name(m_stream->native_handle(), m_serverName.c_str())) {
        m_stream.reset();
        m_state = State::Closed;
        return false;
    }
    m_stream->set_verify_mode(asio::ssl::verify_peer);
    // 验证顺序（不可颠倒，也不可短路）：
    //   1) OpenSSL 的证书链与有效期校验（preverified）——对链上每一层都会回调
    //   2) 主机名 / SNI 校验     —— 只对叶子证书（depth==0）有意义
    //   3) SPKI pinning（仅在显式启用时）—— 只 pin 叶子证书（depth==0）
    // R2-F02：回调会对证书链的**每一层**触发（叶子 depth==0、中间/根 depth>0）。
    // 主机名与 pin 都只针对叶子证书；若对 depth>0 也做，会用中间/根证书去比对叶子
    // 主机名/pin，导致任何多级证书链都被误拒。因此非叶子层只校验 OpenSSL 的链结果。
    m_stream->set_verify_callback([this](bool preverified, asio::ssl::verify_context& ctx) {
        if (!preverified) return false;

        const int depth = X509_STORE_CTX_get_error_depth(ctx.native_handle());
        if (depth != 0) {
            // 中间证书 / 根证书：OpenSSL 已校验其签发关系与有效期，这里直接放行
            return true;
        }

        // ---- 以下只对叶子证书（depth==0）执行 ----
        if (!asio::ssl::host_name_verification(m_serverName)(true, ctx)) return false;
        if (!m_pinningEnabled) return true;

        // 启用 pinning 却没有可用 pin：fail-close，绝不退化成"不校验"
        if (m_spkiPins.empty()) return false;

        X509* cert = X509_STORE_CTX_get_current_cert(ctx.native_handle());
        const std::string pin = spkiSha256Base64(cert);
        if (pin.empty()) return false;
        for (const auto& expected : m_spkiPins) {
            if (expected == pin) return true;
        }
        return false;
    });

    auto resolver = std::make_shared<asio::ip::tcp::resolver>(m_io);
    auto done = std::make_shared<std::promise<bool>>();
    auto future = done->get_future();

    asio::post(m_io, [this, gen, host, port, resolver, done]() {
        resolver->async_resolve(
            host, std::to_string(port),
            [this, gen, done](const asio::error_code& ec,
                              asio::ip::tcp::resolver::results_type results) {
                if (ec || gen != m_generation.load()) { done->set_value(false); return; }
                m_state = State::Connecting;
                asio::async_connect(
                    m_stream->lowest_layer(), results,
                    [this, gen, done](const asio::error_code& ec2,
                                      const asio::ip::tcp::endpoint&) {
                        if (ec2 || gen != m_generation.load()) { done->set_value(false); return; }
                        m_state = State::TlsHandshaking;
                        m_stream->async_handshake(
                            asio::ssl::stream_base::client,
                            [this, gen, done](const asio::error_code& ec3) {
                                if (ec3 || gen != m_generation.load()) {
                                    if (ec3) {
                                        std::cerr << "[TLS] 握手失败 error=" << ec3.message()
                                                  << std::endl;
                                    }
                                    done->set_value(false);
                                    return;
                                }
                                m_state = State::Connected;
                                m_open = true;
                                doRead();
                                done->set_value(true);
                            });
                    });
            });
    });

    startIoThread();

    if (future.wait_for(kConnectTimeout) != std::future_status::ready) {
        std::cerr << "[TLS] 连接超时" << std::endl;
        close();
        return false;
    }
    const bool ok = future.get();
    if (!ok) {
        // 连接失败：收尾 IO 线程，保证同一实例可安全重试
        m_open = false;
        m_state = State::Closed;
        stopIoThread();
        return false;
    }
    return true;
}

TcpTransport::SendResult TcpTransport::send(const char* data, std::size_t len, SendCallback cb)
{
    if (!m_open.load()) return SendResult::NotConnected;
    if (!data || len == 0) return SendResult::TooLarge;

    // 先检查再转换，避免 uint32 截断后发出错误包长
    if (len < FrameCodec::kTypeFieldSize || len > FrameCodec::kMaxBodyLen) {
        return SendResult::TooLarge;
    }
    // 调用方传入的是**完整 body**（已含 4B 协议号），而 body_len 的语义正是
    // 「含协议号、不含自身长度字段」，因此直接等于 len；不能再加一次 kTypeFieldSize。
    const auto bodyLen = static_cast<std::uint32_t>(len);
    // send 传入的 body 已含 4B 协议号，这里只补长度前缀：[4B 大端长度][body]
    auto frame = std::make_shared<const std::vector<char>>();
    {
        std::vector<char> tmp;
        tmp.resize(FrameCodec::kLengthFieldSize + bodyLen);
        FrameCodec::encodeLength(bodyLen, tmp.data());
        std::memcpy(tmp.data() + FrameCodec::kLengthFieldSize, data, len);
        frame = std::make_shared<const std::vector<char>>(std::move(tmp));
    }

    asio::post(m_io, [this, frame, cb = std::move(cb)]() mutable {
        if (!m_open.load() || m_closing) {
            if (cb) cb(SendResult::Cancelled);
            return;
        }
        m_writeQueue.push_back(PendingWrite{std::move(frame), std::move(cb)});
        if (!m_writing) doWrite();
    });
    return SendResult::Ok;
}

void TcpTransport::postToIo(std::function<void()> task)
{
    if (!task) return;
    // 统一投递到 m_io：与 send()/doWrite()/doRead() 同一执行序列，天然串行。
    asio::post(m_io, std::move(task));
}

void TcpTransport::doWrite()
{
    if (m_writeQueue.empty() || m_writing) return;
    m_writing = true;

    // 把「正在写的项」移出队列：此后 m_writeQueue 只剩等待中的项，
    // 保证 drainQueue 不会重复回调它，也不会出现 pop 空队列。
    m_currentWrite = std::move(m_writeQueue.front());
    m_writeQueue.pop_front();

    auto frame = m_currentWrite.frame; // 拷贝 shared_ptr，保证异步期间内存有效

    asio::async_write(*m_stream, asio::buffer(frame->data(), frame->size()),
        [this, frame](const asio::error_code& ec, std::size_t) {
            m_writing = false;
            // 取出并清空当前项，确保每个发送项**有且仅有一次**回执
            const PendingWrite item = std::move(m_currentWrite);
            m_currentWrite = PendingWrite{};

            if (ec) {
                // 写失败：整条连接作废，剩余等待项统一 Disconnected
                if (item.cb) item.cb(SendResult::Disconnected);
                drainQueue(SendResult::Disconnected);
                failClose();
                return;
            }
            if (item.cb) item.cb(SendResult::Ok);

            if (!m_writeQueue.empty()) {
                doWrite();
            } else if (m_closing) {
                shutdownSocket();
            }
        });
}

void TcpTransport::doRead()
{
    if (!m_open.load() || !m_stream) return;
    const auto gen = m_generation.load();

    m_stream->async_read_some(
        asio::buffer(m_readBuf.data(), m_readBuf.size()),
        [this, gen](const asio::error_code& ec, std::size_t n) {
            // 旧连接的迟到回调：直接丢弃，不碰任何新连接状态
            if (gen != m_generation.load()) return;
            if (ec) {
                if (ec == asio::error::operation_aborted) return;
                failClose();
                return;
            }

            std::vector<FrameReader::Frame> frames;
            const auto status = m_frameReader.feed(m_readBuf.data(), n, frames);
            if (status == FrameReader::Status::InvalidLength) {
                std::cerr << "[FRAME] 非法包长，关闭连接" << std::endl;
                failClose();
                return;
            }

            for (auto& f : frames) {
                if (m_onPacket) {
                    m_onPacket(f.type, f.payload.data(), f.payload.size());
                }
            }
            doRead();
        });
}

void TcpTransport::drainQueue(SendResult result)
{
    while (!m_writeQueue.empty()) {
        auto item = std::move(m_writeQueue.front());
        m_writeQueue.pop_front();
        if (item.cb) item.cb(result);
    }
}

void TcpTransport::shutdownSocket()
{
    if (!m_stream) return;
    asio::error_code ec;
    m_stream->lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    m_stream->lowest_layer().close(ec);
}

void TcpTransport::failClose()
{
    if (!m_open.exchange(false)) return;
    m_state = State::Closed;
    shutdownSocket();
    drainQueue(SendResult::Disconnected);
    notifyClose();
}

void TcpTransport::close()
{
    const bool inIoThread = (std::this_thread::get_id() == m_ioThreadId);

    // 递增 generation：让所有在飞的旧 handler 失效
    m_generation.fetch_add(1);
    m_open = false;

    if (m_state.load() == State::Idle && !m_thread.joinable()) {
        m_state = State::Closed;
        return;
    }

    m_state = State::Closing;

    if (inIoThread) {
        // 在 IO 线程回调中调用 close：只投递收尾动作，绝不 join 自身
        asio::post(m_io, [this]() {
            drainQueue(SendResult::Cancelled);
            shutdownSocket();
        });
        return;
    }

    asio::post(m_io, [this]() {
        drainQueue(SendResult::Cancelled);
        shutdownSocket();
    });
    stopIoThread();
    // IO 线程已结束：兜底清空（post 未执行时不会有并发）
    drainQueue(SendResult::Cancelled);

    m_state = State::Closed;
    notifyClose();
}

void TcpTransport::notifyClose()
{
    m_open = false;
    if (m_notified.exchange(true)) return; // 只通知一次
    if (m_onClose) m_onClose();
}

} // namespace im
