#include "ai/AiConfig.h"
#include "ai/AnthropicModelClient.h"
#include "ai/IModelClient.h"
#include "ai/OpenAIModelClient.h"

#include <iostream>
#include <memory>

int main() {
    using imsrv::ai::AiConversationTurn;
    const auto config = imsrv::ai::AiConfig::fromEnvironment();
    if (!config.enabled()) {
        std::cerr << "AI API 未配置：" << config.validationError() << '\n'
                  << "请编辑 im_server/config/ai.env 后，将变量加载到当前终端。\n";
        return 2;
    }

    imsrv::ai::AiReplyRequest request;
    request.requestId = "manual-smoke-test";
    request.turns = {
        {AiConversationTurn::Role::Peer, "今天晚上有时间吗？"},
        {AiConversationTurn::Role::Me, "可能要晚一点。"},
        {AiConversationTurn::Role::Peer, "大概几点？"}
    };

    std::unique_ptr<imsrv::ai::IModelClient> client;
    if (config.provider == imsrv::ai::AiProvider::Anthropic) {
        client = std::make_unique<imsrv::ai::AnthropicModelClient>(config);
    } else {
        client = std::make_unique<imsrv::ai::OpenAIModelClient>(config);
    }
    const auto result = client->generateReplySuggestions(request);
    if (!result.ok()) {
        std::cerr << "AI API 测试失败: " << result.errorMessage
                  << " (HTTP " << result.httpStatus << ")\n";
        return 1;
    }

    std::cout << "AI API 测试成功，候选回复：\n";
    for (const auto& suggestion : result.suggestions) {
        std::cout << "- " << suggestion << '\n';
    }
    std::cout << "tokens: input=" << result.inputTokens
              << " output=" << result.outputTokens
              << " latencyMs=" << result.latencyMs << '\n';
    return 0;
}
