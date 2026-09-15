// OS 级强杀恢复单测（§24.7「一批提交后强杀进程，重启可恢复」）。
//
// 用 fork 子进程 + `_exit` 精确模拟「进程被强杀」：子进程打开加密库、写入一条消息与
// checkpoint（COMMIT 到 WAL）后**不调用任何 close/析构**直接 `_exit(0)`，等价于进程被
// 信号杀死时「文件描述符由内核回收、WAL 未 checkpoint、无任何清理路径执行」。
// 父进程 waitpid 后重开同一库文件，验证 SQLite WAL 崩溃恢复能读到强杀前的 checkpoint，
// 并可继续续跑。
//
// 与「优雅关闭后重开」（test_room_import / test_migration_switch）相比，本测试专门覆盖
// 非优雅退出路径，是 §24.7 强杀恢复的核心语义。

#include "client_core/storage/CipherDatabase.h"
#include "client_core/storage/CipherParams.h"
#include "client_core/storage/MigrationImporter.h"
#include "client_core/storage/SchemaManager.h"

#include <sqlite3.h>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using namespace im::storage;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& name)
{
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << std::endl;
    if (!cond) ++g_failures;
}

const char* kPath = "/tmp/test_kill_recovery.db";
const std::int64_t kOwner = 11;
const std::string kCheckpoint = "42";

std::vector<unsigned char> testKey() { return std::vector<unsigned char>(32, 0x33); }

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

MigrationMessage makeMsg(int i)
{
    MigrationMessage m;
    m.ownerId = kOwner;
    m.msgId = "m-" + std::to_string(i);
    m.conversationId = 100;
    m.peerId = 2;
    m.conversationSeq = i + 1;
    m.serverTime = 1700000000 + i;
    m.localOrder = i;
    m.type = 0;
    m.content = "kill-test" + std::to_string(i);
    m.status = 1;
    m.pinyin = "py" + std::to_string(i);
    m.initials = "i" + std::to_string(i);
    return m;
}
} // namespace

int main()
{
    std::cout << "=== test_kill_recovery ===" << std::endl;
    removeDb();

    // 先由父进程建好 Schema（子进程只做「强杀前的一批写入」）
    {
        CipherDatabase cdb;
        check(cdb.open(kPath, testKey()) == CipherError::Ok, "父进程建库并建 Schema");
        sqlite3* db = cdb.nativeHandle();
        check(SchemaManager::migrateTo(db, 0) == SchemaError::Ok, "建 Schema");
        cdb.close(); // 优雅关闭，仅用于准备 schema
    }

    // fork 子进程：写一批 + checkpoint 后 _exit（模拟强杀，不 close）
    pid_t pid = ::fork();
    if (pid == 0) {
        CipherDatabase cdb;
        sqlite3* db = nullptr;
        if (cdb.open(kPath, testKey()) == CipherError::Ok) db = cdb.nativeHandle();
        if (db) {
            std::string err;
            std::vector<MigrationMessage> batch{makeMsg(0)};
            if (exec(db, "BEGIN IMMEDIATE") &&
                MigrationImporter::importBatch(db, kOwner, batch, &err) &&
                MigrationImporter::saveCheckpoint(db, kOwner, kCheckpoint, &err) &&
                exec(db, "COMMIT")) {
                // 成功 COMMIT 后立即 _exit：不执行 CipherDatabase 析构、不 close、不 checkpoint
                _exit(0);
            }
        }
        _exit(1); // 写入失败
    } else if (pid < 0) {
        check(false, "fork 失败");
    }

    if (pid > 0) {
        int status = 0;
        ::waitpid(pid, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "子进程强杀前一批写入成功");

        // 父进程重开：WAL 崩溃恢复应能读到强杀前的 checkpoint
        CipherDatabase cdb;
        check(cdb.open(kPath, testKey()) == CipherError::Ok, "强杀后重开成功（WAL 恢复）");
        sqlite3* db = cdb.nativeHandle();
        std::string cp;
        check(MigrationImporter::readCheckpoint(db, kOwner, &cp), "读到 checkpoint");
        check(cp == kCheckpoint, "强杀前 checkpoint 持久（=" + cp + "）");

        // 续跑：再写一批，证明可继续迁移
        std::string err;
        std::vector<MigrationMessage> batch{makeMsg(1)};
        check(exec(db, "BEGIN IMMEDIATE") &&
                  MigrationImporter::importBatch(db, kOwner, batch, &err) &&
                  MigrationImporter::saveCheckpoint(db, kOwner, "43", &err) &&
                  exec(db, "COMMIT"),
              "强杀后可续跑 " + err);
        MigrationSummary sum;
        check(MigrationImporter::computeSummary(db, kOwner, &sum), "续跑后统计摘要");
        check(sum.messageCount == 2, "续跑后共 2 条消息");
        cdb.close();
    }

    removeDb();
    if (g_failures == 0) {
        std::cout << "test_kill_recovery PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_kill_recovery FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
