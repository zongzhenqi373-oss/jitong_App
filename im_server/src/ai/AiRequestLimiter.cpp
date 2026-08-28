#include "ai/AiRequestLimiter.h"

#include <algorithm>

namespace imsrv::ai {

AiRequestLimiter::AiRequestLimiter(int perMinute, int maxPending, int replayWindowSeconds)
    : perMinute_(std::max(perMinute, 1)),
      maxPending_(std::max(maxPending, 1)),
      replayWindow_(std::max(replayWindowSeconds, 1)) {}

AiRequestLimiter::Result AiRequestLimiter::tryAcquire(
    int userId, const std::string& requestId, Clock::time_point now) {
    const auto rateCutoff = now - std::chrono::minutes(1);
    const auto replayCutoff = now - replayWindow_;
    std::lock_guard<std::mutex> lock(mutex_);

    auto& seen = seenIds_[userId];
    for (auto it = seen.begin(); it != seen.end();) {
        if (it->second < replayCutoff) it = seen.erase(it);
        else ++it;
    }
    if (seen.find(requestId) != seen.end()) return Result::Duplicate;
    if (inFlightUsers_.find(userId) != inFlightUsers_.end()) return Result::Busy;
    if (static_cast<int>(inFlightUsers_.size()) >= maxPending_) return Result::QueueFull;

    auto& recent = recentRequests_[userId];
    while (!recent.empty() && recent.front() < rateCutoff) recent.pop_front();
    if (static_cast<int>(recent.size()) >= perMinute_) return Result::RateLimited;

    recent.push_back(now);
    seen.emplace(requestId, now);
    inFlightUsers_.insert(userId);
    return Result::Accepted;
}

void AiRequestLimiter::release(int userId) {
    std::lock_guard<std::mutex> lock(mutex_);
    inFlightUsers_.erase(userId);
}

} // namespace imsrv::ai
