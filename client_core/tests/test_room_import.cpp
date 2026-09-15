// Room → Native 影子迁移导入器单元测试（P6-T05）。
//
// 覆盖 24.7（不含 Room 导出部分，导出在 Android 侧另行验证）：
//   - 0 / 1 / 分页边界 / 10 万条分批导入
//   - 重复导入（同批再导一次）不增行、FTS 不翻倍（幂等）
//   - checkpoint 可持久化与续跑
//   - 对账通过才写完成标记；对账不一致时不写标记
//   - 同 seq 不同 msg_id → 整体失败（数据一致性）
//   - 媒体只搬字符串：路径/尺寸/sha256 原样落库

#include "client_core/storage/CipherDatabase.h"
#include "client_core/storage/CipherParams.h"
#include "client_core/storage/MigrationImporter.h"
#include "client_core/storage/SchemaManager.h"

#include <sqlite3.h>

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

const char* kPath = "/tmp/test_room_import.db";
const std::int64_t kOwner = 7;

std::vector<unsigned char> testKey() { return std::vector<unsigned char>(32, 0x66); }

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

long long queryInt(sqlite3* db, const std::string& sql)
{
    sqlite3_stmt* s = nullptr;
    long long v = -1;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) == SQLITE_OK && s) {
        if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
    }
    if (s) sqlite3_finalize(s);
    return v;
}

MigrationMessage makeMsg(int i)
{
    MigrationMessage m;
    m.ownerId = kOwner;
    m.msgId = "m-" + std::to_string(i);
    m.conversationId = 100 + (i % 3);
    m.peerId = 2 + (i % 3);
    m.conversationSeq = i + 1; // 1..N
    m.serverTime = 1700000000 + i;
    m.localOrder = i;
    m.fromMe = (i % 2);
    m.type = 0;
    m.content = "消息内容" + std::to_string(i);
    m.status = 1;
    m.pinyin = "xiaoxineirong" + std::to_string(i);
    m.initials = "xxnr" + std::to_string(i);
    // 媒体字段：只搬字符串，不动文件
    m.mediaPath = "/tmp/media/" + std::to_string(i) + ".jpg";
    m.fileId = "fid-" + std::to_string(i);
    m.fileSize = 1024 + i;
    m.sha256 = "sha-" + std::to_string(i);
    m.localPath = "/tmp/local/" + std::to_string(i) + ".bin";
    return m;
}
} // namespace

int main()
{
    std::cout << "=== test_room_import ===" << std::endl;
    removeDb();

    CipherDatabase cdb;
    check(cdb.open(kPath, testKey()) == CipherError::Ok, "打开加密库");
    sqlite3* db = cdb.nativeHandle();
    check(SchemaManager::migrateTo(db, 0) == SchemaError::Ok, "建 Schema");

    // [1] 空批
    {
        std::string err;
        check(exec(db, "BEGIN IMMEDIATE") &&
                  MigrationImporter::importBatch(db, kOwner, {}, &err) &&
                  exec(db, "COMMIT"),
              "空批导入成功");
    }

    // [2] 单条
    {
        std::string err;
        std::vector<MigrationMessage> batch{makeMsg(0)};
        check(exec(db, "BEGIN IMMEDIATE") &&
                  MigrationImporter::importBatch(db, kOwner, batch, &err) &&
                  exec(db, "COMMIT"),
              "单条导入成功 " + err);
        check(queryInt(db, "SELECT count(*) FROM messages") == 1, "落库 1 条");
        check(queryInt(db, "SELECT count(*) FROM message_fts") == 1, "FTS 1 行");
    }

    // [3] 分页分批导入 100000 条（每批 500）
    const int kTotal = 100000;
    const int kBatch = 500;
    {
        bool allOk = true;
        std::string err;
        for (int start = 1; start < kTotal; start += kBatch) {
            std::vector<MigrationMessage> batch;
            for (int i = start; i < start + kBatch && i < kTotal; ++i) batch.push_back(makeMsg(i));
            const bool ok = exec(db, "BEGIN IMMEDIATE") &&
                            MigrationImporter::importBatch(db, kOwner, batch, &err) &&
                            MigrationImporter::saveCheckpoint(
                                db, kOwner, std::to_string(start + (int)batch.size()), &err) &&
                            exec(db, "COMMIT");
            if (!ok) { allOk = false; std::cout << "      err=" << err << std::endl; break; }
        }
        check(allOk, "分批导入 100000 条成功");
        check(queryInt(db, "SELECT count(*) FROM messages") == kTotal, "落库 100000 条");
        check(queryInt(db, "SELECT count(DISTINCT msg_id) FROM messages") == kTotal, "msg_id 无重复");
        check(queryInt(db, "SELECT count(*) FROM message_fts") == kTotal, "FTS 行数与消息数一致");
        check(queryInt(db, "SELECT count(*) FROM conversations") == 3, "会话摘要 3 个（peer 去重）");
    }

    // [4] checkpoint 持久化
    {
        std::string cp;
        check(MigrationImporter::readCheckpoint(db, kOwner, &cp), "读到 checkpoint");
        check(cp == std::to_string(kTotal), "checkpoint = 最后已提交游标（可续跑）");
    }

    // [5] 幂等：重复导入同一批，不增行、FTS 不翻倍
    {
        std::string err;
        std::vector<MigrationMessage> dup;
        for (int i = 0; i < 100; ++i) dup.push_back(makeMsg(i));
        const bool inTx = exec(db, "BEGIN IMMEDIATE") &&
                          MigrationImporter::importBatch(db, kOwner, dup, &err) &&
                          exec(db, "COMMIT");
        check(inTx, "重复导入同一批不报错 " + err);
        check(queryInt(db, "SELECT count(*) FROM messages") == kTotal, "消息数不变（幂等）");
        check(queryInt(db, "SELECT count(*) FROM message_fts") == kTotal, "FTS 不翻倍");
    }

    // [6] 媒体字段只搬字符串
    {
        sqlite3_stmt* s = nullptr;
        std::string path, sha;
        long long size = -1;
        if (sqlite3_prepare_v2(
                db, "SELECT local_path,sha256,file_size FROM messages WHERE msg_id='m-5'", -1,
                &s, nullptr) == SQLITE_OK && s) {
            if (sqlite3_step(s) == SQLITE_ROW) {
                const unsigned char* p = sqlite3_column_text(s, 0);
                if (p) path = reinterpret_cast<const char*>(p);
                const unsigned char* q = sqlite3_column_text(s, 1);
                if (q) sha = reinterpret_cast<const char*>(q);
                size = sqlite3_column_int64(s, 2);
            }
        }
        if (s) sqlite3_finalize(s);
        check(path == "/tmp/local/5.bin", "local_path 原样落库");
        check(sha == "sha-5", "sha256 原样落库");
        check(size == 1024 + 5, "file_size 原样落库");
    }

    // [7] 对账通过才写完成标记
    {
        MigrationSummary expected;
        check(MigrationImporter::computeSummary(db, kOwner, &expected), "计算实际摘要");
        bool completed = true;
        MigrationImporter::isCompleted(db, kOwner, &completed);
        check(!completed, "尚未完成迁移时 completed=0");

        std::string err;
        MigrationSummary actual;
        check(MigrationImporter::saveConversationCheckpoint(db, kOwner, "streams-complete", &err),
              "会话流 checkpoint 已提交");
        check(MigrationImporter::finish(db, kOwner, expected, &actual, &err),
              "对账一致 → 写完成标记 " + err);
        completed = false;
        MigrationImporter::isCompleted(db, kOwner, &completed);
        check(completed, "完成后 completed=1");
    }

    // [8] 对账不一致 → 不写完成标记（不悄悄跳过后宣称完成）
    {
        // 另起一个干净库，导入后故意少插一条，再拿"期望多一条"去对账
        removeDb();
        CipherDatabase cdb2;
        check(cdb2.open(kPath, testKey()) == CipherError::Ok, "新库打开");
        sqlite3* db2 = cdb2.nativeHandle();
        check(SchemaManager::migrateTo(db2, 0) == SchemaError::Ok, "新库建 Schema");
        std::vector<MigrationMessage> batch{makeMsg(0), makeMsg(1)};
        std::string err;
        check(exec(db2, "BEGIN IMMEDIATE") &&
                  MigrationImporter::importBatch(db2, kOwner, batch, &err) && exec(db2, "COMMIT"),
              "导入 2 条");

        MigrationSummary expected; // 期望 3 条（实际只有 2 条）
        expected.messageCount = 3;
        expected.conversationCount = 1;
        expected.minSeq = 1;
        expected.maxSeq = 2;
        expected.ftsCount = 3;
        std::string finishErr;
        MigrationImporter::saveCheckpoint(db2, kOwner, "2", nullptr);
        MigrationImporter::saveConversationCheckpoint(db2, kOwner, "1", nullptr);
        check(!MigrationImporter::finish(db2, kOwner, expected, nullptr, &finishErr),
              "对账不一致 → finish 失败");
        check(!finishErr.empty(), "给出对账差异（便于定位，不静默跳过）");
        bool completed = true;
        MigrationImporter::isCompleted(db2, kOwner, &completed);
        check(!completed, "对账失败不写完成标记");
        cdb2.close();
    }

    // [9] 同 seq 不同 msg_id → 数据一致性错误（整体回滚）
    {
        removeDb();
        CipherDatabase cdb3;
        cdb3.open(kPath, testKey());
        sqlite3* db3 = cdb3.nativeHandle();
        SchemaManager::migrateTo(db3, 0);
        std::vector<MigrationMessage> batch{makeMsg(0)};
        std::string err;
        exec(db3, "BEGIN IMMEDIATE");
        MigrationImporter::importBatch(db3, kOwner, batch, &err);
        exec(db3, "COMMIT");

        MigrationMessage conflict = makeMsg(0);
        conflict.msgId = "m-conflict";        // 不同 msg_id
        conflict.peerId = makeMsg(0).peerId;  // 同 peer
        conflict.conversationSeq = 1;         // 同 seq → 命中唯一约束 (owner,peer,seq)
        std::vector<MigrationMessage> bad{conflict};
        exec(db3, "BEGIN IMMEDIATE");
        const bool ok = MigrationImporter::importBatch(db3, kOwner, bad, &err);
        exec(db3, "ROLLBACK");
        check(!ok, "同 seq 不同 msg_id → 导入失败（数据一致性错误）");
        check(queryInt(db3, "SELECT count(*) FROM messages") == 1, "失败后不残留冲突行");
        cdb3.close();
    }

    // [10] 数据健壮性：超长正文 / 非法 UTF-8 / 损坏媒体元数据（不崩溃、可定位、不静默跳过）
    {
        removeDb();
        CipherDatabase cdb4;
        check(cdb4.open(kPath, testKey()) == CipherError::Ok, "健壮性库打开");
        sqlite3* db4 = cdb4.nativeHandle();
        check(SchemaManager::migrateTo(db4, 0) == SchemaError::Ok, "健壮性库建 Schema");

        // 10.1 超长正文（1 MiB）：应能导入、不 OOM、长度精确
        {
            MigrationMessage big = makeMsg(0);
            big.content = std::string(1024 * 1024, 'x');
            std::string err;
            check(exec(db4, "BEGIN IMMEDIATE") &&
                      MigrationImporter::importBatch(db4, kOwner, {big}, &err) &&
                      exec(db4, "COMMIT"),
                  "超长正文(1MiB)导入成功 " + err);
            check(queryInt(db4, "SELECT length(content) FROM messages WHERE msg_id='m-0'") ==
                      1024 * 1024,
                  "超长正文长度精确落库");
        }

        // 10.2 非法 UTF-8 正文：不崩溃；若 FTS 不接受则整体失败并可定位（不静默跳过）
        {
            MigrationMessage bad = makeMsg(1);
            bad.content = std::string("\xff\xfe\x80illegal", 11);
            bad.pinyin = "pinyin";
            bad.initials = "py";
            std::string err;
            const bool ok = exec(db4, "BEGIN IMMEDIATE") &&
                            MigrationImporter::importBatch(db4, kOwner, {bad}, &err) &&
                            exec(db4, "COMMIT");
            // 关键不变式：要么整体成功（messages 与 FTS 一致），要么整体失败且给出定位信息
            const long long msgs = queryInt(db4, "SELECT count(*) FROM messages");
            check(ok || msgs == 0, "非法 UTF-8：整体成功或整体回滚，不残留半批 " + err);
            if (ok) {
                check(msgs == 2, "非法 UTF-8 正文落库（不静默丢弃）");
            } else {
                check(!err.empty() && err.find("msg_id=m-1") != std::string::npos,
                      "非法 UTF-8 失败时错误可定位（含 msg_id）");
            }
        }

        // 10.3 损坏媒体元数据（负值 file_size / 尺寸）：能导入且原样落库，不崩溃
        {
            MigrationMessage neg = makeMsg(2);
            neg.fileSize = -12345;
            neg.imgW = -1;
            neg.thumbnailSize = -2;
            std::string err;
            check(exec(db4, "BEGIN IMMEDIATE") &&
                      MigrationImporter::importBatch(db4, kOwner, {neg}, &err) &&
                      exec(db4, "COMMIT"),
                  "损坏媒体元数据导入不崩溃 " + err);
            check(queryInt(db4, "SELECT file_size FROM messages WHERE msg_id='m-2'") == -12345,
                  "负值 file_size 原样落库（可定位）");
        }

        cdb4.close();
    }

    // [11] F06：重复 msg_id 不同内容 → 冲突回滚（不覆盖消息、不篡改会话摘要）
    {
        removeDb();
        CipherDatabase cdb5;
        check(cdb5.open(kPath, testKey()) == CipherError::Ok, "F06 库打开");
        sqlite3* db5 = cdb5.nativeHandle();
        check(SchemaManager::migrateTo(db5, 0) == SchemaError::Ok, "F06 库建 Schema");

        MigrationMessage orig = makeMsg(100);
        orig.content = "原始内容";
        std::string err;
        check(exec(db5, "BEGIN IMMEDIATE") &&
                  MigrationImporter::importBatch(db5, kOwner, {orig}, &err) &&
                  exec(db5, "COMMIT"),
              "F06 原始消息导入 " + err);

        // 同 msg_id 不同 content → 冲突回滚
        MigrationMessage changed = makeMsg(100);
        changed.content = "篡改后的内容";
        exec(db5, "BEGIN IMMEDIATE");
        const bool ok = MigrationImporter::importBatch(db5, kOwner, {changed}, &err);
        exec(db5, "ROLLBACK"); // 显式回滚（importBatch 失败不自动回滚）
        check(!ok, "F06：同 msg_id 不同内容 → 冲突回滚 " + err);
        check(!err.empty() && err.find("m-100") != std::string::npos, "F06：错误可定位（含 msg_id）");

        // 消息内容未被篡改
        sqlite3_stmt* s = nullptr;
        std::string content;
        if (sqlite3_prepare_v2(db5, "SELECT content FROM messages WHERE msg_id='m-100'", -1,
                               &s, nullptr) == SQLITE_OK && s) {
            if (sqlite3_step(s) == SQLITE_ROW) {
                const unsigned char* t = sqlite3_column_text(s, 0);
                if (t) content = reinterpret_cast<const char*>(t);
            }
        }
        if (s) sqlite3_finalize(s);
        check(content == "原始内容", "F06：消息内容未被篡改");

        // 同 msg_id 相同内容 → 幂等成功
        std::string err2;
        check(exec(db5, "BEGIN IMMEDIATE") &&
                  MigrationImporter::importBatch(db5, kOwner, {orig}, &err2) &&
                  exec(db5, "COMMIT"),
              "F06：同 msg_id 相同内容 → 幂等成功 " + err2);

        // 主消息相同但 FTS 拼音被破坏时不得把重复导入误判为成功。
        check(exec(db5, "UPDATE message_fts_identity SET pinyin='corrupt' WHERE msg_id='m-100'"),
              "构造 FTS identity 损坏");
        exec(db5, "BEGIN IMMEDIATE");
        const bool ftsOk = MigrationImporter::importBatch(db5, kOwner, {orig}, &err2);
        exec(db5, "ROLLBACK");
        check(!ftsOk && err2.find("FTS") != std::string::npos,
              "F06：重复消息同时校验 FTS 正文/拼音");

        cdb5.close();
    }

    cdb.close();
    removeDb();
    if (g_failures == 0) {
        std::cout << "test_room_import PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_room_import FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
