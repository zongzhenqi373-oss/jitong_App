#pragma once

#include <string>

namespace imsrv::ai {

enum class AiProvider { OpenAI, Anthropic };

struct AiConfig {
    bool featureEnabled{true};
    int rolloutPercent{100};
    AiProvider provider{AiProvider::OpenAI};
    std::string apiKey;
    std::string model;
    std::string host{"api.openai.com"};
    int port{443};
    std::string path{"/v1/responses"};
    int requestTimeoutMs{15000};
    int maxOutputTokens{180};
    bool disableThinking{true};
    int maxContextMessages{20};
    int maxContextBytes{12000};
    int maxMessageBytes{2000};
    int rateLimitPerMinute{5};
    int maxPendingRequests{8};
    int replayWindowSeconds{300};
    std::string caCertPath;
    std::string anthropicVersion{"2023-06-01"};

    static AiConfig fromEnvironment();
    bool enabled() const noexcept;
    std::string validationError() const;
};

} // namespace imsrv::ai
