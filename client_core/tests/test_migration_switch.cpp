// 迁移开关三态 + 清理影子库 单测（P6-T05 / §24.7）。
//
// 覆盖：
//   - 初始状态 Disabled；beginImport → ShadowImport；
//   - 导入 + finish 对账通过 → Verified（finish 自动置位）；
//   - 已 Verified 时 beginImport 拒绝重复迁移；
//   - resetForReimport 回滚：状态回 ShadowImport、清 completed 与 checkpoint；
//   - 状态持久化（关库重开后恢复）；
//   - clearShadowDatabase 只删影子库自身与旁路文件，不碰生产 Room 与媒体文件。

#include "client_core/storage/CipherDatabase.h"
#include "client_core/storage/CipherParams.h"
#include "client_core/storage/DatabasePaths.h"
#include "client_core/storage/MigrationImporter.h"
#include "client_core/storage/SchemaManager.h"

#include <sqlite3.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
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

const char* kPath = "/tmp/test_migration_switch.db";
const std::int64_t kOwner = 9;
const std::string kFilesDir = "/tmp/migration_switch_files";

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
    m.content = "content" + std::to_string(i);
    m.status = 1;
    m.pinyin = "py" + std::to_string(i);
    m.initials = "i" + std::to_string(i);
    return m;
}

bool fileExists(const std::string& p)
{
    std::ifstream f(p.c_str());
    return f.good();
}
} // namespace

int main()
{
    std::cout << "=== test_migration_switch ===" << std::endl;
    removeDb();

    CipherDatabase cdb;
    check(cdb.open(kPath, testKey()) == CipherError::Ok, "打开加密库");
    sqlite3* db = cdb.nativeHandle();
    check(SchemaManager::migrateTo(db, 0) == SchemaError::Ok, "建 Schema");

    // [1] 初始状态 Disabled
    {
        MigrationState st;
        check(MigrationImporter::getState(db, kOwner, &st), "读取初始状态");
        check(st == MigrationState::Disabled, "初始状态 = Disabled");
    }

    // [2] beginImport → ShadowImport
    {
        std::string err;
        check(MigrationImporter::beginImport(db, kOwner, &err), "beginImport 成功 " + err);
        MigrationState st;
        MigrationImporter::getState(db, kOwner, &st);
        check(st == MigrationState::ShadowImport, "beginImport 后 = ShadowImport");
    }

    // [3] 导入 + finish 对账通过 → Verified（finish 自动置位）
    {
        std::string err;
        std::vector<MigrationMessage> batch{makeMsg(0), makeMsg(1)};
        check(exec(db, "BEGIN IMMEDIATE") &&
                  MigrationImporter::importBatch(db, kOwner, batch, &err) &&
                  MigrationImporter::saveCheckpoint(db, kOwner, "2", &err) &&
                  MigrationImporter::saveConversationCheckpoint(db, kOwner, "1", &err) &&
                  exec(db, "COMMIT"),
              "导入 2 条 " + err);
        MigrationSummary expected;
        MigrationImporter::computeSummary(db, kOwner, &expected);
        MigrationSummary actual;
        check(MigrationImporter::finish(db, kOwner, expected, &actual, &err), "finish 成功 " + err);
        MigrationState st;
        MigrationImporter::getState(db, kOwner, &st);
        check(st == MigrationState::Verified, "finish 后 = Verified");
        bool completed = false;
        MigrationImporter::isCompleted(db, kOwner, &completed);
        check(completed, "completed=1 保持兼容");
    }

    // [4] 已 Verified 时 beginImport 拒绝重复迁移
    {
        std::string err;
        check(!MigrationImporter::beginImport(db, kOwner, &err), "已 verified 时 beginImport 被拒");
        check(!err.empty(), "给出拒绝原因 " + err);
    }

    // [5] resetForReimport 回滚：状态回 ShadowImport、清 completed 与 checkpoint
    {
        std::string err;
        check(MigrationImporter::resetForReimport(db, kOwner, &err), "resetForReimport 成功 " + err);
        MigrationState st;
        MigrationImporter::getState(db, kOwner, &st);
        check(st == MigrationState::ShadowImport, "回滚后 = ShadowImport");
        bool completed = true;
        MigrationImporter::isCompleted(db, kOwner, &completed);
        check(!completed, "completed 清除");
        std::string cp = "x";
        MigrationImporter::readCheckpoint(db, kOwner, &cp);
        check(cp.empty(), "checkpoint 清除");
    }

    // [6] 状态持久化：关库重开后恢复
    {
        cdb.close();
        CipherDatabase cdb2;
        check(cdb2.open(kPath, testKey()) == CipherError::Ok, "重开加密库");
        sqlite3* db2 = cdb2.nativeHandle();
        MigrationState st;
        check(MigrationImporter::getState(db2, kOwner, &st), "重开后读取状态");
        check(st == MigrationState::ShadowImport, "状态持久化恢复（ShadowImport）");
        cdb2.close();
    }

    // [7] clearShadowDatabase 只删影子库自身与旁路，不碰生产 Room 与媒体文件
    {
        const std::string dir = kFilesDir + "/native_db";
        ::mkdir(kFilesDir.c_str(), 0700);
        ::mkdir(dir.c_str(), 0700);
        const std::string shadowPath = dir + "/account_" + std::to_string(kOwner) + ".db";
        { std::ofstream f(shadowPath); f << "shadow"; }
        { std::ofstream f(shadowPath + "-wal"); f << "wal"; }
        { std::ofstream f(shadowPath + "-shm"); f << "shm"; }
        { std::ofstream f(shadowPath + ".lock"); f << "lock"; }
        const std::string roomPath = kFilesDir + "/jitong_" + std::to_string(kOwner) + ".db";
        { std::ofstream f(roomPath); f << "room"; }
        const std::string imgDir = kFilesDir + "/img";
        ::mkdir(imgDir.c_str(), 0700);
        const std::string mediaPath = imgDir + "/photo.jpg";
        { std::ofstream f(mediaPath); f << "media"; }

        std::string err;
        check(clearShadowDatabase(kFilesDir, kOwner, &err), "clearShadowDatabase 成功 " + err);
        check(!fileExists(shadowPath), "影子库 db 已删除");
        check(!fileExists(shadowPath + "-wal"), "wal 已删除");
        check(!fileExists(shadowPath + "-shm"), "shm 已删除");
        check(fileExists(shadowPath + ".lock"), "F10：lock 文件永久保留（互斥锚点不删）");
        check(fileExists(roomPath), "生产 Room 未被删除");
        check(fileExists(mediaPath), "媒体文件未被删除");

        std::string err2;
        check(!clearShadowDatabase(kFilesDir, -1, &err2), "非法 ownerId 拒绝清理");
    }

    cdb.close();
    removeDb();
    // 清理测试目录
    {
        std::remove((kFilesDir + "/native_db/account_" + std::to_string(kOwner) + ".db").c_str());
        std::remove((kFilesDir + "/jitong_" + std::to_string(kOwner) + ".db").c_str());
        std::remove((kFilesDir + "/img/photo.jpg").c_str());
        ::rmdir((kFilesDir + "/native_db").c_str());
        ::rmdir((kFilesDir + "/img").c_str());
        ::rmdir(kFilesDir.c_str());
    }

    if (g_failures == 0) {
        std::cout << "test_migration_switch PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_migration_switch FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
