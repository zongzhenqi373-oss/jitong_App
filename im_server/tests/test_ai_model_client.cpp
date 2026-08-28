#include "ai/AiConfig.h"
#include "ai/AnthropicModelClient.h"
#include "ai/IModelClient.h"
#include "ai/OpenAIModelClient.h"
#include "client_core/Protocol.h"
#include "im.pb.h"

#include <cassert>
#include <cstdlib>
#include <iostream>

namespace {

class MockModelClient final : public imsrv::ai::IModelClient {
public:
    imsrv::ai::AiReplyResult generateReplySuggestions(
        const imsrv::ai::AiReplyRequest&) override {
        imsrv::ai::AiReplyResult result;
        result.suggestions = {"好的，我晚一点回复你。"};
        return result;
    }
};

} // namespace

int main() {
    {
        // IM 服务端只能读取 IM_AI_*；本机 Claude 的 ANTHROPIC_* 不得串入服务端。
#if defined(_WIN32)
        _putenv_s("ANTHROPIC_BASE_URL", "https://claude-proxy.invalid");
        _putenv_s("ANTHROPIC_AUTH_TOKEN", "claude-secret");
        _putenv_s("ANTHROPIC_MODEL", "claude-model");
        _putenv_s("IM_AI_PROVIDER", "anthropic");
        _putenv_s("IM_AI_BASE_URL", "https://im-ai.example/apps/anthropic");
        _putenv_s("IM_AI_API_KEY", "im-secret");
        _putenv_s("IM_AI_MODEL", "im-model");
#else
        setenv("ANTHROPIC_BASE_URL", "https://claude-proxy.invalid", 1);
        setenv("ANTHROPIC_AUTH_TOKEN", "claude-secret", 1);
        setenv("ANTHROPIC_MODEL", "claude-model", 1);
        setenv("IM_AI_PROVIDER", "anthropic", 1);
        setenv("IM_AI_BASE_URL", "https://im-ai.example/apps/anthropic", 1);
        setenv("IM_AI_API_KEY", "im-secret", 1);
        setenv("IM_AI_MODEL", "im-model", 1);
#endif
        const auto isolated = imsrv::ai::AiConfig::fromEnvironment();
        assert(isolated.provider == imsrv::ai::AiProvider::Anthropic);
        assert(isolated.host == "im-ai.example");
        assert(isolated.apiKey == "im-secret");
        assert(isolated.model == "im-model");
    }

    {
        im::proto::AiReplyRq request;
        request.set_request_id("ai-request-1");
        request.set_peer_id(2);
        request.set_tone("自然");
        request.set_max_suggestions(3);
        im::proto::AiReplyRq decoded;
        assert(decoded.ParseFromString(request.SerializeAsString()));
        assert(decoded.request_id() == "ai-request-1");
        assert(decoded.peer_id() == 2);
        assert(im::proto::DEF_PROT_AI_REPLY_RS - im::proto::DEF_BASE <
               im::proto::DEF_PROT_COUNT);

        im::proto::AiReplyRs response;
        response.set_request_id(decoded.request_id());
        response.set_status(im::proto::AI_REPLY_OK);
        response.add_suggestions("好的");
        assert(response.suggestions_size() == 1);
    }

    {
        imsrv::ai::AiConfig config;
        assert(!config.enabled());
        assert(!config.validationError().empty());
        config.apiKey = "test-key";
        config.model = "test-model";
        assert(config.enabled());
        assert(config.validationError().empty());
    }

    {
        const std::string response = R"json({
          "output": [{
            "type": "message",
            "content": [{
              "type": "output_text",
              "text": "{\"suggestions\":[\"八点左右可以。\",\"我大概八点有时间。\",\"晚点确定后告诉你。\"]}"
            }]
          }],
          "usage": {"input_tokens": 42, "output_tokens": 18}
        })json";
        const auto result = imsrv::ai::OpenAIModelClient::parseResponseBody(response);
        assert(result.ok());
        assert(result.suggestions.size() == 3);
        assert(result.suggestions[0] == "八点左右可以。");
        assert(result.inputTokens == 42);
        assert(result.outputTokens == 18);
    }

    {
        const std::string response = R"json({
          "content": [{"type":"text","text":"{\"suggestions\":[\"八点。\",\"八点左右。\"]}"}],
          "usage": {"input_tokens": 20, "output_tokens": 9}
        })json";
        const auto result = imsrv::ai::AnthropicModelClient::parseResponseBody(response);
        assert(result.ok());
        assert(result.suggestions.size() == 2);
        assert(result.inputTokens == 20);
        assert(result.outputTokens == 9);
    }

    {
        const auto result = imsrv::ai::OpenAIModelClient::parseResponseBody("{\"output\":[]}");
        assert(!result.ok());
        assert(result.error == imsrv::ai::AiError::InvalidResponse);
    }

    {
        MockModelClient client;
        imsrv::ai::AiReplyRequest request;
        request.turns.push_back({imsrv::ai::AiConversationTurn::Role::Peer, "在吗？"});
        const auto result = client.generateReplySuggestions(request);
        assert(result.ok());
        assert(result.suggestions.size() == 1);
    }

    std::cout << "AI model client tests passed\n";
    return 0;
}
