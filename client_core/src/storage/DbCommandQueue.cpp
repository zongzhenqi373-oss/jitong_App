#include "client_core/storage/DbCommandQueue.h"

#include <sqlite3.h>

#include <exception>

namespace im {
namespace storage {

namespace {

// Completion 永远不在 Writer/队列锁内执行。这样回调可以安全地关闭数据库，
// 不会让 Writer join 自己；单线程 executor 也保持 invalidation/onDone 的提交顺序。
class CompletionExecutor {
public:
    CompletionExecutor() : thread_([this]() { run(); }) {}
    ~CompletionExecutor()
    {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            running_ = false;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    void post(std::function<void()> fn)
    {
        if (!fn) return;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            tasks_.push_back(std::move(fn));
        }
        cv_.notify_one();
    }

private:
    void run()
    {
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> lk(mutex_);
                cv_.wait(lk, [this]() { return !running_ || !tasks_.empty(); });
                if (!running_ && tasks_.empty()) return;
                fn = std::move(tasks_.front());
                tasks_.pop_front();
            }
            try { fn(); } catch (...) { /* 用户回调异常不得杀死 executor */ }
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    bool running_ = true;
    std::thread thread_;
};

CompletionExecutor& completions()
{
    static CompletionExecutor executor;
    return executor;
}

void complete(DbCommandQueue::DoneFn done, CommandResult result)
{
    if (done) completions().post([done = std::move(done), result]() { done(result); });
}

} // namespace

const char* toString(CommandResult r)
{
    switch (r) {
        case CommandResult::Ok:        return "Ok";
        case CommandResult::Cancelled: return "Cancelled";
        case CommandResult::QueueFull: return "QueueFull";
        case CommandResult::NotOpen:   return "NotOpen";
        case CommandResult::SqlFailed: return "SqlFailed";
    }
    return "?";
}

DbCommandQueue::DbCommandQueue(sqlite3* writeConn, std::size_t capacity, InvalidationSink sink)
    : m_db(writeConn), m_capacity(capacity), m_sink(std::move(sink))
{
    m_thread = std::thread(&DbCommandQueue::writerLoop, this);
}

DbCommandQueue::~DbCommandQueue() { close(); }

CommandResult DbCommandQueue::submit(const Request& req)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_running.load()) return CommandResult::NotOpen;
        if (m_queue.size() >= m_capacity) return CommandResult::QueueFull;
        m_queue.push_back(req);
        m_pending.fetch_add(1);
    }
    m_cv.notify_one();
    return CommandResult::Ok;
}

void DbCommandQueue::close()
{
    std::deque<Request> cancelled;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_running.exchange(false)) {
            cancelled.swap(m_queue);
            if (!cancelled.empty()) m_pending.fetch_sub(cancelled.size());
            for (auto& req : cancelled) complete(std::move(req.onDone), CommandResult::Cancelled);
            return;
        }
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();

    // 线程已退出：仍未执行的命令统一判为取消，waiter 不会悬挂
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        cancelled.swap(m_queue);
        if (!cancelled.empty()) m_pending.fetch_sub(cancelled.size());
    }
    for (auto& req : cancelled) complete(std::move(req.onDone), CommandResult::Cancelled);
}

void DbCommandQueue::writerLoop()
{
    for (;;) {
        Request req;
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait(lk, [this]() { return !m_queue.empty() || !m_running.load(); });
            if (m_queue.empty()) {
                if (!m_running.load()) return; // 关闭且队列已空 → 退出
                continue;
            }
            req = std::move(m_queue.front());
            m_queue.pop_front();
        }

        // ---- 取消只在事务开始前检查；已开始的事务不中途取消 ----
        if (req.cancel && req.cancel->load()) {
            m_pending.fetch_sub(1);
            complete(std::move(req.onDone), CommandResult::Cancelled);
            continue;
        }

        if (!req.fn) {
            m_pending.fetch_sub(1);
            complete(std::move(req.onDone), CommandResult::SqlFailed);
            continue;
        }

        // ---- 一条业务原子操作 = 一个事务 ----
        char* err = nullptr;
        CommandResult result = CommandResult::Ok;

        if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, &err) != SQLITE_OK) {
            if (err) sqlite3_free(err);
            result = CommandResult::SqlFailed;
        } else {
            bool ok = false;
            try { ok = req.fn(m_db); } catch (...) { ok = false; }
            if (ok) {
                if (sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, &err) != SQLITE_OK) {
                    if (err) sqlite3_free(err);
                    sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
                    result = CommandResult::SqlFailed;
                }
            } else {
                sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
                result = CommandResult::SqlFailed;
            }
        }

        // ---- invalidation 只在 commit 成功后发送一次（合并）----
        InvalidationSink sink;
        std::vector<Invalidation> invalidations;
        if (result == CommandResult::Ok && m_sink) {
            Invalidation inv;
            inv.table = req.table;
            inv.conversationId = req.conversationId;
            inv.version = ++m_version;
            invalidations.push_back(std::move(inv));
            sink = m_sink;
        }

        m_pending.fetch_sub(1);
        if (sink) {
            completions().post([sink = std::move(sink), inv = std::move(invalidations)]() {
                sink(inv);
            });
        }
        complete(std::move(req.onDone), result);
    }
}

} // namespace storage
} // namespace im
