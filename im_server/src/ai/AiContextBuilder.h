#pragma once

#include "Database.h"
#include "ai/AiTypes.h"

#include <string>
#include <vector>

namespace imsrv::ai {

struct AiContextPolicy {
    int maxMessages{20};
    int maxMessageBytes{2000};
    int maxContextBytes{12000};
};

// rows 必须是数据库 roamMessages() 的 seq 倒序结果；输出按时间正序。
AiReplyRequest buildReplyContext(const std::vector<StoredMessage>& rows,
                                 int userId, int peerId,
                                 std::string requestId, std::string tone,
                                 int maxSuggestions,
                                 const AiContextPolicy& policy);

// 只处理准备发给模型的内存副本，不修改数据库原文。
std::string redactSensitiveText(const std::string& text);

} // namespace imsrv::ai
