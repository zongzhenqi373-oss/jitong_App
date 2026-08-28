#include "Session.h"
#include "Server.h"

#include <cstring>
#include <iostream>
#include <openssl/ssl.h>


namespace imsrv {

// 单包体上限（与 client_core MAX_PACK_LEN 一致）：防异常/恶意超大包
constexpr std::uint32_t MAX_PACK_LEN = 10 * 1024 * 1024;

Session::Session(asio::ip::tcp::socket socket, Server& server, asio::ssl::context& sslContext,
                 std::shared_ptr<asio::strand<asio::thread_pool::executor_type>> biz)
    : m_streamsocket(std::move(socket),sslContext)
    , m_strand(asio::make_strand(m_streamsocket.get_executor()))
    , m_biz(std::move(biz))
    , m_server(server)
{
}

void Session::start()
{
    doHandshake();
}

void Session::deliver(std::uint32_t type, const std::string& payload)
{
    auto self = shared_from_this();
    // 组帧：[4B 大端包长][4B 小端协议号][payload]
    auto buf = std::make_shared<std::vector<char>>(8 + payload.size());
    encodeLen32(static_cast<std::uint32_t>(4 + payload.size()), buf->data());
    encodeType32(type, buf->data() + 4);
    std::memcpy(buf->data() + 8, payload.data(), payload.size());

    asio::post(m_strand, [this, self, buf]() {
        m_writeQueue.push_back(std::move(buf));
        if (m_writeQueue.size() == 1) doWrite();
    });
}

void Session::close()
{
    auto self = shared_from_this();
    asio::post(m_strand, [this, self]() {
        asio::error_code ec;
        m_streamsocket.lowest_layer().close(ec);
        onClosed();
    });
}

void Session::doReadHeader()
{
    auto self = shared_from_this();
    asio::async_read(m_streamsocket, asio::buffer(m_hdrBuf),
        asio::bind_executor(m_strand,
            [this, self](const asio::error_code& ec, std::size_t) {
                if (ec) { onClosed(); return; }
                const std::uint32_t bodyLen = decodeLen32(m_hdrBuf.data());
                if (bodyLen < 4 || bodyLen > MAX_PACK_LEN) { onClosed(); return; }
                doReadBody(bodyLen);
            }));
}

void Session::doReadBody(std::uint32_t bodyLen)
{
    auto self = shared_from_this();
    m_bodyBuf.resize(bodyLen);
    asio::async_read(m_streamsocket, asio::buffer(m_bodyBuf.data(), bodyLen),
        asio::bind_executor(m_strand,
            [this, self, bodyLen](const asio::error_code& ec, std::size_t) {
                if (ec) { onClosed(); return; }
                const std::uint32_t type = decodeType32(m_bodyBuf.data());
                // 投递到本会话业务 strand：同会话业务严格按序、跨会话并行（IO/业务解耦）
                std::string payload(m_bodyBuf.data() + 4, bodyLen - 4);
                asio::post(*m_biz, [this, self, type, payload = std::move(payload)]() mutable {
                    m_server.dispatchPacket(shared_from_this(), type, std::move(payload));
                });
                doReadHeader();
                std::cout
                << "[TLS] 收到已解密业务帧"
                << " type=" << type
                << " payloadBytes=" << bodyLen - 4
                << std::endl;
            }));
}

void Session::doWrite()
{
    // 本函数只允许在 m_strand 中调用。队首 buffer 必须一直保留到 async_write
    // 回调结束；提前 pop 会使 Asio 持有悬空 buffer，并让回调再次 pop 空队列。
    if (m_writeQueue.empty()) {
        if (m_closeAfterWrite) close();
        return;
    }

    auto self = shared_from_this();
    asio::async_write(m_streamsocket, asio::buffer(*m_writeQueue.front()),
        asio::bind_executor(m_strand,
            [this, self](const asio::error_code& ec, std::size_t) {
                if (ec) { onClosed(); return; }
                m_writeQueue.pop_front();
                if (!m_writeQueue.empty()) {
                    doWrite();
                } else if (m_closeAfterWrite) {
                    close();
                }
            }));
}

void Session::onClosed()
{
    // 在 strand 上执行，保证只收尾一次
    if (m_closedNotified) return;
    m_closedNotified = true;

    asio::error_code ec;
    m_streamsocket.lowest_layer().shutdown(
        asio::ip::tcp::socket::shutdown_both,
        ec
    );
    m_streamsocket.lowest_layer().close(ec);
    m_server.onSessionClosed(shared_from_this());
}

void Session::doHandshake()
{
    auto self = shared_from_this();

    m_streamsocket.async_handshake(
        asio::ssl::stream_base::server,
        asio::bind_executor(
            m_strand,
            [this, self](const asio::error_code& ec) {
                if (ec) {
                    std::cerr << "handshake failed: " << ec.message() << std::endl;
                    onClosed();
                    return;
                }

                SSL* ssl = m_streamsocket.native_handle();
                std::cout << "handshake success: " << SSL_get_version(ssl) << "  " 
                << "cipher: " << SSL_get_cipher_name(ssl) << std::endl;
                doReadHeader();
            }
        )
    );
}

void Session::closeAfterWrite()
{
    auto self = shared_from_this();
    asio::post(m_strand, [this, self]() {
        m_closeAfterWrite = true;
        if (m_writeQueue.empty()) 
            close();
    });
}

} // namespace imsrv
