#include "ai/AiRequestLimiter.h"

#include <cassert>
#include <chrono>
#include <iostream>

int main() {
    using Limiter = imsrv::ai::AiRequestLimiter;
    const auto start = Limiter::Clock::time_point{} + std::chrono::hours(1);

    {
        Limiter limiter(2, 2, 300);
        assert(limiter.tryAcquire(1, "r1", start) == Limiter::Result::Accepted);
        assert(limiter.tryAcquire(1, "r2", start) == Limiter::Result::Busy);
        assert(limiter.tryAcquire(2, "r3", start) == Limiter::Result::Accepted);
        assert(limiter.tryAcquire(3, "r4", start) == Limiter::Result::QueueFull);
        limiter.release(1);
        assert(limiter.tryAcquire(1, "r1", start) == Limiter::Result::Duplicate);
        assert(limiter.tryAcquire(1, "r2", start) == Limiter::Result::Accepted);
        limiter.release(1);
        assert(limiter.tryAcquire(1, "r3", start) == Limiter::Result::RateLimited);
    }

    {
        Limiter limiter(1, 1, 300);
        assert(limiter.tryAcquire(1, "same", start) == Limiter::Result::Accepted);
        limiter.release(1);
        const auto later = start + std::chrono::seconds(301);
        // 重放窗口与分钟限流窗口都过期后，相同 request_id 可再次使用。
        assert(limiter.tryAcquire(1, "same", later) == Limiter::Result::Accepted);
    }

    std::cout << "AI request limiter tests passed\n";
    return 0;
}
