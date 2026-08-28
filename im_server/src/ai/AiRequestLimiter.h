#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace imsrv::ai {

class AiRequestLimiter {
public:
    enum class Result { Accepted, Busy, RateLimited, Duplicate, QueueFull };
    using Clock = std::chrono::steady_clock;

    AiRequestLimiter(int perMinute, int maxPending, int replayWindowSeconds);
    Result tryAcquire(int userId, const std::string& requestId,
                      Clock::time_point now = Clock::now());
    void release(int userId);

private:
    int perMinute_;
    int maxPending_;
    std::chrono::seconds replayWindow_;
    std::mutex mutex_;
    std::unordered_set<int> inFlightUsers_;
    std::unordered_map<int, std::deque<Clock::time_point>> recentRequests_;
    std::unordered_map<int, std::unordered_map<std::string, Clock::time_point>> seenIds_;
};

} // namespace imsrv::ai
