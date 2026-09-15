// 假传输层（P7-G1 Harness）。
//
// 目的：替代真实 TCP/TLS。测试可：
//   - 断言"发出了哪些帧"（帧 = 4B 协议号 + payload）；
//   - 注入入站帧（模拟服务端推送/响应）；
//   - 注入断连、IO 错误；
//   - 不依赖真实网络与时序。

#ifndef CLIENT_CORE_TESTING_FAKE_TRANSPORT_H
#define CLIENT_CORE_TESTING_FAKE_TRANSPORT_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace im {
namespace testing {

class FakeTransport {
public:
    struct SentFrame {
        std::uint32_t type = 0;
        std::string payload;
    };

    using ReceiveFn = std::function<void(std::uint32_t type, const std::string& payload)>;

    void setReceiveHandler(ReceiveFn fn) { m_onReceive = std::move(fn); }

    bool send(std::uint32_t type, const std::string& payload)
    {
        if (!m_connected) return false;
        if (m_ioError) return false;
        m_sent.push_back(SentFrame{type, payload});
        return true;
    }

    /** 模拟服务端下发一帧。 */
    void injectInbound(std::uint32_t type, const std::string& payload)
    {
        if (m_onReceive) m_onReceive(type, payload);
    }

    bool connect()
    {
        if (m_connectFails) return false;
        m_connected = true;
        return true;
    }

    void disconnect() { m_connected = false; }
    bool isConnected() const { return m_connected; }

    // ---- 故障注入 ----
    void setIoError(bool err) { m_ioError = err; }
    void setConnectFails(bool fails) { m_connectFails = fails; }

    // ---- 断言辅助 ----
    const std::vector<SentFrame>& sentFrames() const { return m_sent; }
    std::size_t sentCount() const { return m_sent.size(); }

    /** 是否发出过指定类型的帧。 */
    bool sentType(std::uint32_t type) const
    {
        for (const auto& f : m_sent) {
            if (f.type == type) return true;
        }
        return false;
    }

    /** 指定类型的发送次数（用于"刷新只发一次"等断言）。 */
    std::size_t countType(std::uint32_t type) const
    {
        std::size_t n = 0;
        for (const auto& f : m_sent) {
            if (f.type == type) ++n;
        }
        return n;
    }

    void reset()
    {
        m_sent.clear();
        m_connected = false;
        m_ioError = false;
        m_connectFails = false;
    }

private:
    std::vector<SentFrame> m_sent;
    ReceiveFn m_onReceive;
    bool m_connected = false;
    bool m_ioError = false;
    bool m_connectFails = false;
};

} // namespace testing
} // namespace im

#endif // CLIENT_CORE_TESTING_FAKE_TRANSPORT_H
