#pragma once
// 单连接会话：asio 异步二段式帧读写 + strand 串行化
// 帧格式：[4B 大端包长][包体]，包体 = [4B 小端协议号][pb payload]
// 并发模型：所有读写处理器都在本连接的 strand 上执行 → 连接内天然无锁。
// 关闭权收口：close() 幂等，连接消亡时回调 Server::onSessionClosed。

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <asio.hpp>
#include <asio/ssl.hpp>

namespace imsrv {

class Server;

// 大端包长编解码（与 client_core TcpTransport 一致，不依赖平台 htonl）
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
// 小端协议号编解码（线格式固定小端）
inline void encodeType32(std::uint32_t v, char* out)
{
    out[0] = static_cast<char>(v & 0xFF);
    out[1] = static_cast<char>((v >> 8) & 0xFF);
    out[2] = static_cast<char>((v >> 16) & 0xFF);
    out[3] = static_cast<char>((v >> 24) & 0xFF);
}
inline std::uint32_t decodeType32(const char* p)
{
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return static_cast<std::uint32_t>(u[0]) |
           (static_cast<std::uint32_t>(u[1]) << 8) |
           (static_cast<std::uint32_t>(u[2]) << 16) |
           (static_cast<std::uint32_t>(u[3]) << 24);
}

class Session : public std::enable_shared_from_this<Session> {
public:
    // biz：本会话的业务 strand（Server 按会话一致性分配，同会话固定）
    Session(asio::ip::tcp::socket socket, 
            Server& server,
            asio::ssl::context& sslContext,
            std::shared_ptr<asio::strand<asio::thread_pool::executor_type>> biz);
    ~Session() = default;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void start();

    // 发送一个完整包体（4B 协议号 + payload），内部加包长前缀；任意线程可调用
    void deliver(std::uint32_t type, const std::string& payload);

    // 关闭连接（幂等）
    void close();

    int userId() const { return m_userId.load(); }
    void setUserId(int id) { m_userId.store(id); }

    bool authenticated() const
    {
        return m_userId.load() > 0;
    }

    const std::string& authSessionId() const
    {
        return m_authSessionId;
    }

    const std::string& deviceId() const
    {
        return m_deviceId;
    }

    std::int64_t accessExpiresAt() const
    {
        return m_accessExpiresAt.load();
    }

    void bindAuth(
        int userId,
        std::string authSessionId,
        std::string deviceId,
        std::int64_t expiresAt
    ) {
        m_userId.store(userId);
        m_authSessionId =
            std::move(authSessionId);
        m_deviceId =
            std::move(deviceId);
        m_accessExpiresAt.store(expiresAt);
    }

    void closeAfterWrite();

private:
    void doReadHeader();
    void doReadBody(std::uint32_t bodyLen);
    void doWrite();
    void onClosed(); // 连接收尾（仅一次）
    void doHandshake();   //握手逻辑

    asio::ssl::stream<asio::ip::tcp::socket> m_streamsocket;
    // asio>=1.17 的 tcp::socket::executor_type 为 any_io_executor，strand 类型须匹配
    asio::strand<asio::any_io_executor> m_strand; // IO strand：读写处理器串行
    std::shared_ptr<asio::strand<asio::thread_pool::executor_type>> m_biz; // 业务 strand
    Server& m_server;
    bool m_closeAfterWrite = false;

    std::atomic<int> m_userId{0};  // 业务 strand 写、io 线程读
    bool m_closedNotified = false; // 在 strand 上访问

    std::array<char, 4> m_hdrBuf{};
    std::vector<char> m_bodyBuf;
    std::deque<std::shared_ptr<std::vector<char>>> m_writeQueue;

    std::string m_authSessionId;
    std::string m_deviceId;
    std::atomic<std::int64_t>
    m_accessExpiresAt{0};
};

} // namespace imsrv
