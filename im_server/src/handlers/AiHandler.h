#pragma once

#include "ai/AiConfig.h"
#include "ai/AiRequestLimiter.h"

#include <asio/thread_pool.hpp>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

namespace imsrv {
class Database;
class Session;

class AiHandler {
public:
    explicit AiHandler(Database& db);
    ~AiHandler();

    AiHandler(const AiHandler&) = delete;
    AiHandler& operator=(const AiHandler&) = delete;

    void onReply(const std::shared_ptr<Session>& session, const std::string& payload);
    void onCancel(const std::shared_ptr<Session>& session, const std::string& payload);

private:
    static std::string requestKey(int userId, const std::string& requestId);
    bool markCancelled(int userId, const std::string& requestId);
    bool isCancelled(int userId, const std::string& requestId);
    void finishRequest(int userId);
    void finishRequest(int userId, const std::string& requestId);
    void logMetrics() const;

    Database& m_db;
    ai::AiConfig m_config;
    ai::AiRequestLimiter m_limiter;
    asio::thread_pool m_workers{2};
    std::mutex m_cancelMutex;
    std::unordered_set<std::string> m_activeRequests;
    std::unordered_set<std::string> m_cancelledRequests;

    std::atomic<std::uint64_t> m_accepted{0};
    std::atomic<std::uint64_t> m_succeeded{0};
    std::atomic<std::uint64_t> m_failed{0};
    std::atomic<std::uint64_t> m_rejected{0};
    std::atomic<std::uint64_t> m_cancelled{0};
    std::atomic<std::uint64_t> m_totalLatencyMs{0};
};

} // namespace imsrv
