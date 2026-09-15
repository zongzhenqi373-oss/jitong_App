// 确定性时钟（P7-G1 Harness）。
//
// 目的：测试**不依赖真实 sleep**，所有时间推进由测试显式控制，结果可重放。
// 生产代码依赖 IClock 接口，测试注入 DeterministicClock。

#ifndef CLIENT_CORE_TESTING_DETERMINISTIC_CLOCK_H
#define CLIENT_CORE_TESTING_DETERMINISTIC_CLOCK_H

#include <cstdint>

namespace im {
namespace testing {

class IClock {
public:
    virtual ~IClock() = default;
    /** 单调递增的毫秒时间戳（测试中为虚拟时间）。 */
    virtual std::int64_t nowMs() const = 0;
};

class DeterministicClock : public IClock {
public:
    explicit DeterministicClock(std::int64_t startMs = 1700000000000LL) : m_now(startMs) {}

    std::int64_t nowMs() const override { return m_now; }

    /** 推进虚拟时间；deltaMs <= 0 时不前进（时间单调，不回退）。 */
    void advance(std::int64_t deltaMs)
    {
        if (deltaMs > 0) m_now += deltaMs;
    }

    /** 直接设置虚拟时间（仅用于构造特定场景）。 */
    void set(std::int64_t ms) { m_now = ms; }

private:
    std::int64_t m_now;
};

} // namespace testing
} // namespace im

#endif // CLIENT_CORE_TESTING_DETERMINISTIC_CLOCK_H
