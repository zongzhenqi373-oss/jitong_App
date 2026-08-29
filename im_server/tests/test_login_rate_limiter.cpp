#include "auth/LoginRateLimiter.h"

#include <cassert>
#include <chrono>
#include <iostream>

int main()
{
    using Limiter = imsrv::LoginRateLimiter;
    Limiter::Policy policy;
    policy.pairFailureLimit = 3;
    policy.ipFailureLimit = 5;
    policy.window = std::chrono::seconds(60);
    policy.pairBlock = std::chrono::seconds(10);
    policy.ipBlock = std::chrono::seconds(20);
    Limiter limiter(policy);
    const auto start = Limiter::Clock::time_point{} + std::chrono::hours(1);

    assert(limiter.allow("127.0.0.1", "13800138000", start));
    limiter.recordFailure("127.0.0.1", "13800138000", start);
    limiter.recordFailure("127.0.0.1", "13800138000", start);
    assert(limiter.allow("127.0.0.1", "13800138000", start));
    limiter.recordFailure("127.0.0.1", "13800138000", start);
    assert(!limiter.allow("127.0.0.1", "13800138000", start));
    // 组合限流不应封禁同一IP下的另一个账号。
    assert(limiter.allow("127.0.0.1", "13900139000", start));
    assert(limiter.allow("127.0.0.1", "13800138000", start + std::chrono::seconds(11)));

    limiter.recordFailure("10.0.0.1", "a", start);
    limiter.recordFailure("10.0.0.1", "b", start);
    limiter.recordFailure("10.0.0.1", "c", start);
    limiter.recordFailure("10.0.0.1", "d", start);
    limiter.recordFailure("10.0.0.1", "e", start);
    assert(!limiter.allow("10.0.0.1", "new-account", start));
    assert(limiter.allow("10.0.0.1", "new-account", start + std::chrono::seconds(21)));

    limiter.recordFailure("192.0.2.1", "13800138000", start);
    limiter.recordSuccess("192.0.2.1", "13800138000");
    assert(limiter.allow("192.0.2.1", "13800138000", start));

    std::cout << "login rate limiter tests passed\n";
    return 0;
}
