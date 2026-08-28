#include "DbWriteQueue.h"

#include <sqlite3.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

const char* DB_PATH = "/tmp/im_server_db_write_queue_test.db";

void removeTestDatabase()
{
    std::remove(DB_PATH);
    std::remove((std::string(DB_PATH) + "-wal").c_str());
    std::remove((std::string(DB_PATH) + "-shm").c_str());
}

} // namespace

int main()
{
    removeTestDatabase();

    {
    imsrv::DbWriteQueue queue;

    // 1. 未启动时必须立即失败，不能卡死在 future.get()。
    bool stoppedRejected = false;
    try {
        queue.submit([](sqlite3*) { return 1; });
    } catch (const std::runtime_error&) {
        stoppedRejected = true;
    }
    assert(stoppedRejected);

    // 2. open 可重复调用，且任务返回值、void 返回都能正常传递。
    assert(queue.open(DB_PATH));
    assert(queue.open(DB_PATH));
    assert(queue.submit([](sqlite3*) { return 42; }) == 42);

    bool voidTaskRan = false;
    queue.submit([&voidTaskRan](sqlite3*) { voidTaskRan = true; });
    assert(voidTaskRan);

    // 3. 任务异常必须传播回提交线程，写线程不能因此退出。
    bool exceptionPropagated = false;
    try {
        queue.submit([](sqlite3*) -> int {
            throw std::runtime_error("expected task failure");
        });
    } catch (const std::runtime_error& error) {
        exceptionPropagated = std::string(error.what()) == "expected task failure";
    }
    assert(exceptionPropagated);
    assert(queue.submit([](sqlite3*) { return 7; }) == 7);

    // 4. 支持移动捕获，验证任务不再引用 submit() 栈上的临时 callable。
    auto ownedValue = std::make_unique<int>(99);
    const int movedResult = queue.submit(
        [value = std::move(ownedValue)](sqlite3*) { return *value; }
    );
    assert(movedResult == 99);
    assert(!ownedValue);

    // 5. 写线程内部嵌套 submit 必须立即拒绝，否则会等待自己形成死锁。
    const bool nestedRejected = queue.submit([&queue](sqlite3*) {
        try {
            queue.submit([](sqlite3*) { return 0; });
        } catch (const std::logic_error&) {
            return true;
        }
        return false;
    });
    assert(nestedRejected);

    // 6. 真正执行 SQLite 写入，确认传入的是有效且稳定的专属连接。
    assert(queue.submit([](sqlite3* db) {
        return sqlite3_exec(
            db,
            "CREATE TABLE queue_test(id INTEGER PRIMARY KEY, value INTEGER NOT NULL);",
            nullptr, nullptr, nullptr
        ) == SQLITE_OK;
    }));

    // 7. 多线程并发提交时，任务只能在同一个写线程中逐个执行。
    constexpr int THREAD_COUNT = 8;
    constexpr int TASKS_PER_THREAD = 25;
    std::atomic<int> active{0};
    std::atomic<int> maxActive{0};
    std::atomic<int> completed{0};
    std::mutex threadIdsMutex;
    std::set<std::thread::id> workerThreadIds;
    std::vector<std::thread> submitters;

    for (int threadIndex = 0; threadIndex < THREAD_COUNT; ++threadIndex) {
        submitters.emplace_back([&, threadIndex] {
            for (int taskIndex = 0; taskIndex < TASKS_PER_THREAD; ++taskIndex) {
                const int value = threadIndex * TASKS_PER_THREAD + taskIndex;
                const bool inserted = queue.submit([&, value](sqlite3* db) {
                    const int nowActive = active.fetch_add(1) + 1;
                    int observed = maxActive.load();
                    while (observed < nowActive &&
                           !maxActive.compare_exchange_weak(observed, nowActive)) {
                    }

                    {
                        std::lock_guard<std::mutex> lock(threadIdsMutex);
                        workerThreadIds.insert(std::this_thread::get_id());
                    }

                    std::this_thread::sleep_for(std::chrono::microseconds(100));

                    sqlite3_stmt* statement = nullptr;
                    const bool prepared = sqlite3_prepare_v2(
                        db,
                        "INSERT INTO queue_test(value) VALUES(?);",
                        -1,
                        &statement,
                        nullptr
                    ) == SQLITE_OK;
                    if (!prepared) {
                        active.fetch_sub(1);
                        return false;
                    }
                    sqlite3_bind_int(statement, 1, value);
                    const bool ok = sqlite3_step(statement) == SQLITE_DONE;
                    sqlite3_finalize(statement);
                    active.fetch_sub(1);
                    return ok;
                });
                assert(inserted);
                completed.fetch_add(1);
            }
        });
    }

    for (auto& submitter : submitters) submitter.join();

    const int expected = THREAD_COUNT * TASKS_PER_THREAD;
    assert(completed.load() == expected);
    assert(maxActive.load() == 1);
    assert(workerThreadIds.size() == 1);

    const int rowCount = queue.submit([](sqlite3* db) {
        sqlite3_stmt* statement = nullptr;
        assert(sqlite3_prepare_v2(
            db, "SELECT COUNT(*) FROM queue_test;", -1, &statement, nullptr
        ) == SQLITE_OK);
        assert(sqlite3_step(statement) == SQLITE_ROW);
        const int count = sqlite3_column_int(statement, 0);
        sqlite3_finalize(statement);
        return count;
    });
    assert(rowCount == expected);

    std::cout
        << "test_db_write_queue PASSED: tasks=" << expected
        << " maxActive=" << maxActive.load()
        << " workerThreads=" << workerThreadIds.size()
        << std::endl;

    // queue 析构时线程会被停止并 join，数据库连接随后关闭。
    }
    removeTestDatabase();
    return 0;
}
