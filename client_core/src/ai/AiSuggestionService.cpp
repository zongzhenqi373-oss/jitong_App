#include "client_core/ai/AiSuggestionService.h"

namespace im {
namespace ai {

std::string AiSuggestionService::makeRequestId()
{
    // 单次运行内唯一（候选建议是瞬态的，不要求跨重启唯一）。
    return "ai-" + std::to_string(m_seq++);
}

AiSuggestionService::RequestResult
AiSuggestionService::requestAiReply(const AiRequestContext& ctx)
{
    RequestResult r;
    if (!m_repo || !m_clock) {
        r.error = "依赖为空";
        return r;
    }
    if (ctx.conversationId <= 0) {
        r.error = "conversationId 非法";
        return r;
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    // single-flight：同会话已有 in-flight，复用同一 requestId，不并发
    auto it = m_byConv.find(ctx.conversationId);
    if (it != m_byConv.end()) {
        r.ok = true;
        r.singleFlighted = true;
        r.requestId = it->second;
        return r;
    }

    const std::string id = makeRequestId();
    InFlight f;
    f.requestId = id;
    f.conversationId = ctx.conversationId;
    f.peerId = ctx.peerId;
    f.tone = ctx.tone;
    f.contextVersion = ctx.contextVersion;
    f.generation = m_generation;
    f.deadlineMs = m_clock->nowMs() + kTimeoutMs;
    m_inFlight[id] = f;
    m_byConv[ctx.conversationId] = id;

    r.ok = true;
    r.requestId = id;
    return r;
}

bool AiSuggestionService::cancelAiReply(const std::string& requestId)
{
    if (!m_repo || !m_clock) return false;
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_inFlight.find(requestId);
    if (it == m_inFlight.end()) return false; // 已完成/不存在

    const InFlight f = it->second;
    m_inFlight.erase(it);
    m_byConv.erase(f.conversationId);

    im::dto::AiSuggestionDto d;
    d.requestId = f.requestId;
    d.conversationId = f.conversationId;
    d.peerId = f.peerId;
    d.tone = f.tone;
    d.status = im::dto::AiStatus::Aborted;
    d.generatedAt = m_clock->nowMs();
    d.contextVersion = f.contextVersion;
    std::string err;
    m_repo->upsertAiSuggestion(m_ownerId, d, &err);
    return true;
}

void AiSuggestionService::onAiResult(const std::string& requestId, im::dto::AiStatus status,
                                     const std::vector<std::string>& suggestions,
                                     std::int32_t errorCode)
{
    if (!m_repo || !m_clock) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_inFlight.find(requestId);
    if (it == m_inFlight.end()) return; // 迟到/已完成/取消后到达：抑制

    const InFlight f = it->second;
    if (f.generation != m_generation) {
        // invalidate（换号/重登录）后到达的迟到响应：抑制，不落库
        m_inFlight.erase(it);
        m_byConv.erase(f.conversationId);
        return;
    }
    m_inFlight.erase(it);
    m_byConv.erase(f.conversationId);

    im::dto::AiSuggestionDto d;
    d.requestId = f.requestId;
    d.conversationId = f.conversationId;
    d.peerId = f.peerId;
    d.tone = f.tone;
    d.status = status;
    d.suggestions = suggestions;
    d.errorCode = errorCode;
    d.generatedAt = m_clock->nowMs();
    d.contextVersion = f.contextVersion;
    std::string err;
    m_repo->upsertAiSuggestion(m_ownerId, d, &err);
}

void AiSuggestionService::invalidate()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    ++m_generation;
    m_inFlight.clear();
    m_byConv.clear();
}

void AiSuggestionService::tick()
{
    if (!m_repo || !m_clock) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    const std::int64_t now = m_clock->nowMs();

    std::vector<InFlight> timedOut;
    for (auto it = m_inFlight.begin(); it != m_inFlight.end();) {
        if (it->second.deadlineMs <= now) {
            timedOut.push_back(it->second);
            m_byConv.erase(it->second.conversationId);
            it = m_inFlight.erase(it);
        } else {
            ++it;
        }
    }

    for (const auto& f : timedOut) {
        im::dto::AiSuggestionDto d;
        d.requestId = f.requestId;
        d.conversationId = f.conversationId;
        d.peerId = f.peerId;
        d.tone = f.tone;
        d.status = im::dto::AiStatus::Error;
        d.errorCode = kErrorTimeout;
        d.generatedAt = now;
        d.contextVersion = f.contextVersion;
        std::string err;
        m_repo->upsertAiSuggestion(m_ownerId, d, &err);
    }
}

bool AiSuggestionService::loadSuggestions(std::int64_t conversationId,
                                          std::vector<im::dto::AiSuggestionDto>* out,
                                          std::string* err)
{
    if (!m_repo || !out) {
        if (err) *err = "依赖为空";
        return false;
    }
    return m_repo->loadAiSuggestions(m_ownerId, conversationId, out, err);
}

} // namespace ai
} // namespace im
