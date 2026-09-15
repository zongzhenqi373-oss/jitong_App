// ReadPool 单元测试（P6-T04）。
//
// 覆盖 24.6：
//   - 多只读连接可并行；查询结果不泄漏 SQLite 句柄（只借出到内部回调）；
//   - 任何只读连接执行写 SQL 都被拒绝；
//   - 错误 key 打开失败；连接数量有界；
//   - 只读连接在 Writer 提交后能读到新数据（配合 WAL/同一文件）；
//   - **F01**：池满时绝不复用忙连接，同一 `sqlite3*` 最大并发持有数恒为 1。

#include "client_core/storage/CipherDatabase.h"
#include "client_core/storage/CipherParams.h"
#include "client_core/storage/ReadPool.h"
#include "client_core/storage/SchemaManager.h"

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace im::storage;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& name)
{
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << std::endl;
    if (!cond) ++g_failures;
}

const char* kPath = "/tmp/test_read_pool.db";

std::vector<unsigned char> testKey() { return std::vector<unsigned char>(32, 0x55); }

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
} // namespace

int main()
{
    std::cout << "=== test_read_pool ===" << std::endl;
    removeDb();

    // 先用写连接建库建表并写入一行
    {
        CipherDatabase cdb;
        check(cdb.open(kPath, testKey()) == CipherError::Ok, "打开写库");
        check(SchemaManager::migrateTo(cdb.nativeHandle(), 0) == SchemaError::Ok, "建 Schema");
        check(exec(cdb.nativeHandle(),
                   "INSERT INTO messages(owner_id,msg_id,peer_id,conversation_id,conversation_seq,"
                   "server_time,local_order,from_me,type,content,status) "
                   "VALUES(1,'rp-1',2,100,1,1700000000,1,1,0,'readpool',1)"),
              "写入一行");
        cdb.close();
    }

    // [1] 打开只读池
    ReadPool pool(kPath, testKey(), /*size=*/2);
    std::string msg;
    check(pool.open(&msg), "只读池打开成功 " + msg);
    check(pool.size() == 2, "连接数量为 2（有界）");
    check(pool.isOpen(), "isOpen 为真");

    // [2] 读到数据（结果复制为值对象）
    {
        std::string content;
        const ReadResult rr = pool.withRead([&](sqlite3* d) {
            sqlite3_stmt* s = nullptr;
            if (sqlite3_prepare_v2(d, "SELECT content FROM messages WHERE msg_id='rp-1'", -1, &s,
                                   nullptr) == SQLITE_OK && s) {
                if (sqlite3_step(s) == SQLITE_ROW) {
                    const unsigned char* t = sqlite3_column_text(s, 0);
                    if (t) content = reinterpret_cast<const char*>(t);
                }
            }
            if (s) sqlite3_finalize(s);
        });
        check(rr == ReadResult::Ok, "withRead 借到连接");
        check(content == "readpool", "读到正确内容（不泄漏句柄）");
    }

    // [3] 写 SQL 必须被拒绝
    {
        bool writeRejected = false;
        pool.withRead([&](sqlite3* d) {
            char* err = nullptr;
            const int rc = sqlite3_exec(d, "INSERT INTO messages(owner_id,msg_id,peer_id,"
                                           "conversation_id,conversation_seq) "
                                           "VALUES(1,'hack',2,100,999)",
                                        nullptr, nullptr, &err);
            writeRejected = (rc != SQLITE_OK);
            if (err) sqlite3_free(err);
        });
        check(writeRejected, "只读连接执行写 SQL 被拒绝");
    }

    // [4] F01：并发下同一连接不被复用（每连接最大并发持有数 == 1，且总并发有界 == size）
    {
        std::mutex cntMutex;
        std::unordered_map<sqlite3*, int> current;
        std::unordered_map<sqlite3*, int> perConnMax;
        std::atomic<int> maxConcurrent{0};
        std::atomic<int> nowConcurrent{0};

        std::vector<std::thread> ts;
        for (int i = 0; i < 8; ++i) {
            ts.emplace_back([&]() {
                pool.withRead([&](sqlite3* d) {
                    const int now = nowConcurrent.fetch_add(1) + 1;
                    int prev = maxConcurrent.load();
                    while (now > prev && !maxConcurrent.compare_exchange_weak(prev, now)) {}

                    int myNow = 0;
                    {
                        std::lock_guard<std::mutex> lk(cntMutex);
                        myNow = ++current[d];
                        perConnMax[d] = std::max(perConnMax[d], myNow);
                    }
                    // 模拟查询耗时，放大并发复用窗口
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    {
                        std::lock_guard<std::mutex> lk(cntMutex);
                        --current[d];
                    }
                    nowConcurrent.fetch_sub(1);
                });
            });
        }
        for (auto& t : ts) t.join();

        std::cout << "      最大并发读=" << maxConcurrent.load() << std::endl;
        check(maxConcurrent.load() <= 2, "并发有界（<= size=2）");
        bool allExclusive = true;
        for (const auto& kv : perConnMax) {
            std::cout << "      连接 " << kv.first << " 最大并发持有=" << kv.second << std::endl;
            if (kv.second > 1) allExclusive = false;
        }
        check(allExclusive, "F01：同一连接最大并发持有数恒为 1（无复用）");
    }

    // [5] 错误 key 打开失败（不删库）
    {
        ReadPool bad(kPath, std::vector<unsigned char>(32, 0x99), 1);
        std::string badMsg;
        check(!bad.open(&badMsg), "错误 key 打开只读池失败");
        check(!bad.isOpen(), "失败后 isOpen 为 false");
    }

    // [6] close 后 withRead 返回 Closing
    pool.close();
    {
        const ReadResult rr = pool.withRead([](sqlite3*) {});
        check(rr == ReadResult::Closing, "close 后 withRead 返回 Closing");
    }

    removeDb();
    if (g_failures == 0) {
        std::cout << "test_read_pool PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_read_pool FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
