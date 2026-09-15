// Room → Native 影子迁移导入器（P6-T05）。
//
// 策略：**旧库只读导出、Native 幂等导入、校验通过后标记**。
// 本文件负责 Native 侧的"幂等导入 + checkpoint + 对账 + 完成标记"，不含 Room 导出
// （导出在 Kotlin 侧分页完成，经由 JNI 按批提交）。
//
// 不变式（对应 24.7）：
//   - 每批在**一个事务**中提交：消息 + FTS + 会话摘要 + checkpoint 一起原子化。
//   - 按 `(owner_id, msg_id)` **幂等**：重复导入不增行；禁止 `INSERT OR REPLACE`
//     覆盖不同内容（那会掩盖冲突）。
//   - 相同 `conversation_seq` 对应不同 msg_id 时报数据一致性错误并整体回滚。
//   - checkpoint 记录"最后已提交游标"，进程被杀后可从 checkpoint 续跑。
//   - 只有全部对账通过（消息数/会话数/seq 区间/FTS 行数）才写 `migration_completed=1`；
//     失败则保留旧 Room 为事实源并报告差异，**不悄悄跳过后宣称完成**。
//   - 媒体字段只搬字符串（路径/hash/尺寸），不移动/复制/删除文件。

#ifndef CLIENT_CORE_MIGRATION_IMPORTER_H
#define CLIENT_CORE_MIGRATION_IMPORTER_H

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

namespace im {
namespace storage {

/** 一条待导入的消息（字段与 Room MessageEntity 对齐 + FTS 拼音列）。 */
struct MigrationMessage {
    std::int64_t ownerId = 0;
    std::string msgId;
    std::int64_t conversationId = 0;
    std::int64_t peerId = 0;
    std::int64_t conversationSeq = 0;
    std::int64_t serverTime = 0;
    std::int64_t localOrder = 0;
    int fromMe = 0;
    int type = 0; // 0=TEXT 1=IMAGE 2=FILE
    std::string content;
    int status = 0;

    // 媒体/文件元数据（只搬字符串与数值，不动文件）
    std::string mediaPath;
    int imgW = 0;
    int imgH = 0;
    std::string fileId;
    std::string fileName;
    std::int64_t fileSize = 0;
    std::string contentType;
    std::string sha256;
    std::string thumbnailFileId;
    std::string thumbnailPath;
    std::int64_t thumbnailSize = 0;
    std::string thumbnailSha256;
    int thumbnailW = 0;
    int thumbnailH = 0;
    std::string largeThumbnailFileId;
    std::string largeThumbnailPath;
    std::int64_t largeThumbnailSize = 0;
    std::string largeThumbnailSha256;
    int largeThumbnailW = 0;
    int largeThumbnailH = 0;
    std::string localPath;
    std::int64_t transferred = 0;

    // FTS 拼音列（Room messages_fts 的 pinyin/initials；不得丢失，否则检索倒退）
    std::string pinyin;
    std::string initials;
};

/** 对账摘要。 */
struct MigrationSummary {
    std::int64_t messageCount = 0;
    std::int64_t conversationCount = 0;
    std::int64_t maxSeq = 0;
    std::int64_t minSeq = 0;
    std::int64_t ftsCount = 0;

    bool operator==(const MigrationSummary& o) const
    {
        return messageCount == o.messageCount && conversationCount == o.conversationCount &&
               maxSeq == o.maxSeq && minSeq == o.minSeq && ftsCount == o.ftsCount;
    }
};

std::string toString(const MigrationSummary& s);

/**
 * 一条待导入的会话元数据（字段与 Room ConversationEntity 对齐）。
 * 会话**独立于消息流**分页导出，保留 unread / lastMsg / lastTs 与空会话（无消息的会话）。
 */
struct MigrationConversation {
    std::int64_t conversationId = 0;
    std::int64_t ownerId = 0;
    std::int64_t peerId = 0;
    std::string lastMsg;
    std::int64_t lastTs = 0;
    std::int64_t unread = 0;
};

/**
 * 迁移开关三态（§24.7「disabled → shadow_import → verified」）。
 *
 * - Disabled：迁移关闭（默认）。不写入影子库，保护生产 Room 与媒体文件。
 * - ShadowImport：影子导入中。允许幂等写入消息/FTS/会话摘要/checkpoint。
 * - Verified：对账通过、迁移完成（等价于 `migration_completed=1`）。
 *
 * 状态持久化到 `migration_meta` 的 `state` 键；回滚（resetForReimport）只把状态退回
 * ShadowImport 并清除完成标记/checkpoint，**不删除**生产 Room 与媒体文件。
 */
enum class MigrationState {
    Disabled,
    ShadowImport,
    Verified,
};

const char* toString(MigrationState s);

class MigrationImporter {
public:
    /**
     * 导入一批消息（幂等）。调用方**必须**已经在事务中（或通过 DbCommandQueue 提交，
     * 由队列保证单事务）。
     * @return 成功 true；失败时 *err 给出原因（不含正文）
     */
    static bool importBatch(sqlite3* db, std::int64_t ownerId,
                            const std::vector<MigrationMessage>& batch, std::string* err = nullptr);

    /**
     * 导入一批会话元数据（幂等 upsert，保留 unread/lastMsg/lastTs）。
     * 应在消息流导入**之后**调用，作为会话摘要与未读数的权威来源。
     */
    static bool importConversations(sqlite3* db, std::int64_t ownerId,
                                    const std::vector<MigrationConversation>& convs,
                                    std::string* err = nullptr);

    /** 更新 checkpoint（最后已提交游标）；同事务内调用。 */
    static bool saveCheckpoint(sqlite3* db, std::int64_t ownerId, const std::string& checkpoint,
                               std::string* err = nullptr);

    static bool readCheckpoint(sqlite3* db, std::int64_t ownerId, std::string* checkpoint);

    /** 会话流独立 checkpoint（R03：消息流与会话流游标分离）。 */
    static bool saveConversationCheckpoint(sqlite3* db, std::int64_t ownerId,
                                           const std::string& checkpoint,
                                           std::string* err = nullptr);

    static bool readConversationCheckpoint(sqlite3* db, std::int64_t ownerId,
                                           std::string* checkpoint);

    /** 统计当前库的实际摘要（用于对账）。 */
    static bool computeSummary(sqlite3* db, std::int64_t ownerId, MigrationSummary* out,
                               std::string* err = nullptr);

    /**
     * 完成迁移：先对账，一致才原子写 migration_completed=1。
     * @param expected 导出侧统计出的期望摘要
     * @param actual   可选，输出实际摘要（便于报告差异）
     */
    static bool finish(sqlite3* db, std::int64_t ownerId, const MigrationSummary& expected,
                       MigrationSummary* actual = nullptr, std::string* err = nullptr);

    static bool isCompleted(sqlite3* db, std::int64_t ownerId, bool* completed);

    // ---------------- 迁移开关三态（§24.7） ----------------

    /** 写入迁移状态。 */
    static bool setState(sqlite3* db, std::int64_t ownerId, MigrationState state,
                         std::string* err = nullptr);

    /** 读取迁移状态；无记录时默认 Disabled。 */
    static bool getState(sqlite3* db, std::int64_t ownerId, MigrationState* out);

    /** 开始影子导入：Disabled → ShadowImport（幂等；已 Verified 时返回 false 拒绝重复迁移）。 */
    static bool beginImport(sqlite3* db, std::int64_t ownerId, std::string* err = nullptr);

    /** 回滚到 ShadowImport：清除完成标记与 checkpoint，允许重新导入（不删生产数据）。 */
    static bool resetForReimport(sqlite3* db, std::int64_t ownerId, std::string* err = nullptr);
};

} // namespace storage
} // namespace im

#endif // CLIENT_CORE_MIGRATION_IMPORTER_H
