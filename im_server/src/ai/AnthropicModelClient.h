#pragma once

#include "ai/AiConfig.h"
#include "ai/IModelClient.h"

namespace imsrv::ai {

class AnthropicModelClient final : public IModelClient {
public:
    explicit AnthropicModelClient(AiConfig config);
    AiReplyResult generateReplySuggestions(const AiReplyRequest& request) override;
    static AiReplyResult parseResponseBody(const std::string& body);

private:
    std::string buildRequestBody(const AiReplyRequest& request) const;
    AiConfig config_;
};

} // namespace imsrv::ai
