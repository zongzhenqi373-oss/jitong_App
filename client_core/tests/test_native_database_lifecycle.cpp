// NativeDatabase 生命周期门禁测试（阶段 B：F03/F09）。
//
// 覆盖：
//   - 打开/关闭/重开的状态机转移；
//   - F09：ReadPool close 与 active read 并发时，close 等待 lease 归还，不关闭使用中的连接；
//   - F03：同步等待超时后，迟到的 completion 回调只写堆上 state，不触碰已销毁的栈变量。

#include "client_core/storage/CipherDatabase.h"
#include "client_core/storage/CipherParams.h"
#include "client_core/storage/DbCommandQueue.h"
#include "client_core/storage/NativeDatabase.h"
#include "client_core/storage/ReadPool.h"
#include "client_core/storage/SchemaManager.h"

#include <sqlite3.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace im::storage;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& name)
{
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << std::endl;
    if (!cond) ++g_failures;
}

std::vector<unsigned char> testKey() { return std::vector<unsigned char>(32, 0x55); }

const char* kDir = "/tmp/test_ndb_lifecycle";
const char* kPoolPath = "/tmp/test_ndb_lifecycle/pool_test.db";
} // namespace

int main()
{
    std::cout << "=== test_native_database_lifecycle ===" << std::endl;
    ::mkdir(kDir, 0700); // 确保目录存在（首次运行）

    // [1] 打开/关闭/重开状态机
    {
        NativeDatabase db;
        std::string err;
        check(db.open(kDir, 1, testKey(), &err), "打开 " + err);
        check(db.status() == DbStatus::Ready, "状态 Ready");
        db.close();
        check(db.status() == DbStatus::Closed, "状态 Closed");
        check(db.open(kDir, 1, testKey(), &err), "重开 " + err);
        check(db.status() == DbStatus::Ready, "重开后 Ready");
        db.close();
    }

    // [1b] 迁移编排：beginMigration → queryMigrationState（复现 androidTest 失败）
    {
        NativeDatabase db;
        std::string err;
        check(db.open(kDir, 1, testKey(), &err), "打开 " + err);
        check(db.beginMigration(1, &err), "beginMigration " + err);
        MigrationStateSnapshot snap;
        const bool q = db.queryMigrationState(1, &snap);
        std::cout << "      queryMigrationState=" << q << " state=" << toString(snap.state)
                  << " completed=" << snap.completed
                  << " msgs=" << snap.summary.messageCount << std::endl;
        check(q, "queryMigrationState 返回 true");
        SelfTestOutcome st = db.runSelfTest(1);
        std::cout << "      selfTest ok=" << st.ok << " err=" << st.error << std::endl;
        db.close();
    }

    // [1c] 并发 open/close 由生命周期门串行化，最终状态与资源一致
    {
        NativeDatabase db;
        std::atomic<int> opened{0};
        std::thread opener([&]() {
            for (int i = 0; i < 10; ++i) {
                std::string err;
                if (db.open(kDir, 1, testKey(), &err)) ++opened;
            }
        });
        std::thread closer([&]() {
            for (int i = 0; i < 10; ++i) db.close();
        });
        opener.join();
        closer.join();
        db.close();
        check(opened > 0, "并发 open/close 至少一次打开成功");
        check(db.status() == DbStatus::Closed, "并发 open/close 后资源与 Closed 一致");
    }

    // [2] F09：ReadPool close 与 active lease 并发（close 等待 lease 归还）
    {
        // 先建一个只读池可打开的库
        {
            CipherDatabase cdb;
            check(cdb.open(kPoolPath, testKey()) == CipherError::Ok, "建池用库");
            check(SchemaManager::migrateTo(cdb.nativeHandle(), 0) == SchemaError::Ok, "建 Schema");
            cdb.close();
        }

        ReadPool pool(kPoolPath, testKey(), 2);
        std::string msg;
        check(pool.open(&msg), "池打开 " + msg);

        std::atomic<bool> entered{false};
        std::atomic<bool> readDone{false};
        std::thread reader([&]() {
            pool.withRead([&](sqlite3*) {
                entered = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(200)); // 慢查询
                readDone = true;
            });
        });

        // 等 reader 进入回调（active lease 已建立），带超时保护避免死循环
        int waited = 0;
        while (!entered && waited < 2000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ++waited;
        }
        check(entered, "reader 进入回调（2 秒内）");

        // close 应阻塞，直到 active lease 归还（不关闭使用中的连接）
        pool.close();
        check(readDone, "reader 已完成（close 等待 lease 归还）");
        check(pool.withRead([](sqlite3*) {}) == ReadResult::Closing, "close 后 withRead 返回 Closing");
        reader.join();
    }

    // [3] F03：堆上 completion state + 超时后迟到回调无 UAF
    {
        CipherDatabase cdb;
        check(cdb.open(std::string(kDir) + "/queue_test.db", testKey()) == CipherError::Ok, "建库");
        check(SchemaManager::migrateTo(cdb.nativeHandle(), 0) == SchemaError::Ok, "建 Schema");
        sqlite3* raw = cdb.nativeHandle();

        DbCommandQueue queue(raw, 1024, nullptr);

        // 慢命令占住 writer
        std::atomic<bool> slowDone{false};
        {
            DbCommandQueue::Request slowReq;
            slowReq.fn = [&](sqlite3*) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                slowDone = true;
                return true;
            };
            queue.submit(slowReq);
        }

        // 第二个命令：completion state 放堆上，回调只捕获 shared_ptr
        struct State {
            CommandResult result = CommandResult::NotOpen;
            std::mutex mutex;
            std::condition_variable cv;
            bool done = false;
        };
        auto state = std::make_shared<State>();

        DbCommandQueue::Request req2;
        req2.fn = [](sqlite3*) { return true; };
        req2.onDone = [state](CommandResult r) {
            std::lock_guard<std::mutex> lk(state->mutex);
            state->result = r;
            state->done = true;
            state->cv.notify_all();
        };
        check(queue.submit(req2) == CommandResult::Ok, "第二个命令入队");

        bool timedOut = false;
        {
            std::unique_lock<std::mutex> lk(state->mutex);
            const bool done =
                state->cv.wait_for(lk, std::chrono::milliseconds(50), [&]() { return state->done; });
            timedOut = !done;
        }
        check(timedOut, "50ms 后超时（慢命令占住 writer）");

        // 等慢命令完成后，第二个命令的迟到回调写 state；验证无 UAF、结果正确
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        {
            std::lock_guard<std::mutex> lk(state->mutex);
            check(state->done, "迟到回调完成，state 已更新（无 UAF）");
            check(state->result == CommandResult::Ok, "迟到回调结果 Ok");
        }
        check(slowDone, "慢命令完成");

        queue.close();
        cdb.close();
    }

    std::remove((std::string(kDir) + "/native_db/account_1.db").c_str());
    std::remove((std::string(kDir) + "/native_db/account_1.db-wal").c_str());
    std::remove((std::string(kDir) + "/native_db/account_1.db-shm").c_str());
    std::remove((std::string(kDir) + "/native_db/account_1.db.lock").c_str());
    std::remove((std::string(kDir) + "/queue_test.db").c_str());
    std::remove((std::string(kDir) + "/queue_test.db-wal").c_str());
    std::remove((std::string(kDir) + "/queue_test.db-shm").c_str());
    std::remove(kPoolPath);
    std::remove((std::string(kPoolPath) + "-wal").c_str());
    std::remove((std::string(kPoolPath) + "-shm").c_str());

    if (g_failures == 0) {
        std::cout << "test_native_database_lifecycle PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_native_database_lifecycle FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
