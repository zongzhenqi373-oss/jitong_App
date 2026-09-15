// AI 候选回复服务（P7-G4）。
//
// 职责：候选回复的状态机编排 + 结果落库。
//
// 关键语义（v2 §3 P7-G4）：
//   - 请求时**冻结上下文**（conversationId/tone/contextVersion），结果回填时校验
//     该冻结值，换会话/换号/上下文变化导致的迟到响应一律抑制；
//   - 同一会话 **single-flight**：已有 in-flight 时重复请求复用同一 requestId，不并发；
//   - **取消是 best-effort**：取消后状态落 Aborted，此后到达的响应（迟到）不得展示；
//   - **超时**：20s 未返回，tick() 将其置为 Error(超时码)，旧响应失效；
//   - **generation 失效**：invalidate()（换号/重登录）后所有 in-flight 作废，迟到响应抑制。
//
// 网络收发由上层 transport/SDK 接线驱动；本类只做本地状态决策与落库编排。
// 候选正文不得写日志（仅加密落库，SQLCipher）。

#ifndef CLIENT_CORE_AI_AI_SUGGESTION_SERVICE_H
#define CLIENT_CORE_AI_AI_SUGGESTION_SERVICE_H

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "client_core/dto/Dtos.h"
#include "client_core/storage/NativeRepository.h"
#include "client_core/testing/DeterministicClock.h"

namespace im {
namespace ai {

struct AiRequestContext {
    std::int64_t conversationId = 0;
    std::int64_t peerId = 0;
    std::string tone;
    std::string contextVersion; // 冻结的上下文版本
};

class AiSuggestionService {
public:
    static constexpr std::int64_t kTimeoutMs = 20000; // 20s 超时
    static constexpr std::int32_t kErrorTimeout = 1001; // 超时错误码

    struct RequestResult {
        bool ok = false;
        bool singleFlighted = false; // 同会话已有 in-flight，未新建
        std::string requestId;
        std::string error;
    };

    AiSuggestionService(std::int64_t ownerId, im::storage::NativeRepository* repo,
                        im::testing::IClock* clock)
        : m_ownerId(ownerId), m_repo(repo), m_clock(clock) {}

    /** 发起候选回复请求（冻结上下文 + single-flight + 20s 超时）。 */
    RequestResult requestAiReply(const AiRequestContext& ctx);

    /** 取消（best-effort）：置 Aborted；此后迟到响应抑制。 */
    bool cancelAiReply(const std::string& requestId);

    /** 收到结果：校验 generation/上下文后落库；迟到/失效响应抑制。 */
    void onAiResult(const std::string& requestId, im::dto::AiStatus status,
                    const std::vector<std::string>& suggestions, std::int32_t errorCode);

    /** 换号/重登录：generation 递增，所有 in-flight 作废。 */
    void invalidate();

    /** 周期驱动：检查并关闭超时请求（由上层 scheduler/循环调用）。 */
    void tick();

    /** 按会话读 AI 建议（落库快照，UI 重建可恢复）。 */
    bool loadSuggestions(std::int64_t conversationId,
                         std::vector<im::dto::AiSuggestionDto>* out,
                         std::string* err = nullptr);

private:
    struct InFlight {
        std::string requestId;
        std::int64_t conversationId;
        std::int64_t peerId;
        std::string tone;
        std::string contextVersion;
        std::int64_t generation;
        std::int64_t deadlineMs;
    };

    std::string makeRequestId();

    std::int64_t m_ownerId;
    im::storage::NativeRepository* m_repo;
    im::testing::IClock* m_clock;

    std::mutex m_mutex;
    std::int64_t m_generation = 0;
    std::uint64_t m_seq = 0;
    std::unordered_map<std::string, InFlight> m_inFlight; // requestId → 请求
    std::unordered_map<std::int64_t, std::string> m_byConv; // conversationId → requestId
};

} // namespace ai
} // namespace im

#endif // CLIENT_CORE_AI_AI_SUGGESTION_SERVICE_H
