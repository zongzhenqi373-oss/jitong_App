#include "TcpTransport.h"
#include "client_core/Protocol.h"

#include <openssl/ssl.h>
#include <openssl/tls1.h>
#include <iostream>

namespace im {

TcpTransport::TcpTransport(
    std::string serverName,
    std::string caFile
)
    : m_sslContext(asio::ssl::context::tls_client)
    , m_stream(m_io, m_sslContext)
    , m_serverName(std::move(serverName))
    , m_caFile(std::move(caFile))
{
    SSL_CTX* native = m_sslContext.native_handle();
    
    if(SSL_CTX_set_min_proto_version(
        native,
        TLS1_3_VERSION
    ) != 1){
        throw std::runtime_error("无法启用TLS 1.3");
    }

    m_sslContext.set_verify_mode(asio::ssl::verify_peer);
    m_sslContext.load_verify_file(m_caFile);

    m_stream.set_verify_mode(asio::ssl::verify_peer);
    m_stream.set_verify_callback(asio::ssl::host_name_verification(m_serverName));
}

TcpTransport::~TcpTransport()
{
    close();
}

bool TcpTransport::connect(const std::string& host, std::uint16_t port)
{
    if (m_open.load()) return false;

    asio::error_code ec;
    asio::ip::tcp::resolver resolver(m_io);
    auto endpoints = resolver.resolve(host, std::to_string(port), ec);
    if (ec) return false;

    asio::connect(m_stream.lowest_layer(), endpoints, ec);
    if (ec) return false;

    if(!SSL_set_tlsext_host_name(
        m_stream.native_handle(),
        m_serverName.c_str()
    )){
        return false;
    }
    m_stream.handshake(asio::ssl::stream_base::client, ec);

    if (ec){
        //打印客户端握手信息
        std::cerr
        << "[TLS] 客户端握手失败 error="
        << ec.message()
        << std::endl;

        asio::error_code closeEc;
        m_stream.lowest_layer().close(closeEc);
        return false;
    }else{
        //打印客户端握手信息
        SSL* ssl = m_stream.native_handle();
        std::cout
            << "[TLS] 客户端握手成功"
            << " version=" << SSL_get_version(ssl)
            << " cipher=" << SSL_get_cipher_name(ssl)
            << " serverName=" << m_serverName
            << std::endl;
    }

    m_open = true;
    doReadHeader();

    m_thread = std::thread([this]() { m_io.run(); });
    return true;
}

void TcpTransport::send(const char* data, std::size_t len)
{
    if (!m_open.load() || !data || len == 0 || len > proto::MAX_PACK_LEN) return;

    // 拼好 [包长][包体] 整体一次写出，避免两次写产生交错
    auto buf = std::make_shared<std::vector<char>>(4 + len);
    encodeLen32(static_cast<std::uint32_t>(len), buf->data());
    std::memcpy(buf->data() + 4, data, len);

    asio::post(m_io, [this, buf]() {
        bool busy = !m_writeQueue.empty();
        m_writeQueue.push_back(std::move(buf));
        if (!busy) doWrite();
    });
}

void TcpTransport::close()
{
    const bool wasOpen = m_open.exchange(false);
    if (wasOpen) {
        asio::post(m_io, [this]() {
            // 发送队列未空则标记关闭中，由 doWrite 发完最后一个包后关 socket
            // （保证下线通知等"发完即关"场景的包真正发出）
            if (!m_writeQueue.empty()) {
                m_closing = true;
                return;
            }
            asio::error_code ec;
            m_stream.lowest_layer().close(ec);
        });
    }
    if (m_thread.joinable()) m_thread.join();
    m_io.stop();
    if (wasOpen) notifyClose();
}

void TcpTransport::doReadHeader()
{
    asio::async_read(m_stream, asio::buffer(m_hdrBuf),
        [this](const asio::error_code& ec, std::size_t) {
            if (ec) { notifyClose(); return; }
            const std::uint32_t bodyLen = decodeLen32(m_hdrBuf.data());
            // 长度上限保护：非法长度直接断连
            if (bodyLen == 0 || bodyLen > proto::MAX_PACK_LEN) { notifyClose(); return; }
            doReadBody(bodyLen);
        });
}

void TcpTransport::doReadBody(std::uint32_t bodyLen)
{
    m_bodyBuf.resize(bodyLen);
    asio::async_read(m_stream, asio::buffer(m_bodyBuf.data(), bodyLen),
        [this](const asio::error_code& ec, std::size_t) {
            if (ec) { notifyClose(); return; }
            if (m_onPacket) m_onPacket(m_bodyBuf.data(), m_bodyBuf.size());
            doReadHeader();
        });
}

void TcpTransport::doWrite()
{
    asio::async_write(m_stream, asio::buffer(*m_writeQueue.front()),
        [this](const asio::error_code& ec, std::size_t) {
            if (ec) { notifyClose(); return; }
            m_writeQueue.pop_front();
            if (!m_writeQueue.empty()) {
                doWrite();
            } else if (m_closing) {
                asio::error_code ec2;
                m_stream.lowest_layer().close(ec2);
            }
        });
}

void TcpTransport::notifyClose()
{
    m_open = false;
    if (m_notified.exchange(true)) return;
    if (m_onClose) m_onClose();
}

} // namespace im
