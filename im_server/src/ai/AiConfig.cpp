#include "ai/AiConfig.h"

#include <algorithm>
#include <cstdlib>

namespace imsrv::ai {
namespace {

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

int envInt(const char* name, int fallback, int minimum, int maximum) {
    const std::string value = env(name);
    if (value.empty()) return fallback;
    try {
        const long parsed = std::stol(value);
        if (parsed < minimum || parsed > maximum) return fallback;
        return static_cast<int>(parsed);
    } catch (...) {
        return fallback;
    }
}

bool envBool(const char* name, bool fallback) {
    const std::string value = env(name);
    if (value.empty()) return fallback;
    return value == "1" || value == "true" || value == "TRUE" ||
           value == "yes" || value == "on";
}

void applyBaseUrl(const std::string& baseUrl, AiConfig& config) {
    if (baseUrl.empty()) return;
    std::string value = baseUrl;
    if (value.rfind("https://", 0) == 0) {
        value.erase(0, 8);
        config.port = 443;
    } else if (value.rfind("http://", 0) == 0) {
        value.erase(0, 7);
        config.port = 80;
    }
    const auto slash = value.find('/');
    config.host = value.substr(0, slash);
    if (slash != std::string::npos && slash + 1 < value.size()) {
        config.path = value.substr(slash);
        while (!config.path.empty() && config.path.back() == '/') config.path.pop_back();
        config.path += "/v1/messages";
    }
}

} // namespace

AiConfig AiConfig::fromEnvironment() {
    AiConfig config;
    config.featureEnabled = envBool("IM_AI_ENABLED", true);
    config.rolloutPercent = envInt("IM_AI_ROLLOUT_PERCENT", 100, 0, 100);
    const auto baseUrl = env("IM_AI_BASE_URL");
    const auto provider = env("IM_AI_PROVIDER");
    const bool useAnthropic = provider == "anthropic";
    if (useAnthropic) {
        config.provider = AiProvider::Anthropic;
        config.host = "api.anthropic.com";
        config.path = "/v1/messages";
        config.apiKey = env("IM_AI_API_KEY");
        config.model = env("IM_AI_MODEL");
        applyBaseUrl(baseUrl, config);
        const auto version = env("IM_AI_ANTHROPIC_VERSION");
        if (!version.empty()) config.anthropicVersion = version;
    } else {
        config.apiKey = env("IM_AI_API_KEY");
        config.model = env("IM_AI_MODEL");
        applyBaseUrl(baseUrl, config);
    }

    const auto host = env("IM_AI_API_HOST");
    const auto path = env("IM_AI_API_PATH");
    const auto caCertPath = env("IM_AI_CA_CERT_PATH");
    if (!host.empty()) applyBaseUrl(host, config);
    if (!path.empty()) config.path = path;
    if (!caCertPath.empty()) config.caCertPath = caCertPath;

    config.port = envInt("IM_AI_API_PORT", 443, 1, 65535);
    config.requestTimeoutMs = envInt("IM_AI_REQUEST_TIMEOUT_MS", 15000, 1000, 120000);
    config.maxOutputTokens = envInt("IM_AI_MAX_OUTPUT_TOKENS", 180, 64, 2000);
    config.disableThinking = envBool("IM_AI_DISABLE_THINKING", true);
    config.maxContextMessages = envInt("IM_AI_MAX_CONTEXT_MESSAGES", 20, 1, 100);
    config.maxContextBytes = envInt("IM_AI_MAX_CONTEXT_BYTES", 12000, 1000, 100000);
    config.maxMessageBytes = envInt("IM_AI_MAX_MESSAGE_BYTES", 2000, 100, 10000);
    config.rateLimitPerMinute = envInt("IM_AI_RATE_LIMIT_PER_MINUTE", 5, 1, 60);
    config.maxPendingRequests = envInt("IM_AI_MAX_PENDING_REQUESTS", 8, 1, 100);
    config.replayWindowSeconds = envInt("IM_AI_REPLAY_WINDOW_SECONDS", 300, 30, 3600);
    return config;
}

bool AiConfig::enabled() const noexcept {
    return featureEnabled && !apiKey.empty() && !model.empty();
}

std::string AiConfig::validationError() const {
    if (!featureEnabled) return "AI 功能已关闭";
    if (apiKey.empty()) return "缺少 IM_AI_API_KEY";
    if (model.empty()) return "缺少 IM_AI_MODEL";
    if (host.empty()) return "IM_AI_API_HOST 不能为空";
    if (path.empty() || path.front() != '/') return "IM_AI_API_PATH 必须以 / 开头";
    if (port < 1 || port > 65535) return "IM_AI_API_PORT 无效";
    return {};
}

} // namespace imsrv::ai
