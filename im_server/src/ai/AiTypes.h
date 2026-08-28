#pragma once

#include <string>
#include <vector>

namespace imsrv::ai {

struct AiConversationTurn {
    enum class Role { Me, Peer };
    Role role{Role::Peer};
    std::string text;
};

struct AiReplyRequest {
    std::string requestId;
    std::vector<AiConversationTurn> turns;
    std::string tone{"自然、简洁"};
    int maxSuggestions{3};
};

enum class AiError {
    None,
    NotConfigured,
    Network,
    Timeout,
    Unauthorized,
    RateLimited,
    Service,
    InvalidResponse
};

struct AiReplyResult {
    AiError error{AiError::None};
    std::vector<std::string> suggestions;
    std::string errorMessage;
    int httpStatus{0};
    int inputTokens{0};
    int outputTokens{0};
    long long latencyMs{0};

    bool ok() const noexcept { return error == AiError::None; }
};

} // namespace imsrv::ai
