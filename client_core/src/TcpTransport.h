#pragma once
// 基于 standalone asio 的 TCP 传输层（跨平台，替代原 WinSock TCPClient）
// 协议：4 字节大端包长前缀 + 包体（与服务端一致）
// 线程模型：内部运行一个 io_context 线程；包回调与断连回调均在该线程触发。

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <asio.hpp>
#include <asio/ssl.hpp>

namespace im {

// 手动编解码 4 字节大端长度，避免依赖平台 htonl/ntohl
inline void encodeLen32(std::uint32_t v, char* out)
{
    out[0] = static_cast<char>((v >> 24) & 0xFF);
    out[1] = static_cast<char>((v >> 16) & 0xFF);
    out[2] = static_cast<char>((v >> 8) & 0xFF);
    out[3] = static_cast<char>(v & 0xFF);
}

inline std::uint32_t decodeLen32(const char* p)
{
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return (static_cast<std::uint32_t>(u[0]) << 24) |
           (static_cast<std::uint32_t>(u[1]) << 16) |
           (static_cast<std::uint32_t>(u[2]) << 8) |
           static_cast<std::uint32_t>(u[3]);
}

class TcpTransport {
public:
    using PacketHandler = std::function<void(const char* data, std::size_t len)>;
    using VoidHandler = std::function<void()>;

    TcpTransport(
        std::string serverName,
        std::string caFile
    );
    ~TcpTransport();

    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    // 设置回调（连接前调用）
    void setPacketHandler(PacketHandler h) { m_onPacket = std::move(h); }
    void setCloseHandler(VoidHandler h) { m_onClose = std::move(h); }

    // 同步连接（成功返回后启动接收线程），失败返回 false
    bool connect(const std::string& host, std::uint16_t port);

    // 异步发送完整包体（内部自动加 4 字节大端包长前缀），任意线程可调用
    void send(const char* data, std::size_t len);

    // 关闭连接并停止线程（幂等）
    void close();

    bool isOpen() const { return m_open.load(); }

private:
    void doReadHeader();
    void doReadBody(std::uint32_t bodyLen);
    void doWrite();
    void notifyClose(); // 保证断连回调只触发一次

    asio::io_context m_io;
    asio::ssl::context m_sslContext;
    asio::ssl::stream<asio::ip::tcp::socket> m_stream;
    std::thread m_thread;
    std::string m_serverName;
    std::string m_caFile;

    std::atomic<bool> m_open{false};
    std::atomic<bool> m_notified{false};

    // 以下成员仅在 io 线程访问（send 通过 post 切入 io 线程）
    std::deque<std::shared_ptr<std::vector<char>>> m_writeQueue;
    bool m_closing = false; // close() 已发起但发送队列未空，等排空后关 socket
    std::array<char, 4> m_hdrBuf{};
    std::vector<char> m_bodyBuf;

    PacketHandler m_onPacket;
    VoidHandler m_onClose;
};

} // namespace im
