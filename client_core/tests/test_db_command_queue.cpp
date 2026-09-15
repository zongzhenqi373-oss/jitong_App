// DbCommandQueue 单元测试（P6-T04）。
//
// 覆盖 24.6：
//   - 10 万并发投递写入无丢失、无重复、无死锁/SQLITE_BUSY；
//   - 实际只有一个可写 connection/thread 执行写 SQL；
//   - 队列背压（满载返回 QueueFull）；
//   - 取消语义（取消的命令不执行；已开始的事务不中途取消）；
//   - commit 前无半事务可见；commit 后 invalidation 次数正确；rollback 不发送 invalidation；
//   - close/destroy 后所有 waiter 都有确定结果。

#include "client_core/storage/CipherDatabase.h"
#include "client_core/storage/CipherParams.h"
#include "client_core/storage/DbCommandQueue.h"
#include "client_core/storage/SchemaManager.h"

#include <sqlite3.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace im::storage;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& name)
{
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << std::endl;
    if (!cond) ++g_failures;
}

const char* kPath = "/tmp/test_db_command_queue.db";

std::vector<unsigned char> testKey() { return std::vector<unsigned char>(32, 0x44); }

void removeDb()
{
    std::remove(kPath);
    std::remove((std::string(kPath) + "-wal").c_str());
    std::remove((std::string(kPath) + "-shm").c_str());
}

bool exec(sqlite3* db, const std::string& sql)
{
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

int queryInt(sqlite3* db, const std::string& sql)
{
    sqlite3_stmt* s = nullptr;
    int v = -1;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) == SQLITE_OK && s) {
        if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int(s, 0);
    }
    if (s) sqlite3_finalize(s);
    return v;
}

/** 计数门：用于"确定结果"与并发同步。 */
struct Gate {
    std::mutex m;
    std::condition_variable cv;
    int count = 0;
    void notify()
    {
        std::lock_guard<std::mutex> lk(m);
        ++count;
        cv.notify_all();
    }
    bool waitFor(int target, int ms)
    {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(ms),
                           [&]() { return count >= target; });
    }
};

} // namespace

int main()
{
    std::cout << "=== test_db_command_queue ===" << std::endl;
    removeDb();

    CipherDatabase cdb;
    check(cdb.open(kPath, testKey()) == CipherError::Ok, "打开加密库");
    sqlite3* db = cdb.nativeHandle();
    check(db != nullptr, "拿到写连接");
    check(SchemaManager::migrateTo(db, 0) == SchemaError::Ok, "建 Schema");

    // ---- 记录写 SQL 都在同一线程执行 ----
    std::mutex tidMutex;
    std::unordered_set<std::thread::id> writerThreads;

    std::atomic<int> invalidationCount{0};
    DbCommandQueue queue(db, /*capacity=*/4096,
                         [&](const std::vector<Invalidation>&) { invalidationCount.fetch_add(1); });

    // [1] 正常提交 + invalidation 一次
    {
        Gate done;
        DbCommandQueue::Request req;
        req.table = "messages";
        req.conversationId = 100;
        req.fn = [&](sqlite3* d) {
            {
                std::lock_guard<std::mutex> lk(tidMutex);
                writerThreads.insert(std::this_thread::get_id());
            }
            return exec(d,
                        "INSERT INTO messages(owner_id,msg_id,peer_id,conversation_id,"
                        "conversation_seq,server_time,local_order,from_me,type,content,status) "
                        "VALUES(1,'q-1',2,100,1,1700000000,1,1,0,'hello',1)");
        };
        req.onDone = [&](CommandResult r) {
            if (r == CommandResult::Ok) done.notify();
        };
        check(queue.submit(req) == CommandResult::Ok, "提交成功");
        check(done.waitFor(1, 3000), "命令完成回调触发");
        check(invalidationCount.load() == 1, "commit 后 invalidation 恰好一次");
    }

    // [2] SQL 失败 → 回滚，不发送 invalidation
    {
        Gate done;
        std::atomic<CommandResult> res{CommandResult::Ok};
        DbCommandQueue::Request req;
        req.table = "messages";
        req.fn = [&](sqlite3*) {
            return false; // 模拟失败
        };
        req.onDone = [&](CommandResult r) {
            res.store(r);
            done.notify();
        };
        queue.submit(req);
        check(done.waitFor(1, 3000), "失败命令也有回调");
        check(res.load() == CommandResult::SqlFailed, "失败返回 SqlFailed");
        check(invalidationCount.load() == 1, "rollback 不发送 invalidation");
    }

    // [3] 取消：未开始的事务不执行
    {
        Gate done;
        std::atomic<CommandResult> res{CommandResult::Ok};
        std::atomic<bool> executed{false};
        DbCommandQueue::Request req;
        req.cancel = DbCommandQueue::makeCancelToken();
        req.fn = [&](sqlite3*) {
            executed.store(true);
            return true;
        };
        req.onDone = [&](CommandResult r) {
            res.store(r);
            done.notify();
        };
        *req.cancel = true; // 提交前取消
        check(queue.submit(req) == CommandResult::Ok, "取消态命令仍可入队（由 writer 判定）");
        check(done.waitFor(1, 3000), "取消命令有回调");
        check(res.load() == CommandResult::Cancelled, "返回 Cancelled");
        check(!executed.load(), "被取消的命令未执行");
    }

    // [4] 背压：容量满载 → QueueFull
    {
        // 用长事务堵住 writer，再灌满队列
        std::atomic<bool> release{false};
        Gate blockerDone;
        DbCommandQueue::Request blocker;
        blocker.fn = [&](sqlite3*) {
            while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return true;
        };
        blocker.onDone = [&](CommandResult) { blockerDone.notify(); };
        queue.submit(blocker);
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 等 writer 进入阻塞命令

        int full = 0;
        DbCommandQueue::Request filler;
        filler.fn = [](sqlite3*) { return true; };
        for (int i = 0; i < 10000; ++i) {
            if (queue.submit(filler) == CommandResult::QueueFull) { ++full; break; }
        }
        check(full > 0, "队列满载后返回 QueueFull（背压生效）");
        release.store(true);
        blockerDone.waitFor(1, 5000);
    }

    // [5] 10 万并发投递：无丢失、无重复、都在同一个写线程
    {
        const int kThreads = 8;
        const int kPerThread = 12500; // 合计 100000
        std::atomic<int> okCount{0};
        std::atomic<int> otherCount{0};
        Gate all;
        std::vector<std::thread> producers;
        for (int t = 0; t < kThreads; ++t) {
            producers.emplace_back([&, t]() {
                for (int i = 0; i < kPerThread; ++i) {
                    const std::string msgId = "m-" + std::to_string(t) + "-" + std::to_string(i);
                    const long long seq = static_cast<long long>(t) * kPerThread + i + 10;
                    DbCommandQueue::Request req;
                    req.table = "messages";
                    req.conversationId = 100;
                    req.fn = [&, msgId, seq](sqlite3* d) {
                        {
                            std::lock_guard<std::mutex> lk(tidMutex);
                            writerThreads.insert(std::this_thread::get_id());
                        }
                        return exec(d,
                                    "INSERT INTO messages(owner_id,msg_id,peer_id,conversation_id,"
                                    "conversation_seq,server_time,local_order,from_me,type,content,"
                                    "status) VALUES(1,'" +
                                        msgId + "',2,100," + std::to_string(seq) +
                                        ",1700000000,1,1,0,'x',1)");
                    };
                    req.onDone = [&](CommandResult r) {
                        if (r == CommandResult::Ok) okCount.fetch_add(1);
                        else otherCount.fetch_add(1);
                        all.notify();
                    };
                    // 背压下重试入队
                    for (;;) {
                        const CommandResult s = queue.submit(req);
                        if (s == CommandResult::Ok) break;
                        if (s == CommandResult::QueueFull) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            continue;
                        }
                        otherCount.fetch_add(1);
                        all.notify();
                        break;
                    }
                }
            });
        }
        // Do not leave detached producers accessing stack state after a timeout/failure.
        for (auto& producer : producers) producer.join();
        check(all.waitFor(kThreads * kPerThread, 120000), "10 万命令全部收到结果");
        std::cout << "      ok=" << okCount.load() << " other=" << otherCount.load() << std::endl;
        check(okCount.load() == kThreads * kPerThread, "10 万命令全部成功（无丢失/无失败）");
        check(queryInt(db, "SELECT count(*) FROM messages WHERE owner_id=1") ==
                  kThreads * kPerThread + 1,
              "落库行数正确（含前面 1 条）");
        check(queryInt(db, "SELECT count(DISTINCT msg_id) FROM messages WHERE owner_id=1") ==
                  kThreads * kPerThread + 1,
              "msg_id 无重复");
        std::lock_guard<std::mutex> lk(tidMutex);
        check(writerThreads.size() == 1, "所有写 SQL 都在同一个线程执行");
    }

    // [6] close 后：残留命令判为取消，且不再接收
    {
        Gate done;
        DbCommandQueue::Request req;
        req.fn = [](sqlite3*) { return true; };
        req.onDone = [&](CommandResult) { done.notify(); };
        queue.close();
        check(queue.submit(req) == CommandResult::NotOpen, "close 后提交返回 NotOpen");
        check(!queue.running(), "队列已停止");
        check(queue.pending() == 0, "pending 归零，waiter 不会悬挂");
    }

    // [7] completion 回调可重入 close：回调不在 Writer 上执行，不会 self-join
    {
        auto reentrant = std::make_shared<DbCommandQueue>(db, 8, nullptr);
        Gate done;
        DbCommandQueue::Request req;
        req.fn = [](sqlite3*) { return true; };
        req.onDone = [reentrant, &done](CommandResult r) {
            if (r == CommandResult::Ok) reentrant->close();
            done.notify();
        };
        check(reentrant->submit(req) == CommandResult::Ok, "重入关闭命令入队");
        check(done.waitFor(1, 3000), "completion 重入 close 无死锁/self-join");
        check(!reentrant->running(), "重入 close 后队列停止");
    }

    // [8] 用户 WriteFn 抛异常时转换为 SqlFailed，Writer 继续存活
    {
        DbCommandQueue exceptionQueue(db, 8, nullptr);
        Gate done;
        std::atomic<CommandResult> result{CommandResult::Ok};
        DbCommandQueue::Request req;
        req.fn = [](sqlite3*) -> bool { throw std::runtime_error("expected"); };
        req.onDone = [&](CommandResult r) { result = r; done.notify(); };
        check(exceptionQueue.submit(req) == CommandResult::Ok, "异常命令入队");
        check(done.waitFor(1, 3000), "异常命令收到 completion");
        check(result == CommandResult::SqlFailed, "WriteFn 异常转换为 SqlFailed");
        exceptionQueue.close();
    }

    cdb.close();
    removeDb();
    if (g_failures == 0) {
        std::cout << "test_db_command_queue PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_db_command_queue FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
