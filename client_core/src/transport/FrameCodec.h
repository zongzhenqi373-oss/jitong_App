#pragma once
// 帧编解码：整个工程**唯一**的线格式实现。
//
// 传输层与测试必须共用这一份，禁止各自复制一套端序/长度逻辑（P3-T02）。
//
// 线格式（与服务端 Session、Android Frame.kt 完全一致）：
// ```text
// [4B 大端 body_len = 4 + payload_size][4B 小端 protocol_type][protobuf payload]
// ```
// - body_len 语义：包含 4 字节协议号，**不包含**自身 4 字节；
// - 合法范围：`4 <= body_len <= 10 MiB`，越界立即 fail-close；
// - 包长损坏后**不**在字节流里扫描“下一个包头”，直接关闭本次连接。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "client_core/Protocol.h"

namespace im {
namespace transport {

class FrameCodec {
public:
    static constexpr std::size_t kLengthFieldSize = 4; // 包长字段
    static constexpr std::size_t kTypeFieldSize = 4;   // 协议号字段
    static constexpr std::uint32_t kMaxBodyLen = static_cast<std::uint32_t>(proto::MAX_PACK_LEN);
    // body 至少要能容纳协议号；小于它说明包长字段已损坏
    static constexpr std::uint32_t kMinBodyLen = static_cast<std::uint32_t>(kTypeFieldSize);

    /** 4 字节大端长度编码（不依赖 htonl，跨平台一致）。 */
    static void encodeLength(std::uint32_t bodyLen, char* out) noexcept;

    /** 4 字节大端长度解码。 */
    static std::uint32_t decodeLength(const char* p) noexcept;

    /** body_len 合法性：必须能容纳协议号，且不超过上限。 */
    static bool isValidBodyLen(std::uint32_t bodyLen) noexcept
    {
        return bodyLen >= kMinBodyLen && bodyLen <= kMaxBodyLen;
    }

    /**
     * 由 body_len 得到 payload 长度。
     * 使用安全减法：调用前必须先过 isValidBodyLen，这里再兜一次底防止误用。
     */
    static std::size_t payloadSize(std::uint32_t bodyLen) noexcept
    {
        if (bodyLen < kMinBodyLen) return 0;
        return static_cast<std::size_t>(bodyLen) - kTypeFieldSize;
    }

    /** 组帧：返回 [4B 大端长度][4B 小端协议号][payload]。 */
    static std::string encodeFrame(proto::protType type, const std::string& payload);

    struct ParsedFrame {
        proto::protType type = 0;
        const char* payload = nullptr; // 指向 body 内部，非拥有
        std::size_t payloadLen = 0;
    };

    /**
     * 解析完整 body（长度已校验，内容为 [4B 小端协议号][payload]）。
     * @return 解析成功返回 true；bodyLen 非法或过短返回 false。
     */
    static bool parseBody(const char* body, std::size_t bodyLen, ParsedFrame& out) noexcept;
};

/**
 * 增量帧读取器：把任意分片的字节流还原成完整帧。
 *
 * 解决两类经典问题，且传输层与测试共用同一份逻辑：
 *   - **半包**：Header 每次到 1 字节、Body 每次到 1~7 字节 → 聚合够了才产出；
 *   - **粘包**：一次到达两帧甚至更多 → 逐帧产出，不串包。
 *
 * 一旦包长非法立刻返回 `InvalidLength`，调用方必须关闭连接，
 * **不允许**在残留字节流里继续扫描“可能的下一个包头”。
 */
class FrameReader {
public:
    enum class Status {
        NeedMore,      // 数据不足，继续喂
        FrameReady,    // 至少解出一帧
        InvalidLength, // 包长非法，必须断线
    };

    struct Frame {
        proto::protType type = 0;
        std::vector<char> payload;
    };

    /** 追加数据；解出的帧追加到 out。 */
    Status feed(const char* data, std::size_t len, std::vector<Frame>& out);

    /** 丢弃内部缓冲（重连时必须调用）。 */
    void reset() { m_pending.clear(); }

    std::size_t pendingBytes() const { return m_pending.size(); }

private:
    std::vector<char> m_pending;
};

} // namespace transport
} // namespace im
