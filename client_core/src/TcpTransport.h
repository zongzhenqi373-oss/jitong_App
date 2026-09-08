#pragma once
// 基于 standalone asio 的 TCP 传输层（P3 重构版）。
//
// 线格式由 transport/FrameCodec 唯一实现，本类只负责字节搬运与生命周期。
//
// ## 关键设计（对应 P3-T01 / P3-T03）
//
// 1. **每次 connect() 创建全新的 resolver / socket / TLS stream**，绝不复用已关闭对象。
//    旧实现在构造函数里创建 stream，一旦握手失败该实例就永久报废。
// 2. **重连前 io_context.restart()**，并清空上一个连接的读写、队列与 FrameReader 状态。
// 3. **明确状态机**：Idle → Resolving → Connecting → TlsHandshaking → Connected → Closing → Closed。
// 4. **connection generation**：每个异步 handler 捕获 generation；旧连接的迟到回调
//    发现 generation 不匹配时直接丢弃，不会污染新连接。
// 5. **close() 可从任意线程调用，包括 IO 线程回调中**：在 IO 线程内只投递关闭动作，
//    绝不 join 自身（join 自身会直接崩溃）。
// 6. **单写队列**：所有发送 post 到同一 io_context；任意时刻最多一个 async_write；
//    close 后队列项统一回调 Cancelled，不会悬挂。
// 7. **关闭通知只产生一次**（远端断开 / 握手失败 / 主动 close 都汇到 notifyClose）。
//
// 线程模型：内部一个 IO 线程；packet / close 回调均在该线程触发。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <asio.hpp>
#include <asio/ssl.hpp>

#include "client_core/Protocol.h"
#include "transport/FrameCodec.h"

namespace im {

// 线格式实现来自 im::transport，这里引入别名避免调用方写全限定名
using transport::FrameCodec;
using transport::FrameReader;

class TcpTransport {
public:
    enum class State {
        Idle,
        Resolving,
        Connecting,
        TlsHandshaking,
        Connected,
        Closing,
        Closed,
    };

    enum class SendResult {
        Ok,           // 已入队，稍后写出
        NotConnected, // 当前未连接，未入队
        TooLarge,     // 超过单包上限，未入队
        Cancelled,    // 关闭时队列项被取消
        Disconnected, // 写失败/远端断开
    };

    /**
     * 包回调：**已由 FrameCodec 解析**，直接给出协议号与 payload，
     * 业务层不再触碰端序，保证线格式实现唯一。
     */
    using PacketHandler = std::function<void(proto::protType type, const char* payload, std::size_t len)>;
    using VoidHandler = std::function<void()>;
    using SendCallback = std::function<void(SendResult)>;

    TcpTransport(std::string serverName, std::string caFile);
    ~TcpTransport();

    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    void setPacketHandler(PacketHandler h) { m_onPacket = std::move(h); }
    void setCloseHandler(VoidHandler h) { m_onClose = std::move(h); }

    /**
     * 设置 SPKI SHA-256 pin（base64，公钥而非整证书）。
     *
     * 调用本方法即表示**启用** pinning：证书链/域名校验通过之后，还必须命中其中一个 pin。
     * 因此传入空集合会导致所有连接被拒 —— 这正是"空 pin 集"负向用例的预期行为，
     * 而不是退化成"不校验"。
     */
    void setSpkiPins(std::vector<std::string> pins);

    /** 同步连接；成功后启动 IO 线程并开始读。失败后可在本实例上安全重试。 */
    bool connect(const std::string& host, std::uint16_t port);

    /** 发送一个完整包体（内部按 FrameCodec 组帧）。任意线程可调用。 */
    SendResult send(const char* data, std::size_t len, SendCallback cb = nullptr);

    /**
     * 把任意任务投递到内部 IO 线程（单线程 executor）串行执行。任意线程可调用。
     *
     * 用途（P4/R2-F01）：出站业务帧的「分配 sequence + 加密 + 组 body + 入队」必须与
     * 入站解密、握手状态变更在**同一线程序列**里发生，否则并发调用 sendPacket 会对
     * 加密序号造成数据竞争，进而导致 AES-GCM nonce 重用或服务端按严格 +1 校验时
     * fail-close。调用方把整段加密逻辑用本方法 post 进来即可获得串行化保证。
     *
     * 注意：任务在 IO 线程执行；若在连接建立前投递，会排队至 IO 线程运行后执行。
     */
    void postToIo(std::function<void()> task);

    /** 关闭并停止 IO 线程（幂等，可从任意线程调用）。 */
    void close();

    bool isOpen() const { return m_open.load(); }
    State state() const { return m_state.load(); }

    /** 当前连接代号；每次连接/关闭递增，用于识别迟到回调（测试与诊断用）。 */
    std::uint64_t generation() const { return m_generation.load(); }

private:
    using SslStream = asio::ssl::stream<asio::ip::tcp::socket>;

    struct PendingWrite {
        std::shared_ptr<const std::vector<char>> frame; // 完整帧（含 4B 长度）
        SendCallback cb;
    };

    void startIoThread();
    void stopIoThread();
    void doRead();
    void doWrite();
    void drainQueue(SendResult result);
    void shutdownSocket();
    void failClose();
    void notifyClose();
    void resetConnectionState();

    asio::io_context m_io;
    asio::ssl::context m_sslContext;
    std::unique_ptr<SslStream> m_stream; // 每次连接重建
    std::thread m_thread;
    std::thread::id m_ioThreadId{};

    std::string m_serverName;
    std::string m_caFile;

    // SPKI pinning：m_pinningEnabled 为 true 时才校验；空 pin 集一律拒绝
    std::vector<std::string> m_spkiPins;
    bool m_pinningEnabled = false;

    std::atomic<bool> m_open{false};
    std::atomic<bool> m_notified{false};
    std::atomic<State> m_state{State::Idle};
    std::atomic<std::uint64_t> m_generation{0};

    // 以下成员只在 IO 线程访问（send 通过 post 切入 IO 线程）
    std::deque<PendingWrite> m_writeQueue;  // 仅保存「等待中」的项
    PendingWrite m_currentWrite;            // 正在写的项已从队列移出，避免重复回调
    bool m_writing = false;
    bool m_closing = false;
    std::vector<char> m_readBuf;
    FrameReader m_frameReader;

    PacketHandler m_onPacket;
    VoidHandler m_onClose;
};

} // namespace im
