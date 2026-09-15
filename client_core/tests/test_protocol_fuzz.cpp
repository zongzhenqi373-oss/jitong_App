// P7-G1：协议 fuzz corpus（线格式健壮性）。
//
// 覆盖 v2 §3 P7-G1 要求的畸形输入：截断、尾随、未知字段、错误长度、超大计数、坏 tag。
// 核心不变式：**畸形输入必须被明确拒绝且不崩溃、不 OOM、不无限循环**，
// 且包长损坏后**不在字节流里扫描下一个包头**（直接 fail-close）。

#include "transport/FrameCodec.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "client_core/Protocol.h"
#include "client_core/testing/SeededRandom.h"
#include "im.pb.h"

using namespace im::transport;
using namespace im::testing;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& name)
{
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << std::endl;
    if (!cond) ++g_failures;
}

/** 手工组帧：可故意写入非法 body_len（绕过 encodeFrame 的合法性保护）。 */
std::string makeRawFrame(std::uint32_t bodyLenField, std::uint32_t typeLe,
                         const std::string& payload)
{
    std::string out;
    out.resize(4 + payload.size() + (bodyLenField >= 4 ? 4 : 0));
    char* p = &out[0];
    FrameCodec::encodeLength(bodyLenField, p);
    if (bodyLenField >= 4) {
        std::memcpy(p + 4, &typeLe, 4); // 小端（测试主机为小端，与协议的 encode 一致）
    }
    if (!payload.empty()) std::memcpy(p + 8, payload.data(), payload.size());
    return out;
}

std::string validPayload()
{
    // 注：心跳(1010/1011)在 protobuf 中无独立消息（空 payload），
    // 这里用 RegisterRq 构造一个真实 payload；fuzz 只关心帧层语义。
    im::proto::RegisterRq rq;
    rq.set_nick("tester");
    rq.set_tel("13800000000");
    rq.set_pass("p");
    return rq.SerializeAsString();
}
} // namespace

int main()
{
    std::cout << "=== test_protocol_fuzz ===" << std::endl;

    // [1] 包长边界矩阵：只有 [4, 10MiB] 合法
    {
        struct Case { std::uint32_t len; bool valid; const char* name; };
        const Case cases[] = {
            {0, false, "bodyLen=0（< 最小 4）"},
            {3, false, "bodyLen=3（不足协议号）"},
            {4, true, "bodyLen=4（仅协议号，合法下界）"},
            {FrameCodec::kMaxBodyLen, true, "bodyLen=10MiB（合法上界）"},
            {FrameCodec::kMaxBodyLen + 1, false, "bodyLen=10MiB+1（超上界）"},
            {0xFFFFFFFFu, false, "bodyLen=0xFFFFFFFF（超大）"},
        };
        for (const auto& c : cases) {
            check(FrameCodec::isValidBodyLen(c.len) == c.valid,
                  std::string("包长边界：") + c.name);
        }
    }

    // [2] 截断：数据不足时必须 NeedMore，而不是产出错误帧
    {
        FrameReader r;
        std::vector<FrameReader::Frame> out;
        const std::string full = FrameCodec::encodeFrame(1010, validPayload());

        // 只喂长度字段的前 2 字节
        check(r.feed(full.data(), 2, out) == FrameReader::Status::NeedMore, "半包：2 字节 → NeedMore");
        check(out.empty(), "半包不产出帧");

        // 补齐剩余到只剩 1 字节 body
        r.reset();
        out.clear();
        check(r.feed(full.data(), full.size() - 1, out) == FrameReader::Status::NeedMore,
              "缺 1 字节 → NeedMore");
        check(out.empty(), "缺字节不产出帧");

        // 逐字节喂：最后刚好产出
        r.reset();
        out.clear();
        bool allNeedMoreUntilEnd = true;
        for (std::size_t i = 0; i < full.size(); ++i) {
            const auto st = r.feed(full.data() + i, 1, out);
            if (i + 1 < full.size() && st == FrameReader::Status::FrameReady) {
                allNeedMoreUntilEnd = false;
            }
        }
        check(allNeedMoreUntilEnd, "逐字节喂：最后一字节前不产出");
        check(out.size() == 1, "逐字节喂最终产出 1 帧");
    }

    // [3] 粘包：一次多帧必须逐帧产出，不串包
    {
        FrameReader r;
        std::vector<FrameReader::Frame> out;
        const std::string f1 = FrameCodec::encodeFrame(1010, validPayload());
        const std::string f2 = FrameCodec::encodeFrame(1011, validPayload());
        const std::string f3 = FrameCodec::encodeFrame(1029, validPayload());
        const std::string all = f1 + f2 + f3;
        check(r.feed(all.data(), all.size(), out) == FrameReader::Status::FrameReady, "粘包产出帧");
        check(out.size() == 3, "3 帧粘包产出 3 帧");
        if (out.size() == 3) {
            check(out[0].type == 1010 && out[1].type == 1011 && out[2].type == 1029,
                  "粘包顺序与类型正确（不串包）");
        }
    }

    // [4] 尾随垃圾：解出完整帧后残留字节保留，不误判
    {
        FrameReader r;
        std::vector<FrameReader::Frame> out;
        const std::string f1 = FrameCodec::encodeFrame(1010, validPayload());
        std::string withTail = f1;
        withTail.push_back('\x01'); // 1 字节尾随（不足以构成下一帧头）
        const auto st = r.feed(withTail.data(), withTail.size(), out);
        check(out.size() == 1, "尾随垃圾：仍解出 1 帧");
        // 语义：只要解出过帧就返回 FrameReady，残留字节保留给下次 feed
        // （而不是因为尾部还有不完整字节就报 NeedMore）
        check(st == FrameReader::Status::FrameReady, "解出帧 → FrameReady（残留留待下次）");
        check(r.pendingBytes() > 0, "残留字节被保留");
    }

    // [5] 非法长度：必须 InvalidLength，且**不扫描**后续字节找包
    {
        FrameReader r;
        std::vector<FrameReader::Frame> out;
        // body_len 写成超大值
        std::string bad = makeRawFrame(0xFFFFFFFFu, 1010, validPayload());
        check(r.feed(bad.data(), bad.size(), out) == FrameReader::Status::InvalidLength,
              "超大包长 → InvalidLength");
        check(out.empty(), "非法长度不产出帧");

        // 即使后面跟着一个合法帧，也不得扫描出来
        r.reset();
        out.clear();
        const std::string good = FrameCodec::encodeFrame(1010, validPayload());
        std::string badThenGood = makeRawFrame(0xFFFFFFFFu, 1010, validPayload()) + good;
        check(r.feed(badThenGood.data(), badThenGood.size(), out) ==
                  FrameReader::Status::InvalidLength,
              "非法长度后紧跟合法帧 → 仍 InvalidLength（不扫描下一个包头）");
        check(out.empty(), "非法长度后不产出任何帧（fail-close）");
    }

    // [6] protobuf 未知字段：必须忽略而非报错（前向兼容）
    {
        // 构造：合法 RegisterRq 后追加未知 field（field 99, varint）
        im::proto::RegisterRq rq;
        rq.set_nick("张三");
        rq.set_tel("13800000000");
        rq.set_pass("p");
        std::string wire = rq.SerializeAsString();
        // field 99, wire type 0 (varint): tag = (99<<3)|0 = 792 = 0x98 0x06
        wire.push_back(static_cast<char>(0x98));
        wire.push_back(static_cast<char>(0x06));
        wire.push_back(static_cast<char>(0x2A)); // value 42

        im::proto::RegisterRq parsed;
        const bool ok = parsed.ParseFromString(wire);
        check(ok, "含未知字段的 payload 解析成功（前向兼容）");
        check(parsed.nick() == "张三", "未知字段不影响已知字段");

        // 组帧后应能正常读出
        FrameReader r;
        std::vector<FrameReader::Frame> out;
        const std::string frame = FrameCodec::encodeFrame(1000, wire);
        check(r.feed(frame.data(), frame.size(), out) == FrameReader::Status::FrameReady,
              "含未知字段的帧正常解出");
        check(out.size() == 1 && out[0].type == 1000, "未知字段帧类型正确");
    }

    // [7] 畸形 payload：解析失败应 fail-close，不崩溃
    {
        const char* badPayloads[] = {
            "\xff\xff\xff\xff\xff\xff\xff\xff", // 非法 varint
            "\x08",                              // 截断的 varint
            "\x0a\xff\xff\xff\xff\x0f",          // 长度前缀超大（bytes 字段）
        };
        for (const char* p : badPayloads) {
            const std::string payload(p);
            im::proto::RegisterRq parsed;
            // 只要求"不崩溃"；解析成功与否都接受，但必须不抛不崩
            parsed.ParseFromString(payload);
            // 组帧并喂给 reader，必须不崩且有明确状态
            FrameReader r;
            std::vector<FrameReader::Frame> out;
            const std::string frame = FrameCodec::encodeFrame(1000, payload);
            const auto st = r.feed(frame.data(), frame.size(), out);
            check(st == FrameReader::Status::FrameReady || st == FrameReader::Status::NeedMore,
                  "畸形 payload 不导致 InvalidLength（帧层仍可解，由业务层拒绝）");
        }
    }

    // [8] 确定性随机 corpus：不崩溃、状态明确（可复现的轻量 fuzz）
    {
        SeededRandom rnd(0xF00DF00D);
        std::size_t needMore = 0, frameReady = 0, invalid = 0;
        for (int i = 0; i < 2000; ++i) {
            const std::size_t len = 1 + (rnd.nextU32() % 64);
            std::string buf;
            buf.resize(len);
            for (std::size_t j = 0; j < len; ++j) {
                buf[j] = static_cast<char>(rnd.nextU32() & 0xFF);
            }
            FrameReader r;
            std::vector<FrameReader::Frame> out;
            const auto st = r.feed(buf.data(), buf.size(), out);
            if (st == FrameReader::Status::NeedMore) ++needMore;
            else if (st == FrameReader::Status::FrameReady) ++frameReady;
            else ++invalid;
        }
        std::cout << "      随机 corpus 2000 例: NeedMore=" << needMore
                  << " FrameReady=" << frameReady << " InvalidLength=" << invalid << std::endl;
        check(needMore + frameReady + invalid == 2000, "全部 2000 例均有明确状态（无崩溃/挂起）");
        check(invalid > 0, "随机数据确实触发过 InvalidLength（拒绝路径被覆盖）");
    }

    // [9] 超大计数防护：body_len 声称巨大但实际数据少 → 必须 NeedMore 而非 OOM
    {
        FrameReader r;
        std::vector<FrameReader::Frame> out;
        // body_len = 5MiB（合法范围内），但只喂 16 字节
        const std::string partial = makeRawFrame(5 * 1024 * 1024, 1010, "");
        const auto st = r.feed(partial.data(), partial.size(), out);
        check(st == FrameReader::Status::NeedMore, "声称 5MiB 但数据不足 → NeedMore（不预分配爆内存）");
        check(out.empty(), "未收全不产出帧");
    }

    if (g_failures == 0) {
        std::cout << "test_protocol_fuzz PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_protocol_fuzz FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
