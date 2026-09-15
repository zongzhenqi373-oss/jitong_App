// 确定性随机源（P7-G1 Harness）。
//
// 目的：msg_id、requestId、nonce 等随机值必须可复现，否则 Golden 用例无法稳定 diff。
// 测试注入 SeededRandom，同一 seed 必产生同一序列。

#ifndef CLIENT_CORE_TESTING_SEEDED_RANDOM_H
#define CLIENT_CORE_TESTING_SEEDED_RANDOM_H

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>

namespace im {
namespace testing {

class IRandom {
public:
    virtual ~IRandom() = default;
    virtual std::uint64_t nextU64() = 0;
    virtual void nextBytes(unsigned char* out, std::size_t n) = 0;
};

class SeededRandom : public IRandom {
public:
    explicit SeededRandom(std::uint64_t seed = 0x9E3779B97F4A7C15ULL) : m_rng(seed) {}

    std::uint64_t nextU64() override { return m_rng(); }

    std::uint32_t nextU32() { return static_cast<std::uint32_t>(m_rng() & 0xFFFFFFFFULL); }

    void nextBytes(unsigned char* out, std::size_t n) override
    {
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = static_cast<unsigned char>((m_rng() >> ((i % 8) * 8)) & 0xFF);
        }
    }

    /** 生成确定性 hex 字符串（常用于构造 msg_id / requestId）。 */
    std::string nextHex(std::size_t bytes)
    {
        static const char* kHex = "0123456789abcdef";
        std::string out;
        out.reserve(bytes * 2);
        for (std::size_t i = 0; i < bytes; ++i) {
            const std::uint64_t v = m_rng();
            out.push_back(kHex[(v >> ((i % 8) * 8)) & 0xF]);
            out.push_back(kHex[((v >> ((i % 8) * 8)) >> 4) & 0xF]);
        }
        return out;
    }

    void reseed(std::uint64_t seed) { m_rng.seed(seed); }

private:
    std::mt19937_64 m_rng;
};

} // namespace testing
} // namespace im

#endif // CLIENT_CORE_TESTING_SEEDED_RANDOM_H
