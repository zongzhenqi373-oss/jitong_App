#pragma once

#include "ai/AiConfig.h"
#include "ai/IModelClient.h"

#include <string>

namespace imsrv::ai {

class OpenAIModelClient final : public IModelClient {
public:
    explicit OpenAIModelClient(AiConfig config);

    bool isConfigured() const noexcept;
    AiReplyResult generateReplySuggestions(const AiReplyRequest& request) override;

    // 公开纯解析函数，便于在不访问网络、不消耗额度的情况下单元测试。
    static AiReplyResult parseResponseBody(const std::string& body);

private:
    std::string buildRequestBody(const AiReplyRequest& request) const;

    AiConfig config_;
};

} // namespace imsrv::ai
