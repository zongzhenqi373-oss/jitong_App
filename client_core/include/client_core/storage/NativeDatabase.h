// NativeDatabase：按账号打开的加密数据底座（P6 粘合层，T02 收尾）。
//
// 把既有组件组合成一个"可打开/关闭的账号库"：
//   CipherDatabase（加密打开） + SchemaManager（版本化迁移） +
//   DbCommandQueue（单 Writer 写） + ReadPool（只读并发）
//
// 约束（对应 24.2/24.4/24.6）：
//   - 路径只走 DatabasePaths（filesDir/native_db/account_<ownerId>.db），越界在创建文件前拒绝；
//   - key 只以"本次打开所需的 32 字节副本"传入，用完由调用方清零；本类不持久化、不回传；
//   - 只有 Writer 写；网络/JNI/UI 线程不得直接写库（只能通过 submit 提交命令）；
//   - 关闭顺序：停队列 → 关读池 → 关写连接；
//   - **两阶段关闭**：锁内转移所有权并置 Closing，锁外 join/等待 lease，锁内置 Closed；
//     生命周期锁内绝不 join 外部线程、绝不执行用户回调（防重入死锁）。

#ifndef CLIENT_CORE_NATIVE_DATABASE_H
#define CLIENT_CORE_NATIVE_DATABASE_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "client_core/storage/DbCommandQueue.h"
#include "client_core/storage/DbKeyBridge.h"
#include "client_core/storage/MigrationImporter.h"
#include "client_core/storage/ReadPool.h"

struct sqlite3;

namespace im {
namespace storage {

enum class DbStatus {
    Closed,  // 未打开 / 已完全关闭
    Opening, // 打开中（短暂态）
    Ready,   // 可读写
    Closing, // 关闭中（短暂态）
    Locked,  // 已锁定（无可用 key 或 key 错误）
};

const char* toString(DbStatus s);

/** 迁移状态快照（queryMigrationState 的只读结果，值对象）。 */
struct MigrationStateSnapshot {
    bool completed = false;
    MigrationState state = MigrationState::Disabled;
    std::string checkpoint;
    MigrationSummary summary;
};

/** 一次迁移批次的同步提交结果。 */
struct MigrationSubmitOutcome {
    bool ok = false;        // 命令已确定完成且成功
    bool timedOut = false;  // 等待超时，命令可能仍在 Writer 运行（TimedOutButMayCommit）
    CommandResult command = CommandResult::NotOpen;
    std::size_t imported = 0;
    std::string error;      // 失败原因（不含正文）
};

/** 数据库自检结果（值对象）。 */
struct SelfTestOutcome {
    bool ok = false;
    std::string cipher;
    int schemaVersion = -1;
    bool completed = false;
    MigrationSummary summary;
    std::string error;
};

class NativeDatabase {
public:
    NativeDatabase();
    ~NativeDatabase();

    NativeDatabase(const NativeDatabase&) = delete;
    NativeDatabase& operator=(const NativeDatabase&) = delete;

    /**
     * 打开（必要时创建）指定账号的影子库。
     * @param filesDir 平台 filesDir（由 Android 传入）
     * @param ownerId  账号 id（正整数）
     * @param key32    32 字节库密钥副本
     * @param err      失败原因（不含 key 与正文）
     */
    bool open(const std::string& filesDir, std::int64_t ownerId,
              const std::vector<unsigned char>& key32, std::string* err = nullptr);

    /**
     * 经平台密钥桥打开（**正式路径**）。
     * 桥返回 Unavailable（例如 Token-only 冷启动、还没有 passHash）时不做任何降级，
     * 状态保持 Locked 并返回 false；不得拿 Access/Refresh Token 派生 key。
     */
    bool openWithBridge(const std::string& filesDir, std::int64_t ownerId,
                        IPlatformKeyBridge& bridge, std::string* err = nullptr);

    /** 两阶段关闭：锁内移交所有权 → 锁外 join/等待 → 锁内置 Closed。幂等。 */
    void close();

    DbStatus status() const { return m_status.load(std::memory_order_acquire); }
    std::int64_t ownerId() const { return m_ownerId.load(std::memory_order_acquire); }
    const std::string& path() const { return m_path; }

    /** 提交一条写命令（由队列保证单事务、串行、背压与取消语义）。 */
    CommandResult submit(const DbCommandQueue::Request& req);

    /**
     * 同步执行一个写命令（在 Writer 线程串行）并等待完成。
     * @param timedOut 非 null 时接收"等待超时"标志（命令可能仍在运行，见 R09）
     */
    CommandResult submitSync(const DbCommandQueue::WriteFn& fn,
                             std::chrono::milliseconds timeout = std::chrono::seconds(60),
                             bool* timedOut = nullptr);

    /** 在只读连接上执行查询；结果必须由调用方复制为值对象。 */
    ReadResult withRead(const std::function<void(sqlite3*)>& fn,
                        std::chrono::milliseconds waitFor = std::chrono::milliseconds(5000));

    // ---------------- 迁移编排（写走队列、读走读池，不暴露写连接） ----------------

    /** 开始影子导入：Disabled → ShadowImport（已 Verified 拒绝）。 */
    bool beginMigration(std::int64_t ownerId, std::string* err = nullptr);

    /** 回滚到 ShadowImport：清完成标记与 checkpoint（不删生产数据）。 */
    bool resetMigration(std::int64_t ownerId, std::string* err = nullptr);

    /** 同步提交一个迁移批次（消息 + FTS + 会话摘要 + checkpoint 在一个事务内）。 */
    MigrationSubmitOutcome submitMigrationBatch(
        std::int64_t ownerId, const std::vector<MigrationMessage>& msgs,
        const std::string& checkpoint,
        std::chrono::milliseconds timeout = std::chrono::seconds(60));

    /** 同步提交一个会话元数据批次（unread/lastMsg/lastTs，权威覆盖，独立 checkpoint）。 */
    MigrationSubmitOutcome submitConversationBatch(
        std::int64_t ownerId, const std::vector<MigrationConversation>& convs,
        const std::string& checkpoint,
        std::chrono::milliseconds timeout = std::chrono::seconds(60));

    /** 完成迁移：在同一 Writer 事务内对账 + 写 completed + 置 Verified。 */
    bool finishMigration(std::int64_t ownerId, const MigrationSummary& expected,
                         MigrationSummary* actual = nullptr, std::string* err = nullptr);

    /** 查询迁移状态（只读，同一快照）。 */
    bool queryMigrationState(std::int64_t ownerId, MigrationStateSnapshot* out);

    /** 数据库自检（只读）。 */
    SelfTestOutcome runSelfTest(std::int64_t ownerId);

private:
    bool openInternal(const std::string& filesDir, std::int64_t ownerId,
                      const std::vector<unsigned char>& key32, std::string* err);
    void closeInternal();

    struct Impl;
    std::unique_ptr<Impl> m_impl;

    std::string m_path;
    std::atomic<DbStatus> m_status{DbStatus::Closed};
    std::atomic<std::int64_t> m_ownerId{0};

    // 保护 m_status 的状态转移与 m_impl 成员指针的替换（短临界区，绝不 join/回调）
    std::mutex m_mutex;
    // 串行化完整 open/close 生命周期；不能只保护最终指针交换。
    std::mutex m_lifecycleMutex;
};

} // namespace storage
} // namespace im

#endif // CLIENT_CORE_NATIVE_DATABASE_H
