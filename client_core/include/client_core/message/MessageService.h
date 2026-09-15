// 消息服务：Outbox/Inbox 统一合并入口（P7-G4）。
//
// 契约（v2 §3 P7-G4 文本部分）：
//   - 发送：UI intent → Native 生成 msg_id 与 local_order → 单事务写
//     messages(SENDING) + outbox + 会话预览 → commit 后回显 → 加密通道发送 →
//     Ack 按 msg_id 合并 server_time / conversation_seq / status 并删除 outbox；
//   - 接收：**Ack、Push、离线和漫游都进入同一 Inbox 合并入口**
//     （onIncoming），保证"无论从哪条路径来，行为一致"；
//   - 幂等与顺序分离：`msg_id` 只负责幂等，`conversation_seq` 只负责顺序/缺洞，
//     不得混用（这是 P6 已确立、G4 必须维持的不变式）；
//   - 未读只在**首次提交的非本人新消息**且会话未读时增加；
//   - 发送尚未 Ack 的本地消息用 `(local_order, msg_id)` 稳定排序，
//     已确认消息用 `(conversation_seq, msg_id)`——两者不共享不稳定的排序键。

#ifndef CLIENT_CORE_MESSAGE_MESSAGE_SERVICE_H
#define CLIENT_CORE_MESSAGE_MESSAGE_SERVICE_H

#include <cstdint>
#include <functional>
#include <string>

#include "client_core/dto/Dtos.h"
#include "client_core/storage/NativeRepository.h"

namespace im {
namespace message {

struct SendResult {
    bool ok = false;
    std::string msgId;
    std::int64_t localOrder = 0;
    std::string error;
};

struct IncomingResult {
    bool ok = false;
    bool inserted = false; // false 表示已存在（幂等跳过），非错误
    std::string error;
};

class MessageService {
public:
    explicit MessageService(im::storage::NativeRepository* repo) : m_repo(repo) {}

    /**
     * 注入 ID 生成器（测试用，保持确定性）。
     * 生产环境使用 128 位密码学随机 ID，
     * 避免内存计数器重启归零导致 msg_id 重复、被唯一索引当成重复消息（第 1 点缺陷）。
     */
    void setIdGenerator(std::function<std::string()> gen) { m_idGen = std::move(gen); }

    // ---------------- 发送（Outbox） ----------------

    /**
     * 准备一条待发送消息：在 C++ 发号（msg_id + local_order），
     * 单事务写 messages(SENDING) + outbox + 会话预览。commit 后才可回显。
     */
    SendResult prepareOutgoing(const im::dto::MessageDto& intent);

    /**
     * Ack 合并：按 msg_id 定位，写入 server_time / conversation_seq / status
     * 并删除 outbox，单事务。重复 Ack 幂等。
     */
    bool onSendAck(std::int64_t ownerId, const std::string& msgId, std::int64_t serverTime,
                   std::int64_t conversationSeq, std::int32_t status,
                   bool* updated = nullptr, std::string* err = nullptr);
    // Maps actual ChatInfoRs.result (0 online / 1 offline / 2,4 permanent / 3 retry).
    bool onProtocolAck(std::int64_t ownerId, const std::string& msgId,
                         int result, std::int64_t seq, std::int64_t attempt,
                         std::int64_t nowSeconds, std::string* err = nullptr,
                         std::int64_t* retryAt = nullptr);

    // ---------------- 接收（Inbox 统一入口） ----------------

    /**
     * Ack / Push / 离线 / 漫游的统一入口。
     * 幂等由 msg_id 保证；顺序与缺洞交由调用方配合 SyncTracker 处理。
     * @param ctx 接收上下文（isRoaming / activeConversationId）：决定是否计入未读
     */
    IncomingResult onIncoming(const im::dto::MessageDto& m,
                              const im::storage::IncomingContext& ctx =
                                  im::storage::IncomingContext{});
    bool markRead(std::int64_t ownerId,std::int64_t conversationId,std::int64_t readSeq,
                  std::string* err=nullptr);

private:
    im::storage::NativeRepository* m_repo;
    std::function<std::string()> m_idGen; // 默认在 .cpp 中实现（重启后唯一）
};

} // namespace message
} // namespace im

#endif // CLIENT_CORE_MESSAGE_MESSAGE_SERVICE_H
