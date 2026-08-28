#include "ai/AiContextBuilder.h"

#include "client_core/Protocol.h"
#include "im.pb.h"

#include <algorithm>
#include <regex>

namespace imsrv::ai {
namespace {

std::string replaceAll(std::string input, const std::regex& pattern,
                       const std::string& replacement) {
    return std::regex_replace(input, pattern, replacement);
}

} // namespace

std::string redactSensitiveText(const std::string& text) {
    // 规则刻意保守，只覆盖高置信度敏感字段，避免把普通数字和正常语义误删。
    static const std::regex email(
        R"([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,})");
    static const std::regex mobile(R"((^|[^0-9])1[3-9][0-9]{9}([^0-9]|$))");
    static const std::regex idCard(R"((^|[^0-9])[1-9][0-9]{16}[0-9Xx]([^0-9]|$))");
    static const std::regex apiKey(R"((sk-[A-Za-z0-9_\-]{8,}))");

    std::string result = replaceAll(text, email, "[邮箱已隐藏]");
    result = replaceAll(result, mobile, "$1[手机号已隐藏]$2");
    result = replaceAll(result, idCard, "$1[身份证号已隐藏]$2");
    result = replaceAll(result, apiKey, "[密钥已隐藏]");
    return result;
}

AiReplyRequest buildReplyContext(const std::vector<StoredMessage>& rows,
                                 int userId, int peerId,
                                 std::string requestId, std::string tone,
                                 int maxSuggestions,
                                 const AiContextPolicy& policy) {
    AiReplyRequest request;
    request.requestId = std::move(requestId);
    request.tone = std::move(tone);
    request.maxSuggestions = std::clamp(maxSuggestions, 1, 3);

    std::vector<AiConversationTurn> newestFirst;
    std::size_t usedBytes = 0;
    const std::size_t totalLimit = static_cast<std::size_t>(std::max(policy.maxContextBytes, 1));
    const std::size_t messageLimit = static_cast<std::size_t>(std::max(policy.maxMessageBytes, 1));
    const int messageCount = std::max(policy.maxMessages, 1);

    for (const auto& row : rows) {
        if (static_cast<int>(newestFirst.size()) >= messageCount) break;
        if (row.type != static_cast<int>(im::proto::TEXT) || row.content.empty()) continue;
        const bool belongs = (row.senderId == userId && row.receiverId == peerId) ||
                             (row.senderId == peerId && row.receiverId == userId);
        if (!belongs) continue;

        std::string sanitized = redactSensitiveText(row.content);
        sanitized = im::proto::utf8Truncate(sanitized, messageLimit);
        if (sanitized.empty()) continue;
        const std::size_t remaining = totalLimit - std::min(usedBytes, totalLimit);
        if (remaining == 0) break;
        sanitized = im::proto::utf8Truncate(sanitized, remaining);
        if (sanitized.empty()) break;

        newestFirst.push_back({
            row.senderId == userId ? AiConversationTurn::Role::Me
                                   : AiConversationTurn::Role::Peer,
            std::move(sanitized)
        });
        usedBytes += newestFirst.back().text.size();
    }

    request.turns.assign(newestFirst.rbegin(), newestFirst.rend());
    return request;
}

} // namespace imsrv::ai
