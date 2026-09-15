// 确定性图片编解码 Fake（P7-G1 Harness）。
//
// 目的：Golden 测试**不做真实像素编解码**（避免平台差异与耗时）。
// FakeCodec 对同一输入必产生同一输出字节，用于验证 Native 是否正确选择了
// 变体档位、是否传入了正确的参数（宽高/质量/方向/色彩空间），而非验证像素质量。
//
// 真实像素/色彩空间质量由 Android 端 Golden（SSIM/Delta-E）单独验证，不混入 C++ Golden。

#ifndef CLIENT_CORE_TESTING_FAKE_CODEC_H
#define CLIENT_CORE_TESTING_FAKE_CODEC_H

#include <cstdint>
#include <string>
#include <vector>

namespace im {
namespace testing {

struct EncodeRequest {
    std::int32_t srcW = 0;
    std::int32_t srcH = 0;
    std::int32_t targetW = 0;
    std::int32_t targetH = 0;
    std::int32_t quality = 0;
    std::int32_t orientation = 0; // 0/90/180/270
    std::string mime;              // image/avif 或 image/jpeg
    bool stripMetadata = true;
    std::string colorSpace;        // 规范化后的色彩空间，如 srgb
};

struct EncodeResult {
    bool ok = false;
    std::vector<unsigned char> bytes;
    std::string actualMime; // 必须反映真实编码格式，禁止 JPEG 字节标 AVIF
    std::int32_t outW = 0;
    std::int32_t outH = 0;
};

class FakeCodec {
public:
    /** 确定性编码：输出字节由请求参数决定，同参数必同输出。 */
    EncodeResult encode(const EncodeRequest& req)
    {
        // 统计"调用次数"而非"成功次数"：失败的编码尝试也应被观测到
        ++m_encodeCalls;

        EncodeResult res;
        if (req.targetW <= 0 || req.targetH <= 0) return res;
        if (req.mime != "image/avif" && req.mime != "image/jpeg") return res;

        res.ok = true;
        res.actualMime = req.mime; // 格式如实反映，不做"声称 avif 实际 jpeg"
        res.outW = req.targetW;
        res.outH = req.targetH;

        // 确定性输出：按参数拼一个可预测的短字节序列（非真实编码）
        const std::string desc = std::to_string(req.srcW) + "x" + std::to_string(req.srcH) + "->" +
                                 std::to_string(req.targetW) + "x" + std::to_string(req.targetH) +
                                 "|q" + std::to_string(req.quality) + "|o" +
                                 std::to_string(req.orientation) + "|" + req.mime + "|" +
                                 req.colorSpace + (req.stripMetadata ? "|strip" : "|keep");
        res.bytes.assign(desc.begin(), desc.end());
        return res;
    }

    /** 探测宽高（确定性：按输入前 8 字节推导，够 Golden 用）。 */
    bool probeSize(const std::vector<unsigned char>& data, std::int32_t* w, std::int32_t* h)
    {
        if (data.size() < 8) return false;
        if (w) *w = static_cast<std::int32_t>(data[0]) * 256 + data[1];
        if (h) *h = static_cast<std::int32_t>(data[2]) * 256 + data[3];
        return true;
    }

    int encodeCalls() const { return m_encodeCalls; }
    void reset() { m_encodeCalls = 0; }

private:
    int m_encodeCalls = 0;
};

} // namespace testing
} // namespace im

#endif // CLIENT_CORE_TESTING_FAKE_CODEC_H
