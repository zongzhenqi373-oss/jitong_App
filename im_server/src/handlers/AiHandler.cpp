#include "handlers/AiHandler.h"

#include "Database.h"
#include "Log.h"
#include "Session.h"
#include "ai/AnthropicModelClient.h"
#include "ai/AiContextBuilder.h"
#include "ai/IModelClient.h"
#include "ai/OpenAIModelClient.h"
#include "client_core/Protocol.h"
#include "handlers/HandlerUtils.h"
#include "im.pb.h"

#include <algorithm>
#include <asio/post.hpp>
#include <cstdint>
#include <limits>
#include <vector>

namespace imsrv {
namespace {

using im::proto::AiReplyRs;
using im::proto::AiReplyStatus;

void sendResult(const std::shared_ptr<Session>& session, const std::string& requestId,
                AiReplyStatus status, const std::string& error = {},
                const std::vector<std::string>& suggestions = {}) {
    if (!session) return;
    AiReplyRs response;
    response.set_request_id(requestId);
    response.set_status(status);
    response.set_error_message(error);
    response.set_done(true);
    for (const auto& suggestion : suggestions) response.add_suggestions(suggestion);
    session->deliver(im::proto::DEF_PROT_AI_REPLY_RS, response.SerializeAsString());
}

std::unique_ptr<ai::IModelClient> makeModelClient(const ai::AiConfig& config) {
    if (config.provider == ai::AiProvider::Anthropic) {
        return std::make_unique<ai::AnthropicModelClient>(config);
    }
    return std::make_unique<ai::OpenAIModelClient>(config);
}

AiReplyStatus mapError(ai::AiError error) {
    switch (error) {
    case ai::AiError::NotConfigured:
        return im::proto::AI_REPLY_NOT_CONFIGURED;
    case ai::AiError::RateLimited:
        return im::proto::AI_REPLY_RATE_LIMITED;
    case ai::AiError::None:
        return im::proto::AI_REPLY_OK;
    default:
        return im::proto::AI_REPLY_UPSTREAM_ERROR;
    }
}

} // namespace

AiHandler::AiHandler(Database& db)
    : m_db(db), m_config(ai::AiConfig::fromEnvironment()),
      m_limiter(m_config.rateLimitPerMinute, m_config.maxPendingRequests,
                m_config.replayWindowSeconds) {
    log("[AI] 服务状态 enabled=", m_config.enabled() ? "true" : "false",
        " provider=", m_config.provider == ai::AiProvider::Anthropic ? "anthropic" : "openai",
        " model=", m_config.model.empty() ? "<unset>" : m_config.model);
}

AiHandler::~AiHandler() {
    // 先拒绝尚未开始的任务，再等待正在执行的 HTTPS 请求退出，避免关停悬挂。
    m_workers.stop();
    m_workers.join();
}

void AiHandler::finishRequest(int userId) {
    m_limiter.release(userId);
}

std::string AiHandler::requestKey(int userId, const std::string& requestId) {
    return std::to_string(userId) + ":" + requestId;
}

bool AiHandler::markCancelled(int userId, const std::string& requestId) {
    std::lock_guard<std::mutex> lock(m_cancelMutex);
    const auto key = requestKey(userId, requestId);
    if (m_activeRequests.find(key) == m_activeRequests.end()) return false;
    m_cancelledRequests.insert(key);
    return true;
}

bool AiHandler::isCancelled(int userId, const std::string& requestId) {
    std::lock_guard<std::mutex> lock(m_cancelMutex);
    return m_cancelledRequests.find(requestKey(userId, requestId)) != m_cancelledRequests.end();
}

void AiHandler::finishRequest(int userId, const std::string& requestId) {
    {
        std::lock_guard<std::mutex> lock(m_cancelMutex);
        const auto key = requestKey(userId, requestId);
        m_activeRequests.erase(key);
        m_cancelledRequests.erase(key);
    }
    finishRequest(userId);
}

void AiHandler::logMetrics() const {
    const auto completed = m_succeeded.load() + m_failed.load();
    const auto average = completed == 0 ? 0 : m_totalLatencyMs.load() / completed;
    log("[AI_METRICS] accepted=", m_accepted.load(), " success=", m_succeeded.load(),
        " failed=", m_failed.load(), " rejected=", m_rejected.load(),
        " cancelled=", m_cancelled.load(), " avgLatencyMs=", average);
}

void AiHandler::onCancel(const std::shared_ptr<Session>& session, const std::string& payload) {
    im::proto::AiCancelRq request;
    if (!session || session->userId() <= 0 || !handlers::parsePayload(payload, request) ||
        request.request_id().empty() || request.request_id().size() > 64) return;
    if (markCancelled(session->userId(), request.request_id())) {
        ++m_cancelled;
        log("[AI] 请求取消 user=", session->userId(), " requestId=", request.request_id());
        logMetrics();
    }
}

void AiHandler::onReply(const std::shared_ptr<Session>& session, const std::string& payload) {
    im::proto::AiReplyRq request;
    if (!handlers::parsePayload(payload, request)) {
        sendResult(session, {}, im::proto::AI_REPLY_INVALID_REQUEST, "请求格式错误");
        return;
    }

    const int userId = session ? session->userId() : 0;
    if (userId <= 0) {
        ++m_rejected;
        sendResult(session, request.request_id(), im::proto::AI_REPLY_UNAUTHORIZED, "请先登录");
        return;
    }
    if (request.request_id().empty() || request.request_id().size() > 64 ||
        request.peer_id() <= 0 || request.peer_id() == userId ||
        request.tone().size() > 32 || request.max_suggestions() < 0 ||
        request.max_suggestions() > 3) {
        ++m_rejected;
        sendResult(session, request.request_id(), im::proto::AI_REPLY_INVALID_REQUEST,
                   "请求参数无效");
        return;
    }
    const auto rolloutBucket = static_cast<unsigned>(userId) * 2654435761u % 100u;
    if (!m_config.featureEnabled || rolloutBucket >= static_cast<unsigned>(m_config.rolloutPercent)) {
        ++m_rejected;
        sendResult(session, request.request_id(), im::proto::AI_REPLY_NOT_CONFIGURED,
                   "AI 功能暂未开放");
        return;
    }
    if (!m_db.isFriend(userId, request.peer_id())) {
        ++m_rejected;
        sendResult(session, request.request_id(), im::proto::AI_REPLY_NOT_FRIEND,
                   "只能分析好友会话");
        return;
    }
    if (!m_config.enabled()) {
        ++m_rejected;
        sendResult(session, request.request_id(), im::proto::AI_REPLY_NOT_CONFIGURED,
                   "AI 服务未配置");
        return;
    }

    const auto start = m_limiter.tryAcquire(userId, request.request_id());
    if (start != ai::AiRequestLimiter::Result::Accepted) {
        ++m_rejected;
        if (start == ai::AiRequestLimiter::Result::Duplicate) {
            sendResult(session, request.request_id(), im::proto::AI_REPLY_INVALID_REQUEST,
                       "重复的 AI 请求");
        } else if (start == ai::AiRequestLimiter::Result::RateLimited) {
            sendResult(session, request.request_id(), im::proto::AI_REPLY_RATE_LIMITED,
                       "请求过于频繁，请稍后再试");
        } else {
            sendResult(session, request.request_id(), im::proto::AI_REPLY_BUSY,
                       start == ai::AiRequestLimiter::Result::QueueFull ? "AI 服务繁忙，请稍后再试"
                                                       : "已有 AI 请求正在处理");
        }
        return;
    }
    ++m_accepted;
    {
        std::lock_guard<std::mutex> lock(m_cancelMutex);
        m_activeRequests.insert(requestKey(userId, request.request_id()));
    }

    const int peerId = request.peer_id();
    const std::string requestId = request.request_id();
    const std::string tone = request.tone().empty() ? "自然、简洁" : request.tone();
    const int maxSuggestions = request.max_suggestions() == 0 ? 3 : request.max_suggestions();
    std::weak_ptr<Session> weakSession = session;

    asio::post(m_workers, [this, weakSession, userId, peerId, requestId, tone, maxSuggestions] {
        struct FinishGuard {
            AiHandler* owner;
            int userId;
            std::string requestId;
            ~FinishGuard() { owner->finishRequest(userId, requestId); }
        } guard{this, userId, requestId};

        if (isCancelled(userId, requestId)) return;

        const int fetchLimit = std::min(100, m_config.maxContextMessages * 3);
        const auto rows = m_db.roamMessages(
            userId, peerId, std::numeric_limits<std::int64_t>::max(),
            fetchLimit);
        const ai::AiContextPolicy policy{
            m_config.maxContextMessages,
            m_config.maxMessageBytes,
            m_config.maxContextBytes
        };
        auto modelRequest = ai::buildReplyContext(
            rows, userId, peerId, requestId, tone, maxSuggestions, policy);

        auto currentSession = weakSession.lock();
        if (!currentSession || currentSession->userId() != userId) return;
        if (modelRequest.turns.empty()) {
            sendResult(currentSession, requestId, im::proto::AI_REPLY_INVALID_REQUEST,
                       "当前会话暂无文本消息");
            return;
        }

        auto client = makeModelClient(m_config);
        const auto result = client->generateReplySuggestions(modelRequest);
        if (isCancelled(userId, requestId)) return;
        currentSession = weakSession.lock();
        if (!currentSession || currentSession->userId() != userId) return;
        if (!result.ok()) {
            ++m_failed;
            m_totalLatencyMs += static_cast<std::uint64_t>(std::max<long long>(result.latencyMs, 0));
            // 上游正文只写服务端日志；客户端只收到稳定、无敏感信息的错误。
            log("[AI] 请求失败 user=", userId, " peer=", peerId,
                " requestId=", requestId, " http=", result.httpStatus,
                " error=", result.errorMessage);
            sendResult(currentSession, requestId, mapError(result.error), "AI 服务暂时不可用");
            logMetrics();
            return;
        }

        std::vector<std::string> safeSuggestions;
        for (const auto& suggestion : result.suggestions) {
            const auto safe = im::proto::utf8Truncate(suggestion, 240);
            if (!safe.empty() && safeSuggestions.size() < 3) safeSuggestions.push_back(safe);
        }
        if (safeSuggestions.empty()) {
            ++m_failed;
            sendResult(currentSession, requestId, im::proto::AI_REPLY_UPSTREAM_ERROR,
                       "AI 返回内容无效");
            logMetrics();
            return;
        }
        ++m_succeeded;
        m_totalLatencyMs += static_cast<std::uint64_t>(std::max<long long>(result.latencyMs, 0));
        sendResult(currentSession, requestId, im::proto::AI_REPLY_OK, {}, safeSuggestions);
        log("[AI] 请求成功 user=", userId, " peer=", peerId,
            " candidates=", safeSuggestions.size(), " inputTokens=", result.inputTokens,
            " outputTokens=", result.outputTokens, " latencyMs=", result.latencyMs);
        logMetrics();
    });
}

} // namespace imsrv
