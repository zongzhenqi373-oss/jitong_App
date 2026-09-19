// Native 业务级 Repository（P7-G2）。
//
// 目的：把"一条业务原子操作"封装成**一个 Writer 事务**，禁止 Service 层拼接多个裸 DAO
// 操作（那会产生半事务可见、FTS 与消息不一致、未读与水位不一致等问题）。
//
// 不变式：
//   - 每个方法对应一个业务事务，由 DbCommandQueue 保证 BEGIN IMMEDIATE … COMMIT/ROLLBACK；
//   - 消息与 FTS/identity 在同一事务内维护；
//   - 会话 preview、unread、watermark 与消息提交同事务；
//   - 媒体引用计数与消息提交同事务（物理删文件只能在 commit 之后，不在本层做）；
//   - 所有写都经 Writer 队列串行，绝不并发写 SQL。

#ifndef CLIENT_CORE_NATIVE_REPOSITORY_H
#define CLIENT_CORE_NATIVE_REPOSITORY_H

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "client_core/dto/Dtos.h"
#include "client_core/storage/DbCommandQueue.h"
#include "client_core/storage/NativeDatabase.h"

namespace im {
namespace storage {

/**
 * 接收上下文：决定"是否计入未读"。
 * 未读只在**首次提交的非本人新消息、seq 超过已读水位、且非历史漫游、
 * 且不是当前前台会话**时增加——否则漫游一个会话就会把未读灌满。
 *
 * 说明：定义在 NativeRepository 之外，是因为带默认成员初始化器的嵌套类型
 * 不能安全地用作外层类成员函数声明中的默认参数（C++ 求值顺序限制）。
 */
struct IncomingContext {
    bool isRoaming = false;                // 历史漫游批量导入：不计未读
    std::int64_t activeConversationId = 0; // 当前前台会话：不计未读
};

struct OutboxAttempt {
    std::string msgId;
    std::string payload;
    std::int32_t packetType = 0;
    std::int64_t attempt = 0;
    std::int64_t localOrder = 0;
};

/** 会话历史的稳定 keyset 游标；四个字段共同对应查询排序键。 */
struct MessageCursor {
    std::int64_t serverTime = 0;
    std::int64_t conversationSeq = 0;
    std::int64_t localOrder = 0;
    std::string msgId;
};

struct MessagePage {
    std::vector<im::dto::MessageDto> messages;
    bool hasMore = false;
    MessageCursor next;
};

/** sync_gaps 表的行（缺洞区间 + 退避信息）。 */
struct SyncGapRow {
    std::int64_t gapFrom = 0;
    std::int64_t gapTo = 0;
    int attempt = 0;
    std::int64_t nextRetryAtMs = 0;
};
struct MessageSeqRange { std::int64_t from=0; std::int64_t to=0; };

struct DownloadTaskRow {
    std::string taskId;
    std::string msgId;
    std::string fileId;
    std::string localPath;
    std::string expectedSha256;
    std::int64_t totalSize = 0;
    std::int64_t transferred = 0;
    std::int64_t generation = 0;
};

class NativeRepository {
public:
    explicit NativeRepository(NativeDatabase* db) : m_db(db) {}
    explicit NativeRepository(std::shared_ptr<NativeDatabase> db)
        : m_dbOwner(std::move(db)), m_db(m_dbOwner.get()) {}

    // 下载任务由加密库单 Writer 持久化；取消/完成是终态，不被启动恢复重新领取。
    bool beginDownloadTask(std::int64_t ownerId,const std::string& taskId,
                           const std::string& msgId,const std::string& fileId,
                           const std::string& localPath,std::int64_t generation,
                           std::string* err=nullptr);
    bool finishDownloadTask(std::int64_t ownerId,const std::string& taskId,
                            std::int64_t generation,int state,std::int64_t transferred,
                            std::string* err=nullptr);
    bool listRecoverableDownloads(std::int64_t ownerId,std::vector<DownloadTaskRow>* out,
                                  std::string* err=nullptr);

    /**
     * 接收消息提交（Inbox 统一入口：Ack/Push/离线/漫游都走这里）。
     * 单事务：messages upsert（msg_id 幂等，且补齐既有记录缺失的 seq/status/server_time）
     * + FTS/identity + 会话 preview（仅当更新时才覆盖）+ 未读（按 ctx 与已读水位判断）
     * + 水位（maxseen 单调递增，不回退）。
     * @param inserted 非 null 时接收"是否真正新增"（false 表示已存在）
     */
    bool commitIncomingMessage(const im::dto::MessageDto& m, bool* inserted = nullptr,
                               std::string* err = nullptr,
                               const IncomingContext& ctx = IncomingContext{});

    /** 发送草稿提交：messages(SENDING) + outbox + 会话 preview，单事务。 */
    bool commitOutgoingDraft(const im::dto::MessageDto& m, std::string* err = nullptr,
                             std::int64_t* allocatedOrder = nullptr);
    bool findMessage(std::int64_t ownerId, const std::string& msgId, im::dto::MessageDto* out);
    /**
     * 按新到旧分页读取会话消息。cursor=nullptr 读取首页；后续传回上一页 next。
     * 使用完整 keyset，不使用 OFFSET，避免同毫秒消息漏读及插入新消息导致翻页漂移。
     */
    bool listConversation(std::int64_t ownerId, std::int64_t conversationId,
                          const MessageCursor* cursor, int limit, MessagePage* out,
                          std::string* err = nullptr);
    /** 按最后消息时间倒序读取账号会话快照；供 invalidation 后整表重查。 */
    bool loadConversations(std::int64_t ownerId,std::vector<im::dto::ConversationDto>* out,
                           std::string* err = nullptr);
    // A lease is durable; another claimant cannot send the same attempt until it expires.
    bool claimOutbox(std::int64_t ownerId, std::int64_t nowSeconds, std::int64_t leaseSeconds,
                      int limit, std::vector<OutboxAttempt>* out, std::string* err = nullptr);
    bool finishOutboxAttempt(std::int64_t ownerId, const std::string& msgId,
                              std::int64_t attempt, bool terminal, std::int64_t retryAt,
                              const std::string& error, std::string* err = nullptr);
    bool cancelOutbox(std::int64_t ownerId, const std::string& msgId, std::string* err = nullptr);

    /**
     * Ack 提交：写 server_time/conversation_seq/status 并删除 outbox，单事务。
     * 幂等：重复 Ack 不产生错误结果。
     */
    bool commitAck(std::int64_t ownerId, const std::string& msgId, std::int64_t serverTime,
                   std::int64_t conversationSeq, std::int32_t status,
                   bool* updated = nullptr, std::string* err = nullptr);

    /** 已读：原子更新 read_seq 与 unread（多设备已读同步属增强项，不在此处假定）。 */
    bool markConversationRead(std::int64_t ownerId, std::int64_t conversationId,
                              std::int64_t readSeq, std::string* err = nullptr);

    /**
     * 媒体 + 消息提交：media_variants + media_refs + messages 在同一事务内更新。
     * 物理删文件必须在 commit 之后由上层执行，不在本事务内做。
     */
    bool upsertMediaAndMessage(const std::vector<im::dto::MediaTaskDto>& variants,
                               const im::dto::MessageDto& m, std::string* err = nullptr);

    // ---------------- 好友域（P7-G4） ----------------
    // FriendDto/FriendRequestDto 是纯值对象，不含 ownerId（owner 属调用上下文），
    // 故 ownerId 作为显式参数传入，与 MessageDto（消息按 owner 隔离、内置 ownerId）不同。

    /** 好友资料幂等落库：不存在插入、存在全量覆盖更新（服务端资料为权威）。 */
    bool upsertFriend(std::int64_t ownerId, const im::dto::FriendDto& f,
                      std::string* err = nullptr);

    /** 删除好友（仅删 friends 事实，不影响消息/会话历史）。 */
    bool deleteFriend(std::int64_t ownerId, std::int64_t friendId, bool* deleted = nullptr,
                      std::string* err = nullptr);

    /** 好友申请幂等落库（request_id 唯一，重复不增行）。 */
    bool upsertFriendRequest(std::int64_t ownerId, const im::dto::FriendRequestDto& r,
                             bool* inserted = nullptr, std::string* err = nullptr);

    /** 更新申请状态（pending→accepted/rejected）。 */
    bool setFriendRequestState(std::int64_t ownerId, const std::string& requestId,
                               im::dto::RequestState state, std::string* err = nullptr);

    /**
     * 同意申请：同一事务内 申请状态→Accepted + 好友资料落库。
     * 若申请不存在或非 pending，幂等返回（不重复插好友）。
     */
    bool acceptFriendRequest(std::int64_t ownerId, const std::string& requestId,
                             const im::dto::FriendDto& accepted, std::string* err = nullptr);

    // ---------------- 只读查询（ReadPool） ----------------

    bool loadFriends(std::int64_t ownerId, std::vector<im::dto::FriendDto>* out,
                     std::string* err = nullptr);
    bool loadFriendRequests(std::int64_t ownerId,
                            std::vector<im::dto::FriendRequestDto>* out,
                            std::string* err = nullptr);

    // ---------------- AI 候选回复（P7-G4） ----------------

    /** AI 建议幂等落库（request_id 唯一，重复覆盖）。suggestions 用 US(0x1F) 分隔。 */
    bool upsertAiSuggestion(std::int64_t ownerId, const im::dto::AiSuggestionDto& s,
                            std::string* err = nullptr);

    /** 按会话读 AI 建议（按 generated_at 升序，最新在后）。 */
    bool loadAiSuggestions(std::int64_t ownerId, std::int64_t conversationId,
                           std::vector<im::dto::AiSuggestionDto>* out,
                           std::string* err = nullptr);

    // ---------------- 同步缺洞（P7-G4） ----------------

    /** 全量重建某会话的 sync_gaps（DELETE + INSERT），与内存缺洞保持一致。 */
    bool replaceSyncGaps(std::int64_t ownerId, std::int64_t conversationId,
                         const std::vector<SyncGapRow>& gaps, std::string* err = nullptr);

    /** 读某会话的 sync_gaps（用于恢复/诊断）。 */
    bool loadSyncGaps(std::int64_t ownerId, std::int64_t conversationId,
                      std::vector<SyncGapRow>* out, std::string* err = nullptr);
    /** 从权威消息表按索引扫描并压缩为已到达 seq 区间，用于 SyncTracker 冷启动恢复。 */
    bool loadMessageSeqRanges(std::int64_t ownerId,std::int64_t conversationId,
                              std::vector<MessageSeqRange>* out,std::string* err = nullptr);

    // ---------------- 分片上传草稿（P7 媒体断点续传） ----------------

    /** 登记/复用上传草稿（同 owner+msgId+variant 幂等：已存在则保留进度，不重置）。 */
    bool upsertUploadDraft(std::int64_t ownerId, const im::dto::UploadDraftDto& d,
                           bool* inserted = nullptr, std::string* err = nullptr);

    /** 写入服务端会话信息（uploadId/chunkSize/chunkCount），状态 Pending→Uploading。 */
    bool setUploadDraftSession(std::int64_t ownerId, const std::string& msgId,
                               im::dto::MediaVariant variant, const std::string& uploadId,
                               std::int64_t chunkSize, std::int32_t chunkCount,
                               std::string* err = nullptr);

    /** 确认一个分片完成（幂等：重复分片号不重复记录）。 */
    bool markUploadChunkDone(std::int64_t ownerId, const std::string& msgId,
                             im::dto::MediaVariant variant, std::int32_t chunkIndex,
                             std::string* err = nullptr);

    /** finalize 成功：回填 file_id，状态→Finalized（同事务）。 */
    bool setUploadDraftFileId(std::int64_t ownerId, const std::string& msgId,
                              im::dto::MediaVariant variant, const std::string& fileId,
                              std::string* err = nullptr);

    /** 服务端会话丢失（404/410）时重置：换 upload_id、清空分片进度、回到 Uploading。 */
    bool resetUploadDraftForNewSession(std::int64_t ownerId, const std::string& msgId,
                                       im::dto::MediaVariant variant, const std::string& uploadId,
                                       std::int64_t chunkSize, std::int32_t chunkCount,
                                       std::string* err = nullptr);

    /** 状态推进（Sent/Cancelled/Failed/Uploading 等），终态只允许从非终态进入。 */
    bool setUploadDraftState(std::int64_t ownerId, const std::string& msgId,
                             im::dto::MediaVariant variant, im::dto::UploadDraftState state,
                             std::int32_t errorCode, std::string* err = nullptr);

    /** 读单条草稿。不存在返回 false。 */
    bool getUploadDraft(std::int64_t ownerId, const std::string& msgId,
                        im::dto::MediaVariant variant, im::dto::UploadDraftDto* out,
                        std::string* err = nullptr);

    /** 启动恢复扫描：某 owner 全部活跃草稿（Pending/Uploading/Finalized/Failed）。 */
    bool loadActiveUploadDrafts(std::int64_t ownerId,
                                std::vector<im::dto::UploadDraftDto>* out,
                                std::string* err = nullptr);

private:
    static bool mergeIncoming(sqlite3* db, const im::dto::MessageDto& m,
                              bool* inserted, const IncomingContext& ctx);
    std::shared_ptr<NativeDatabase> m_dbOwner;
    NativeDatabase* m_db;

    // 内部：在 Writer 线程执行一个事务并等待；统一处理超时与错误
    bool runTx(const DbCommandQueue::WriteFn& fn, std::string* err);
};

} // namespace storage
} // namespace im

#endif // CLIENT_CORE_NATIVE_REPOSITORY_H
