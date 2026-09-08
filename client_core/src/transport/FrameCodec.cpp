#include "transport/FrameCodec.h"

#include <cstring>

namespace im {
namespace transport {

void FrameCodec::encodeLength(std::uint32_t bodyLen, char* out) noexcept
{
    out[0] = static_cast<char>((bodyLen >> 24) & 0xFF);
    out[1] = static_cast<char>((bodyLen >> 16) & 0xFF);
    out[2] = static_cast<char>((bodyLen >> 8) & 0xFF);
    out[3] = static_cast<char>(bodyLen & 0xFF);
}

std::uint32_t FrameCodec::decodeLength(const char* p) noexcept
{
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return (static_cast<std::uint32_t>(u[0]) << 24) |
           (static_cast<std::uint32_t>(u[1]) << 16) |
           (static_cast<std::uint32_t>(u[2]) << 8) |
           static_cast<std::uint32_t>(u[3]);
}

std::string FrameCodec::encodeFrame(proto::protType type, const std::string& payload)
{
    // 长度先检查再转换：payload 过大时直接抛，避免 uint32 截断后发出错误包长
    const std::uint64_t bodyLen64 = static_cast<std::uint64_t>(kTypeFieldSize) + payload.size();
    if (bodyLen64 > kMaxBodyLen) {
        throw std::length_error("frame body length exceeds MAX_PACK_LEN");
    }
    const auto bodyLen = static_cast<std::uint32_t>(bodyLen64);

    std::string out;
    out.resize(kLengthFieldSize + bodyLen);
    encodeLength(bodyLen, out.data());
    proto::encodeType32(type, out.data() + kLengthFieldSize);
    if (!payload.empty()) {
        std::memcpy(out.data() + kLengthFieldSize + kTypeFieldSize, payload.data(), payload.size());
    }
    return out;
}

bool FrameCodec::parseBody(const char* body, std::size_t bodyLen, ParsedFrame& out) noexcept
{
    if (body == nullptr || bodyLen < kTypeFieldSize) return false;
    // body_len 来自线上，可能是任意 uint32；先按 uint32 校验再比较，避免 size_t 提升歧义
    if (bodyLen > kMaxBodyLen) return false;

    out.type = proto::decodeType32(body);
    out.payload = body + kTypeFieldSize;
    out.payloadLen = bodyLen - kTypeFieldSize;
    return true;
}

// ---------------- FrameReader ----------------

FrameReader::Status FrameReader::feed(const char* data, std::size_t len, std::vector<Frame>& out)
{
    if (len > 0) {
        if (data == nullptr) return Status::InvalidLength;
        m_pending.insert(m_pending.end(), data, data + len);
    }

    bool produced = false;
    std::size_t consumed = 0;
    const std::size_t available = m_pending.size();

    while (true) {
        const std::size_t remaining = available - consumed;
        if (remaining < FrameCodec::kLengthFieldSize) break;

        const std::uint32_t bodyLen = FrameCodec::decodeLength(m_pending.data() + consumed);
        if (!FrameCodec::isValidBodyLen(bodyLen)) {
            // 包长损坏：不扫描后续字节，整条连接作废
            m_pending.clear();
            return Status::InvalidLength;
        }

        const std::size_t frameLen = FrameCodec::kLengthFieldSize + static_cast<std::size_t>(bodyLen);
        if (remaining < frameLen) break; // Body 还没到齐

        // 显式解析 + 拷贝到独立 buffer：帧数据拥有自己的内存，
        // 后续从 m_pending 擦除时不会悬空。
        Frame f;
        const char* body = m_pending.data() + consumed + FrameCodec::kLengthFieldSize;
        f.type = proto::decodeType32(body);
        const auto payloadBegin =
            m_pending.begin() +
            static_cast<std::ptrdiff_t>(consumed + FrameCodec::kLengthFieldSize + FrameCodec::kTypeFieldSize);
        f.payload.assign(payloadBegin, payloadBegin + static_cast<std::ptrdiff_t>(bodyLen - FrameCodec::kTypeFieldSize));
        out.push_back(std::move(f));

        consumed += frameLen;
        produced = true;
    }

    if (consumed > 0) {
        m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
    return produced ? Status::FrameReady : Status::NeedMore;
}

} // namespace transport
} // namespace im
