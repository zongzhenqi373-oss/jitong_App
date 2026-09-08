// FrameCodec 单元测试：覆盖 P3-T02 验收矩阵的全部边界。
//
// 编译后由 CTest 运行：ctest -R frame_codec

#include "transport/FrameCodec.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++g_failures;
        std::cerr << "  [FAIL] " << what << std::endl;
    } else {
        std::cout << "  [ok]   " << what << std::endl;
    }
}

using im::proto::protType;
using im::transport::FrameCodec;
using im::transport::FrameReader;

constexpr protType kLoginRq = im::proto::DEF_PROT_LOGIN_RQ;      // 1002
constexpr protType kHeartbeatRq = im::proto::DEF_PROT_HEARTBEAT_RQ; // 1010

std::string makeFrame(protType type, const std::string& payload)
{
    return FrameCodec::encodeFrame(type, payload);
}

// ---------------- 1. 长度字段端序 ----------------

void testLengthEndianness()
{
    std::cout << "[1] 长度字段大端" << std::endl;
    char buf[4] = {};
    FrameCodec::encodeLength(0x01020304u, buf);
    check(static_cast<unsigned char>(buf[0]) == 0x01, "长度最高字节在前");
    check(static_cast<unsigned char>(buf[3]) == 0x04, "长度最低字节在后");
    check(FrameCodec::decodeLength(buf) == 0x01020304u, "编解码往返一致");

    // 常见值往返
    for (std::uint32_t v : {0u, 1u, 4u, 255u, 65535u, 10u * 1024u * 1024u, 0xDEADBEEFu}) {
        char b[4] = {};
        FrameCodec::encodeLength(v, b);
        check(FrameCodec::decodeLength(b) == v, "往返 " + std::to_string(v));
    }
}

// ---------------- 2. 组帧与解析 ----------------

void testEncodeParse()
{
    std::cout << "[2] 组帧与解析" << std::endl;
    const std::string payload = "hello-payload";
    const std::string frame = makeFrame(kLoginRq, payload);

    check(frame.size() == 4 + 4 + payload.size(), "帧长 = 4 + 4 + payload");
    check(FrameCodec::decodeLength(frame.data()) == 4 + payload.size(), "body_len 含协议号不含自身");

    FrameCodec::ParsedFrame parsed;
    const bool ok = FrameCodec::parseBody(frame.data() + 4, frame.size() - 4, parsed);
    check(ok, "parseBody 成功");
    check(parsed.type == kLoginRq, "协议号正确（小端）");
    check(parsed.payloadLen == payload.size(), "payload 长度正确");
    check(std::memcmp(parsed.payload, payload.data(), payload.size()) == 0, "payload 内容一致");

    // 空 payload
    const std::string empty = makeFrame(kHeartbeatRq, "");
    check(empty.size() == 8, "空 payload 帧长 8");
    FrameCodec::ParsedFrame p2;
    check(FrameCodec::parseBody(empty.data() + 4, 4, p2) && p2.payloadLen == 0, "空 payload 可解析");
}

// ---------------- 3. 包长边界 ----------------

void testBodyLenBounds()
{
    std::cout << "[3] body_len 边界" << std::endl;
    // 规范：4 <= body_len <= 10MiB
    for (std::uint32_t bad : {0u, 1u, 2u, 3u}) {
        check(!FrameCodec::isValidBodyLen(bad), "拒绝 body_len=" + std::to_string(bad));
    }
    check(FrameCodec::isValidBodyLen(4), "接受 body_len=4（仅协议号）");
    check(FrameCodec::isValidBodyLen(10u * 1024u * 1024u), "接受 body_len=10MiB");
    check(!FrameCodec::isValidBodyLen(10u * 1024u * 1024u + 1u), "拒绝 body_len>10MiB");
    check(!FrameCodec::isValidBodyLen(0xFFFFFFFFu), "拒绝 body_len=0xFFFFFFFF");

    // 安全减法：非法长度不得产生巨大 payloadSize
    check(FrameCodec::payloadSize(0) == 0, "payloadSize(0) == 0（不回绕）");
    check(FrameCodec::payloadSize(4) == 0, "payloadSize(4) == 0");
    check(FrameCodec::payloadSize(8) == 4, "payloadSize(8) == 4");
}

// ---------------- 4. 包长误写为小端 ----------------

void testLittleEndianLengthIsRejected()
{
    std::cout << "[4] 包长误写为小端" << std::endl;
    // 真实长度 8 被误编码为小端：00 00 00 08 -> 大端解读为 0x00000008 = 134217728 > 10MiB
    std::string frame = makeFrame(kLoginRq, "abc"); // 正确帧 len=7
    // 手工覆盖为小端
    frame[0] = static_cast<char>(0x07);
    frame[1] = 0;
    frame[2] = 0;
    frame[3] = 0;

    FrameReader reader;
    std::vector<FrameReader::Frame> out;
    const auto st = reader.feed(frame.data(), frame.size(), out);
    check(st == FrameReader::Status::InvalidLength, "小端包长被判非法（按超长处理）");
    check(out.empty(), "非法时不产出帧");
}

// ---------------- 5. 半包：Header 每次 1 字节 ----------------

void testHeaderByteByByte()
{
    std::cout << "[5] Header 每次 1 字节" << std::endl;
    const std::string frame = makeFrame(kLoginRq, std::string(32, 'x'));

    FrameReader reader;
    std::vector<FrameReader::Frame> out;
    std::size_t readyAt = 0;
    for (std::size_t i = 0; i < frame.size(); ++i) {
        const auto st = reader.feed(frame.data() + i, 1, out);
        if (st == FrameReader::Status::InvalidLength) {
            check(false, "分片过程不应报非法");
            return;
        }
        if (!out.empty() && readyAt == 0) readyAt = i + 1;
    }
    check(out.size() == 1, "最终解出 1 帧");
    check(readyAt == frame.size(), "恰好在收满整帧后才产出");
    if (!out.empty()) {
        check(out[0].type == kLoginRq, "类型正确");
        check(out[0].payload.size() == 32, "payload 长度正确");
    }
}

// ---------------- 6. 半包：Body 每次 1~7 字节 ----------------

void testBodyChunked()
{
    std::cout << "[6] Body 分片到达" << std::endl;
    const std::string payload(100, 'y');
    const std::string frame = makeFrame(kHeartbeatRq, payload);

    for (std::size_t chunk = 1; chunk <= 7; ++chunk) {
        FrameReader reader;
        std::vector<FrameReader::Frame> out;
        bool invalid = false;
        for (std::size_t i = 0; i < frame.size(); i += chunk) {
            const std::size_t n = std::min(chunk, frame.size() - i);
            if (reader.feed(frame.data() + i, n, out) == FrameReader::Status::InvalidLength) {
                invalid = true;
                break;
            }
        }
        check(!invalid, "分片大小 " + std::to_string(chunk) + " 不报非法");
        check(out.size() == 1, "分片大小 " + std::to_string(chunk) + " 只回调一次");
        if (out.size() == 1) {
            check(out[0].payload.size() == payload.size(),
                  "分片大小 " + std::to_string(chunk) + " payload 完整");
        }
    }
}

// ---------------- 7. 粘包 ----------------

void testStickyFrames()
{
    std::cout << "[7] 粘包（两帧一次到达）" << std::endl;
    const std::string f1 = makeFrame(kLoginRq, "first");
    const std::string f2 = makeFrame(kHeartbeatRq, "second-payload");
    const std::string both = f1 + f2;

    FrameReader reader;
    std::vector<FrameReader::Frame> out;
    const auto st = reader.feed(both.data(), both.size(), out);
    check(st == FrameReader::Status::FrameReady, "一次喂入产出帧");
    check(out.size() == 2, "解出 2 帧");
    if (out.size() == 2) {
        check(out[0].type == kLoginRq, "第 1 帧类型正确");
        check(std::string(out[0].payload.begin(), out[0].payload.end()) == "first", "第 1 帧内容正确");
        check(out[1].type == kHeartbeatRq, "第 2 帧类型正确");
        check(std::string(out[1].payload.begin(), out[1].payload.end()) == "second-payload",
              "第 2 帧内容正确（不串包）");
    }
    check(reader.pendingBytes() == 0, "无残留字节");
}

// ---------------- 8. Header 与半个 Body 同时到达 ----------------

void testHeaderPlusPartialBody()
{
    std::cout << "[8] Header + 半个 Body" << std::endl;
    const std::string frame = makeFrame(kLoginRq, std::string(50, 'z'));

    FrameReader reader;
    std::vector<FrameReader::Frame> out;

    // 先喂 Header + 半个 Body
    const std::size_t first = 4 + 10;
    check(reader.feed(frame.data(), first, out) == FrameReader::Status::NeedMore, "半包时返回 NeedMore");
    check(out.empty(), "半包不产出帧");
    check(reader.pendingBytes() == first, "半包字节被保存");

    // 补齐剩余
    const auto st = reader.feed(frame.data() + first, frame.size() - first, out);
    check(st == FrameReader::Status::FrameReady, "补齐后产出帧");
    check(out.size() == 1 && out[0].payload.size() == 50, "补齐后 payload 完整");
}

// ---------------- 9. 非法长度后不再解析 ----------------

void testInvalidLengthStopsParsing()
{
    std::cout << "[9] 非法长度后停止解析" << std::endl;
    const std::string good = makeFrame(kLoginRq, "ok");
    std::string bad = makeFrame(kLoginRq, "xx");
    bad[0] = static_cast<char>(0xFF); // 超大长度
    bad[1] = static_cast<char>(0xFF);
    bad[2] = static_cast<char>(0xFF);
    bad[3] = static_cast<char>(0xFF);

    FrameReader reader;
    std::vector<FrameReader::Frame> out;
    check(reader.feed(bad.data(), bad.size(), out) == FrameReader::Status::InvalidLength, "非法包长被拒");
    check(reader.pendingBytes() == 0, "非法后缓冲被清空（不在流里扫描下一个包头）");

    // 之后即使喂入合法帧也不应被当作“恢复解析”的依据
    out.clear();
    reader.feed(good.data(), good.size(), out);
    check(out.size() == 1, "新数据仍可正常解析（调用方应重建连接，此处只验证不崩）");
}

// ---------------- 10. 未知 / 越界协议号 ----------------

void testUnknownProtocolType()
{
    std::cout << "[10] 未知与越界协议号" << std::endl;
    // 未知但合法的协议号：帧可解析，分发由上层做范围校验
    for (protType t : {0u, 999u, 5000u, 0xFFFFFFFFu}) {
        const std::string frame = makeFrame(t, "p");
        FrameReader reader;
        std::vector<FrameReader::Frame> out;
        const auto st = reader.feed(frame.data(), frame.size(), out);
        check(st == FrameReader::Status::FrameReady, "协议号 " + std::to_string(t) + " 可解析");
        if (out.size() == 1) {
            check(out[0].type == t, "协议号 " + std::to_string(t) + " 原样透传（不做越界访问）");
        }
    }
}

// ---------------- 11. 截断 protobuf ----------------

void testTruncatedPayload()
{
    std::cout << "[11] 截断 payload" << std::endl;
    // 组一个声称 100 字节 payload 但只给 10 字节的帧：FrameReader 应等待而非崩溃
    std::string frame;
    frame.resize(8 + 10);
    FrameCodec::encodeLength(4 + 100, frame.data());
    im::proto::encodeType32(kLoginRq, frame.data() + 4);
    std::memset(frame.data() + 8, 'a', 10);

    FrameReader reader;
    std::vector<FrameReader::Frame> out;
    check(reader.feed(frame.data(), frame.size(), out) == FrameReader::Status::NeedMore,
          "截断帧等待剩余数据，不崩溃");
    check(out.empty(), "截断帧不产出");
}

// ---------------- 12. 超大帧上限 ----------------

void testMaxFrameAccepted()
{
    std::cout << "[12] 10MiB 边界" << std::endl;
    const std::size_t maxPayload = FrameCodec::kMaxBodyLen - 4;
    // 不真的分配 10MB 做全量拷贝验证边界，只验证编解码与校验逻辑
    check(FrameCodec::isValidBodyLen(static_cast<std::uint32_t>(maxPayload) + 4), "恰好 10MiB 合法");

    const std::string frame = makeFrame(kLoginRq, std::string(1024, 'b'));
    FrameReader reader;
    std::vector<FrameReader::Frame> out;
    check(reader.feed(frame.data(), frame.size(), out) == FrameReader::Status::FrameReady, "常规大小帧正常");
}

} // namespace

int main()
{
    std::cout << "=== test_frame_codec ===" << std::endl;
    testLengthEndianness();
    testEncodeParse();
    testBodyLenBounds();
    testLittleEndianLengthIsRejected();
    testHeaderByteByByte();
    testBodyChunked();
    testStickyFrames();
    testHeaderPlusPartialBody();
    testInvalidLengthStopsParsing();
    testUnknownProtocolType();
    testTruncatedPayload();
    testMaxFrameAccepted();

    if (g_failures == 0) {
        std::cout << "test_frame_codec PASSED" << std::endl;
        return 0;
    }
    std::cerr << "test_frame_codec FAILED: " << g_failures << " 项" << std::endl;
    return 1;
}
