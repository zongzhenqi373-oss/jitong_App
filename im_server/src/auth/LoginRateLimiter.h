#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace imsrv {

/** 密码登录失败限流；Token登录/刷新不走这里。线程安全。 */
class LoginRateLimiter {
public:
    using Clock = std::chrono::steady_clock;

    struct Policy {
        std::size_t pairFailureLimit = 5;
        std::size_t ipFailureLimit = 30;
        std::chrono::seconds window{300};
        std::chrono::seconds pairBlock{60};
        std::chrono::seconds ipBlock{300};
    };

    LoginRateLimiter();
    explicit LoginRateLimiter(Policy policy);

    bool allow(const std::string& ip, const std::string& account,
               Clock::time_point now = Clock::now());
    void recordFailure(const std::string& ip, const std::string& account,
                       Clock::time_point now = Clock::now());
    void recordSuccess(const std::string& ip, const std::string& account);

private:
    struct Bucket {
        std::deque<Clock::time_point> failures;
        Clock::time_point blockedUntil{};
        Clock::time_point lastSeen{};
    };

    static std::string pairKey(const std::string& ip, const std::string& account);
    void prune(Bucket& bucket, Clock::time_point now) const;
    bool blocked(const Bucket& bucket, Clock::time_point now) const;
    void cleanupMaps(Clock::time_point now);

    Policy policy_;
    std::mutex mutex_;
    std::unordered_map<std::string, Bucket> ipBuckets_;
    std::unordered_map<std::string, Bucket> pairBuckets_;
    std::size_t operations_ = 0;
};

} // namespace imsrv
