// P7-G2：Schema v3（状态机实体）迁移测试。
//
// 验收（v2 §3 P7-G2）：
//   - 空库 0→3、现有 Native 1→3 与 2→3 均可升级；
//   - 迁移幂等（重复 migrateTo 不报错、版本不变）；
//   - 新表具备预期语义（好友申请方向/去重、缺洞区间、三档媒体、引用计数、
//     传输 generation 防串号、cutover journal 四态）；
//   - 关键查询命中目标索引（EXPLAIN QUERY PLAN）。

#include <sqlite3.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "client_core/storage/CipherDatabase.h"
#include "client_core/storage/CipherParams.h"
#include "client_core/storage/SchemaManager.h"

using namespace im::storage;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& name)
{
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << std::endl;
    if (!cond) ++g_failures;
}

const char* kPath = "/tmp/test_schema_v3.db";

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
    if (err) {
        std::cout << "      SQL 错误: " << err << std::endl;
        sqlite3_free(err);
    }
    return rc == SQLITE_OK;
}

bool tableExists(sqlite3* db, const std::string& name)
{
    sqlite3_stmt* s = nullptr;
    bool found = false;
    const std::string sql =
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name=?";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) == SQLITE_OK && s) {
        sqlite3_bind_text(s, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_ROW) found = sqlite3_column_int(s, 0) > 0;
    }
    if (s) sqlite3_finalize(s);
    return found;
}

bool indexExists(sqlite3* db, const std::string& name)
{
    sqlite3_stmt* s = nullptr;
    bool found = false;
    const std::string sql =
        "SELECT count(*) FROM sqlite_master WHERE type='index' AND name=?";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) == SQLITE_OK && s) {
        sqlite3_bind_text(s, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_ROW) found = sqlite3_column_int(s, 0) > 0;
    }
    if (s) sqlite3_finalize(s);
    return found;
}

/** EXPLAIN QUERY PLAN 输出是否包含指定索引名。 */
bool planUsesIndex(sqlite3* db, const std::string& query, const std::string& indexName)
{
    sqlite3_stmt* s = nullptr;
    bool used = false;
    const std::string sql = "EXPLAIN QUERY PLAN " + query;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) == SQLITE_OK && s) {
        while (sqlite3_step(s) == SQLITE_ROW) {
            const unsigned char* t = sqlite3_column_text(s, 3);
            if (t) {
                const std::string detail = reinterpret_cast<const char*>(t);
                if (detail.find(indexName) != std::string::npos) used = true;
            }
        }
    }
    if (s) sqlite3_finalize(s);
    return used;
}

sqlite3* openAt(int targetVersion, std::string* err)
{
    removeDb();
    CipherDatabase* cdb = new CipherDatabase();
    if (cdb->open(kPath, testKey()) != CipherError::Ok) {
        delete cdb;
        return nullptr;
    }
    sqlite3* db = cdb->nativeHandle();
    if (SchemaManager::migrateTo(db, targetVersion, err) != SchemaError::Ok) {
        cdb->close();
        delete cdb;
        return nullptr;
    }
    return db;
}
} // namespace

int main()
{
    std::cout << "=== test_schema_v3 ===" << std::endl;

    // [1] 最新版应为 6（001..006，保留 v3/v4/v5 回归）
    check(SchemaManager::latestVersion() == 6, "latestVersion() == 6");
    check(SchemaManager::migrationCount() == 6, "已注册 6 个迁移脚本（保留 v3/v4/v5 回归）");

    // [2] 空库 0→3：全部新表存在
    {
        removeDb();
        CipherDatabase cdb;
        check(cdb.open(kPath, testKey()) == CipherError::Ok, "打开空库");
        sqlite3* db = cdb.nativeHandle();
        std::string msg;
        check(SchemaManager::migrateTo(db, 3, &msg) == SchemaError::Ok, "0→3 迁移 " + msg);
        check(SchemaManager::currentVersion(db) == 3, "user_version == 3");

        const char* tables[] = {"friend_requests",     "sync_gaps",
                                "media_variants",      "media_refs",
                                "transfer_parts",      "migration_checkpoint",
                                "cutover_journal"};
        for (const char* t : tables) {
            check(tableExists(db, t), std::string("新表存在: ") + t);
        }
        cdb.close();
    }

    // [3] 升级路径 1→3 与 2→3
    {
        std::string err;
        {
            sqlite3* db = openAt(1, &err);
            check(db != nullptr, "停在 v1 " + err);
            if (db) {
                check(SchemaManager::currentVersion(db) == 1, "v1 版本正确");
                check(SchemaManager::migrateTo(db, 3, &err) == SchemaError::Ok, "1→3 " + err);
                check(SchemaManager::currentVersion(db) == 3, "1→3 后为 3");
                check(tableExists(db, "cutover_journal"), "1→3 后 cutover_journal 存在");
            }
        }
        {
            sqlite3* db = openAt(2, &err);
            check(db != nullptr, "停在 v2 " + err);
            if (db) {
                check(SchemaManager::migrateTo(db, 3, &err) == SchemaError::Ok, "2→3 " + err);
                check(SchemaManager::currentVersion(db) == 3, "2→3 后为 3");
            }
        }
    }

    // [4] 幂等：重复 migrateTo(3) 不报错、版本不变
    {
        std::string err;
        sqlite3* db = openAt(3, &err);
        if (db) {
            check(SchemaManager::migrateTo(db, 3, &err) == SchemaError::Ok, "重复 migrateTo(3) 成功");
            check(SchemaManager::currentVersion(db) == 3, "重复迁移后版本仍为 3");
            check(SchemaManager::migrateTo(db, 2, &err) != SchemaError::Ok ||
                      SchemaManager::currentVersion(db) == 3,
                  "不允许回退到旧版本（或回退被拒）");
        }
    }

    // [5] 好友申请：方向/状态/去重（唯一键防重复）
    {
        std::string err;
        sqlite3* db = openAt(3, &err);
        if (db) {
            check(exec(db, "INSERT INTO friend_requests(request_id,owner_id,from_user_id,"
                           "to_user_id,direction,state,server_version,updated_at) "
                           "VALUES('r1',1,2,1,0,0,100,1700000000)"),
                  "插入好友申请");
            // 唯一键防重复
            check(!exec(db, "INSERT INTO friend_requests(request_id,owner_id,from_user_id,"
                            "to_user_id,direction,state) VALUES('r1',1,2,1,0,0)"),
                  "同一 (owner,request_id) 重复插入被拒（唯一键）");
            // 按状态查询
            sqlite3_stmt* s = nullptr;
            int n = -1;
            if (sqlite3_prepare_v2(db, "SELECT count(*) FROM friend_requests WHERE owner_id=1 "
                                       "AND state=0", -1, &s, nullptr) == SQLITE_OK && s) {
                if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
            }
            if (s) sqlite3_finalize(s);
            check(n == 1, "按 owner+state 查询到 1 条");
        }
    }

    // [6] 缺洞区间：闭区间存储，可按重试时间检索
    {
        std::string err;
        sqlite3* db = openAt(3, &err);
        if (db) {
            check(exec(db, "INSERT INTO sync_gaps(owner_id,conversation_id,gap_from,gap_to,"
                           "attempt,next_retry_at,updated_at) VALUES(1,555,10,20,1,1700000060,0)"),
                  "插入缺洞区间 [10,20]");
            check(!exec(db, "INSERT INTO sync_gaps(owner_id,conversation_id,gap_from,gap_to) "
                            "VALUES(1,555,10,30)"),
                  "同起点重复区间被拒（主键）");
            sqlite3_stmt* s = nullptr;
            int cnt = -1;
            if (sqlite3_prepare_v2(db, "SELECT count(*) FROM sync_gaps WHERE owner_id=1 AND "
                                       "next_retry_at<=1700009999", -1, &s, nullptr) == SQLITE_OK && s) {
                if (sqlite3_step(s) == SQLITE_ROW) cnt = sqlite3_column_int(s, 0);
            }
            if (s) sqlite3_finalize(s);
            check(cnt == 1, "按 retry 时间检索到 1 个缺洞");
        }
    }

    // [7] 三档媒体 + 引用计数（real_mime 必须反映真实格式）
    {
        std::string err;
        sqlite3* db = openAt(3, &err);
        if (db) {
            check(exec(db, "INSERT INTO media_variants(media_id,variant,real_mime,width,height,"
                           "file_size,file_hash,state) VALUES('m1',0,'image/avif',4000,3000,1024,'h0',1)"),
                  "插入原图变体（avif）");
            check(exec(db, "INSERT INTO media_variants(media_id,variant,real_mime,width,height,"
                           "file_size,file_hash,state) VALUES('m1',1,'image/jpeg',640,480,64,'h1',1)"),
                  "插入大缩略图（jpeg，如实标注不冒充 avif）");
            check(exec(db, "INSERT INTO media_refs(owner_id,msg_id,media_id,variant) "
                           "VALUES(1,'msg1','m1',0)"),
                  "插入媒体引用");
            // 按 hash 查变体
            sqlite3_stmt* s = nullptr;
            std::string mime;
            if (sqlite3_prepare_v2(db, "SELECT real_mime FROM media_variants WHERE file_hash='h1'",
                                   -1, &s, nullptr) == SQLITE_OK && s) {
                if (sqlite3_step(s) == SQLITE_ROW) {
                    const unsigned char* t = sqlite3_column_text(s, 0);
                    if (t) mime = reinterpret_cast<const char*>(t);
                }
            }
            if (s) sqlite3_finalize(s);
            check(mime == "image/jpeg", "缩略图 mime 如实为 image/jpeg（未被标成 avif）");
            check(indexExists(db, "idx_media_variants_hash"), "media_variants hash 索引存在");
        }
    }

    // [8] 传输任务新列：generation 防串号、offset 断点、错误域、终态
    {
        std::string err;
        sqlite3* db = openAt(3, &err);
        if (db) {
            check(exec(db, "INSERT INTO transfer_tasks(task_id,owner_id,msg_id,file_id,direction,"
                           "total_size,transferred,state,generation,phase,offset_bytes,"
                           "error_domain,terminal_state) "
                           "VALUES('t1',1,'msg1','f1',1,1024,512,1,7,2,512,'network',0)"),
                  "插入带新列的传输任务");
            sqlite3_stmt* s = nullptr;
            int gen = -1, off = -1;
            std::string dom;
            if (sqlite3_prepare_v2(db, "SELECT generation,offset_bytes,error_domain FROM "
                                       "transfer_tasks WHERE task_id='t1'", -1, &s, nullptr) == SQLITE_OK && s) {
                if (sqlite3_step(s) == SQLITE_ROW) {
                    gen = sqlite3_column_int(s, 0);
                    off = sqlite3_column_int(s, 1);
                    const unsigned char* t = sqlite3_column_text(s, 2);
                    if (t) dom = reinterpret_cast<const char*>(t);
                }
            }
            if (s) sqlite3_finalize(s);
            check(gen == 7, "generation=7（换号防串号）");
            check(off == 512, "offset_bytes=512（断点续传）");
            check(dom == "network", "error_domain=network（错误域可定位）");
        }
    }

    // [9] Cutover journal：ADR-03 四态可存储与推进
    {
        std::string err;
        sqlite3* db = openAt(3, &err);
        if (db) {
            check(exec(db, "INSERT INTO cutover_journal(epoch,state,high_water,schema_version,"
                           "key_id,summary,dirty,updated_at) "
                           "VALUES(1,0,0,3,'k1','',0,1700000000)"),
                  "插入 epoch=1 state=LEGACY_ACTIVE(0)");
            // 推进 PREPARED → NO_WRITE → DIRTY
            check(exec(db, "UPDATE cutover_journal SET state=1,high_water=100 WHERE epoch=1"),
                  "推进到 PREPARED(1)");
            check(exec(db, "UPDATE cutover_journal SET state=2 WHERE epoch=1"),
                  "推进到 NO_WRITE(2)");
            check(exec(db, "UPDATE cutover_journal SET state=3,dirty=1 WHERE epoch=1"),
                  "推进到 DIRTY(3) 并置 dirty=1");
            // epoch 主键：同一 epoch 只允许一条（恢复矩阵不猜测）
            check(!exec(db, "INSERT INTO cutover_journal(epoch,state) VALUES(1,0)"),
                  "同一 epoch 重复插入被拒（主键防多值歧义）");
        }
    }

    // [10] 迁移 checkpoint：每流独立水位
    {
        std::string err;
        sqlite3* db = openAt(3, &err);
        if (db) {
            check(exec(db, "INSERT INTO migration_checkpoint(epoch,stream,checkpoint_value,"
                           "updated_at) VALUES(1,'messages','900',1)"),
                  "写入 messages 流水位");
            check(exec(db, "INSERT INTO migration_checkpoint(epoch,stream,checkpoint_value,"
                           "updated_at) VALUES(1,'conversations','42',1)"),
                  "写入 conversations 流水位（独立）");
            sqlite3_stmt* s = nullptr;
            int n = -1;
            if (sqlite3_prepare_v2(db, "SELECT count(*) FROM migration_checkpoint WHERE epoch=1",
                                   -1, &s, nullptr) == SQLITE_OK && s) {
                if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
            }
            if (s) sqlite3_finalize(s);
            check(n == 2, "同一 epoch 下 2 个流各自独立水位");
        }
    }

    // [11] 索引命中（EXPLAIN QUERY PLAN）
    {
        std::string err;
        sqlite3* db = openAt(3, &err);
        if (db) {
            check(indexExists(db, "idx_friend_requests_state"), "好友申请状态索引存在");
            check(indexExists(db, "idx_sync_gaps_retry"), "缺洞重试索引存在");
            check(indexExists(db, "idx_media_refs_media"), "媒体引用索引存在");
            check(indexExists(db, "idx_transfer_tasks_generation"), "传输 generation 索引存在");

            const bool ok1 = planUsesIndex(db, "SELECT * FROM friend_requests WHERE owner_id=1 "
                                               "AND state=0", "idx_friend_requests_state");
            const bool ok2 = planUsesIndex(db, "SELECT * FROM sync_gaps WHERE owner_id=1 AND "
                                               "next_retry_at<=1700009999", "idx_sync_gaps_retry");
            check(ok1, "好友申请查询命中 idx_friend_requests_state");
            check(ok2, "缺洞查询命中 idx_sync_gaps_retry");
        }
    }

    removeDb();
    if (g_failures == 0) {
        std::cout << "test_schema_v3 PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_schema_v3 FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
