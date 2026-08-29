#include "auth/LoginRateLimiter.h"

#include <algorithm>

namespace imsrv {

LoginRateLimiter::LoginRateLimiter() : LoginRateLimiter(Policy{}) {}

LoginRateLimiter::LoginRateLimiter(Policy policy) : policy_(policy)
{
    policy_.pairFailureLimit = std::max<std::size_t>(policy_.pairFailureLimit, 1);
    policy_.ipFailureLimit = std::max<std::size_t>(policy_.ipFailureLimit, 1);
}

std::string LoginRateLimiter::pairKey(const std::string& ip, const std::string& account)
{
    return ip + '\n' + account;
}

void LoginRateLimiter::prune(Bucket& bucket, Clock::time_point now) const
{
    const auto cutoff = now - policy_.window;
    while (!bucket.failures.empty() && bucket.failures.front() < cutoff)
        bucket.failures.pop_front();
}

bool LoginRateLimiter::blocked(const Bucket& bucket, Clock::time_point now) const
{
    return bucket.blockedUntil > now;
}

bool LoginRateLimiter::allow(const std::string& ip, const std::string& account,
                             Clock::time_point now)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto ipIt = ipBuckets_.find(ip);
    if (ipIt != ipBuckets_.end()) {
        prune(ipIt->second, now);
        if (blocked(ipIt->second, now)) return false;
    }
    if (!account.empty()) {
        auto pairIt = pairBuckets_.find(pairKey(ip, account));
        if (pairIt != pairBuckets_.end()) {
            prune(pairIt->second, now);
            if (blocked(pairIt->second, now)) return false;
        }
    }
    return true;
}

void LoginRateLimiter::recordFailure(const std::string& ip, const std::string& account,
                                     Clock::time_point now)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& ipBucket = ipBuckets_[ip];
    prune(ipBucket, now);
    ipBucket.failures.push_back(now);
    ipBucket.lastSeen = now;
    if (ipBucket.failures.size() >= policy_.ipFailureLimit)
        ipBucket.blockedUntil = std::max(ipBucket.blockedUntil, now + policy_.ipBlock);

    if (!account.empty()) {
        auto& pairBucket = pairBuckets_[pairKey(ip, account)];
        prune(pairBucket, now);
        pairBucket.failures.push_back(now);
        pairBucket.lastSeen = now;
        if (pairBucket.failures.size() >= policy_.pairFailureLimit)
            pairBucket.blockedUntil = std::max(pairBucket.blockedUntil, now + policy_.pairBlock);
    }
    if (++operations_ % 256 == 0) cleanupMaps(now);
}

void LoginRateLimiter::recordSuccess(const std::string& ip, const std::string& account)
{
    if (account.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    pairBuckets_.erase(pairKey(ip, account));
}

void LoginRateLimiter::cleanupMaps(Clock::time_point now)
{
    const auto staleBefore = now - policy_.window - policy_.ipBlock;
    auto cleanup = [staleBefore](auto& buckets) {
        for (auto it = buckets.begin(); it != buckets.end();) {
            if (it->second.lastSeen < staleBefore && it->second.blockedUntil <= staleBefore)
                it = buckets.erase(it);
            else
                ++it;
        }
    };
    cleanup(ipBuckets_);
    cleanup(pairBuckets_);
}

} // namespace imsrv
