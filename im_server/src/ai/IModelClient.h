#pragma once

#include "ai/AiTypes.h"

namespace imsrv::ai {

class IModelClient {
public:
    virtual ~IModelClient() = default;
    virtual AiReplyResult generateReplySuggestions(const AiReplyRequest& request) = 0;
};

} // namespace imsrv::ai
