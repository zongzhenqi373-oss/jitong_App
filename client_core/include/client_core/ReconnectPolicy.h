// 自动重连退避策略（P5-T05）。
//
// 纯逻辑，无 IO、无线程：只计算"第 N 次重连应等待多久"，实际的定时/发起由上层执行。
//
// 规则（对齐 Kotlin 现状基线并收敛到内核）：
//   - 指数退避：base(默认 1s) 逐次翻倍：1s, 2s, 4s, 8s, ...，封顶 maxDelay(默认 30s)；
//   - jitter：在基准值上叠加 [0, jitterRatio*base] 的随机抖动，避免大量客户端同时重连造成
//     惊群。随机源通过 IJitter 注入，测试里用固定 0 抖动以获得确定性；
//   - 连接成功后 reset()，下次从 base 重新开始；
//   - 被踢/登出后 disable()，nextDelayMs 返回 -1 表示"不应重连"。

#ifndef CLIENT_CORE_RECONNECT_POLICY_H
#define CLIENT_CORE_RECONNECT_POLICY_H

#include <cstdint>
#include <memory>

namespace im {
namespace account {

/** 抖动源：返回 [0,1) 的随机比例。测试用固定实现。 */
class IJitter {
public:
    virtual ~IJitter() = default;
    virtual double next01() const = 0;
};

class ReconnectPolicy {
public:
    ReconnectPolicy(std::int64_t baseMs = 1000, std::int64_t maxMs = 30000,
                    double jitterRatio = 0.25, std::shared_ptr<IJitter> jitter = nullptr)
        : m_baseMs(baseMs), m_maxMs(maxMs), m_jitterRatio(jitterRatio), m_jitter(std::move(jitter))
    {
    }

    /**
     * 计算下一次重连的等待毫秒并推进退避计数。
     * 返回 -1 表示已禁用（不应重连）。
     */
    std::int64_t nextDelayMs();

    /** 当前退避指数（从 0 开始）。仅供观测/测试。 */
    std::uint32_t attempt() const { return m_attempt; }

    bool enabled() const { return m_enabled; }

    /** 连接成功：清零退避。 */
    void reset()
    {
        m_attempt = 0;
        m_enabled = true;
    }

    /** 被踢/登出：禁止重连。 */
    void disable() { m_enabled = false; }

    /** 重新允许重连（例如用户手动重试）。 */
    void enable() { m_enabled = true; }

private:
    std::int64_t m_baseMs;
    std::int64_t m_maxMs;
    double m_jitterRatio;
    std::shared_ptr<IJitter> m_jitter;
    std::uint32_t m_attempt = 0;
    bool m_enabled = true;
};

} // namespace account
} // namespace im

#endif // CLIENT_CORE_RECONNECT_POLICY_H
