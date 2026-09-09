#include "client_core/ReconnectPolicy.h"

namespace im {
namespace account {

std::int64_t ReconnectPolicy::nextDelayMs()
{
    if (!m_enabled) return -1;

    // 基准 = base * 2^attempt，封顶 maxMs
    std::int64_t delay = m_baseMs;
    for (std::uint32_t i = 0; i < m_attempt; ++i) {
        delay <<= 1;
        if (delay >= m_maxMs) {
            delay = m_maxMs;
            break;
        }
    }
    if (delay > m_maxMs) delay = m_maxMs;

    // jitter：叠加 [0, jitterRatio * base] 的抖动（仅在未封顶时有意义，封顶后也允许小抖动）
    if (m_jitter && m_jitterRatio > 0.0) {
        const double frac = m_jitter->next01(); // [0,1)
        const std::int64_t jitterMax = static_cast<std::int64_t>(m_baseMs * m_jitterRatio);
        delay += static_cast<std::int64_t>(frac * jitterMax);
        if (delay > m_maxMs) delay = m_maxMs;
    }

    ++m_attempt;
    return delay;
}

} // namespace account
} // namespace im
