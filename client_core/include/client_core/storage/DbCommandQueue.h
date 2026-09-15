// 写命令队列与单 Writer（P6-T04）。
//
// 核心不变式：
//   1. **只有一个 Writer 连接 / 一个 Writer 线程**写库；网络、JNI、UI 线程都不执行写 SQL。
//   2. **一条业务原子操作只开启一个事务**：BEGIN IMMEDIATE → 执行命令 → COMMIT/ROLLBACK。
//      不允许多条命令各开各的事务，也不允许把消息/FTS/会话摘要/checkpoint 拆成多次提交。
//   3. **背压**：队列有界，满载返回 QueueFull，不无限增长内存。
//   4. **取消**：只在事务**开始前**检查取消标记；已开始的事务不中途取消，按操作语义整体
//      提交或回滚（避免半事务）。
//   5. **invalidation 只在 commit 成功后发送一次**（合并），rollback 不发送。
//   6. close/destroy：停止接收 → 完成或取消队列 → join；每个任务都会收到**确定结果**。

#ifndef CLIENT_CORE_DB_COMMAND_QUEUE_H
#define CLIENT_CORE_DB_COMMAND_QUEUE_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct sqlite3;

namespace im {
namespace storage {

enum class CommandResult {
    Ok,         // 已提交
    Cancelled,  // 被取消（未执行）
    QueueFull,  // 队列满载，未入队
    NotOpen,    // 队列已关闭，未入队
    SqlFailed,  // SQL 失败，已回滚
};

const char* toString(CommandResult r);

/** 一次数据变更后的失效通知：只带表/会话/版本，业务侧据此决定是否重查。 */
struct Invalidation {
    std::string table;
    std::int64_t conversationId = 0;
    std::uint64_t version = 0;
};

class DbCommandQueue {
public:
    using CancelToken = std::shared_ptr<std::atomic<bool>>;
    /** 返回 false 表示该命令失败 → 整体回滚。 */
    using WriteFn = std::function<bool(sqlite3*)>;
    using DoneFn = std::function<void(CommandResult)>;
    using InvalidationSink = std::function<void(const std::vector<Invalidation>&)>;

    struct Request {
        WriteFn fn;
        CancelToken cancel;
        std::string table;           // 失效表名（可空）
        std::int64_t conversationId = 0;
        DoneFn onDone;
    };

    /**
     * @param writeConn 唯一的写连接（由 CipherDatabase 打开，生命周期长于本对象）
     * @param capacity  队列容量（背压阈值）
     * @param sink      commit 成功后调用一次，传入合并后的 invalidation
     */
    DbCommandQueue(sqlite3* writeConn, std::size_t capacity, InvalidationSink sink);
    ~DbCommandQueue();

    DbCommandQueue(const DbCommandQueue&) = delete;
    DbCommandQueue& operator=(const DbCommandQueue&) = delete;

    CommandResult submit(const Request& req);

    /** 停止接收 → 完成/取消在途命令 → join 线程。 */
    void close();

    std::size_t pending() const { return m_pending.load(); }
    bool running() const { return m_running.load(); }
    std::size_t capacity() const { return m_capacity; }

    static CancelToken makeCancelToken()
    {
        return std::make_shared<std::atomic<bool>>(false);
    }

private:
    void writerLoop();

    sqlite3* m_db = nullptr;
    std::size_t m_capacity = 1024;
    InvalidationSink m_sink;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Request> m_queue;
    std::atomic<bool> m_running{true};
    std::atomic<std::size_t> m_pending{0};
    std::uint64_t m_version = 0;
    std::thread m_thread;
};

} // namespace storage
} // namespace im

#endif // CLIENT_CORE_DB_COMMAND_QUEUE_H
