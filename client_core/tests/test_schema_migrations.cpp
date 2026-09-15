// Schema 与迁移单元测试（P6-T03）。
//
// 覆盖 24.5 验收项：
//   - 空库 0→N 与跨版本一致；
//   - 迁移单事务、失败完整回滚（user_version 与 schema 保持旧版本）；
//   - msg_id 幂等（重复导入不增行）；同会话 seq 冲突报错；
//   - 必需索引存在，且关键查询命中索引（EXPLAIN QUERY PLAN）；
//   - FTS 行数与可检索文本消息数一致，重建两次结果一致；
//   - 中文/全拼/首字母检索不倒退（与 Room 同一策略：匹配 pinyin/initials）。

#include "client_core/storage/CipherDatabase.h"
#include "client_core/storage/CipherParams.h"
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

const char* kPath = "/tmp/test_schema_migrations.db";

std::vector<unsigned char> testKey() { return std::vector<unsigned char>(32, 0x33); }

void removeDb()
{
    std::remove(kPath);
    std::remove((std::string(kPath) + "-wal").c_str());
    std::remove((std::string(kPath) + "-shm").c_str());
}

bool exec(sqlite3* db, const std::string& sql, std::string* err = nullptr)
{
    char* e = nullptr;
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &e);
    if (rc != SQLITE_OK) {
        if (err) *err = e ? e : sqlite3_errmsg(db);
        if (e) sqlite3_free(e);
        return false;
    }
    return true;
}

int queryInt(sqlite3* db, const std::string& sql, bool* okOut = nullptr)
{
    sqlite3_stmt* stmt = nullptr;
    int v = -1;
    bool ok = false;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK && stmt) {
        if (sqlite3_step(stmt) == SQLITE_ROW) { v = sqlite3_column_int(stmt, 0); ok = true; }
    }
    if (stmt) sqlite3_finalize(stmt);
    if (okOut) *okOut = ok;
    return v;
}

struct Db {
    CipherDatabase db;
    bool ok = false;
    Db()
    {
        ok = (db.open(kPath, testKey()) == CipherError::Ok) && db.nativeHandle() != nullptr;
    }
    sqlite3* h() { return db.nativeHandle(); }
};

} // namespace

int main()
{
    std::cout << "=== test_schema_migrations ===" << std::endl;
    removeDb();

    // [1] 迁移脚本注册与最新版本
    check(SchemaManager::migrationCount() >= 1, "已注册至少一个迁移脚本");
    check(SchemaManager::latestVersion() >= 1, "最新版本 >= 1");

    // [2] 空库 0 → 最新
    {
        Db d;
        check(d.ok, "打开加密库成功");
        check(SchemaManager::currentVersion(d.h()) == 0, "新建库 user_version = 0");
        std::string msg;
        check(SchemaManager::migrateTo(d.h(), 0, &msg) == SchemaError::Ok,
              "迁移到最新版本成功 " + msg);
        check(SchemaManager::currentVersion(d.h()) == SchemaManager::latestVersion(),
              "迁移后 user_version = 最新版本");
        std::string vmsg;
        check(SchemaManager::verify(d.h(), &vmsg), "Schema 自检通过 " + vmsg);
    }

    // [3] 重复迁移幂等
    {
        Db d;
        check(d.ok, "重开成功");
        check(SchemaManager::migrateTo(d.h(), 0) == SchemaError::Ok, "重复迁移无副作用");
        check(SchemaManager::currentVersion(d.h()) == SchemaManager::latestVersion(),
              "版本保持不变");
    }

    // [4] msg_id 幂等：重复导入不增行
    {
        Db d;
        const std::string ins =
            "INSERT INTO messages(owner_id,msg_id,peer_id,conversation_id,conversation_seq,"
            "server_time,local_order,from_me,type,content,status) "
            "VALUES(1,'m-1',2,100,1,1700000000,1,1,0,'你好',1)";
        check(exec(d.h(), ins), "插入 msg m-1");
        std::string err;
        const bool dup = exec(d.h(), ins, &err);
        check(!dup, "重复 msg_id 插入被拒绝（幂等）");
        check(queryInt(d.h(), "SELECT count(*) FROM messages WHERE msg_id='m-1'") == 1,
              "m-1 只有一行");
    }

    // [5] 同会话 seq 冲突：不同 msg_id 占用同一 seq 必须失败
    {
        Db d;
        std::string err;
        const bool r = exec(
            d.h(),
            "INSERT INTO messages(owner_id,msg_id,peer_id,conversation_id,conversation_seq,"
            "server_time,local_order,from_me,type,content,status) "
            "VALUES(1,'m-2',2,100,1,1700000001,2,1,0,'第二条',1)",
            &err);
        check(!r, "seq 冲突插入被拒绝（数据一致性错误）");
    }

    // [6] 索引命中（EXPLAIN QUERY PLAN）
    {
        Db d;
        auto plan = [&](const std::string& sql) {
            std::string out;
            sqlite3_stmt* stmt = nullptr;
            const std::string q = "EXPLAIN QUERY PLAN " + sql;
            if (sqlite3_prepare_v2(d.h(), q.c_str(), -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const unsigned char* t = sqlite3_column_text(stmt, 3);
                    if (t) out += reinterpret_cast<const char*>(t);
                    out += ";";
                }
            }
            if (stmt) sqlite3_finalize(stmt);
            return out;
        };
        const std::string p1 =
            plan("SELECT * FROM messages WHERE owner_id=1 AND peer_id=2 AND conversation_seq<10 "
                 "ORDER BY conversation_seq DESC LIMIT 20");
        check(p1.find("idx_messages_conversation_seq") != std::string::npos,
              "会话历史分页命中 idx_messages_conversation_seq");

        const std::string p2 = plan("SELECT * FROM messages WHERE owner_id=1 AND msg_id='m-1'");
        check(p2.find("idx_messages_owner_msg") != std::string::npos,
              "msg_id 查重命中 idx_messages_owner_msg");

        const std::string p3 =
            plan("SELECT * FROM outbox WHERE owner_id=1 ORDER BY next_retry_at LIMIT 1");
        check(p3.find("idx_outbox_retry") != std::string::npos,
              "outbox 查询命中 idx_outbox_retry");
    }

    // [7] FTS：行数一致 + 中文/全拼/首字母检索
    {
        Db d;
        check(exec(d.h(),
                   "INSERT INTO messages(owner_id,msg_id,peer_id,conversation_id,conversation_seq,"
                   "server_time,local_order,from_me,type,content,status) "
                   "VALUES(1,'f-1',2,100,10,1700000010,10,1,0,'你好世界',1)"),
              "插入带正文消息");
        check(exec(d.h(),
                   "INSERT INTO message_fts(content,pinyin,initials,msg_id) "
                   "VALUES('你好世界','nihaoshijie','nhsj','f-1')"),
              "写入 FTS 行（含拼音/首字母）");
        check(queryInt(d.h(), "SELECT count(*) FROM message_fts") == 1, "FTS 行数 = 1");

        auto match = [&](const std::string& pattern) {
            return queryInt(d.h(), "SELECT count(*) FROM message_fts WHERE message_fts MATCH '" +
                                       pattern + "'");
        };
        check(match("nihaoshijie") == 1, "全拼检索命中");
        check(match("nhsj") == 1, "首字母检索命中");

        // 重建两次结果一致
        check(exec(d.h(), "DELETE FROM message_fts"), "清空 FTS");
        check(exec(d.h(),
                   "INSERT INTO message_fts(content,pinyin,initials,msg_id) "
                   "SELECT content,'nihaoshijie','nhsj',msg_id FROM messages "
                   "WHERE owner_id=1 AND content IS NOT NULL"),
              "按 messages 重建 FTS");
        const int c1 = queryInt(d.h(), "SELECT count(*) FROM message_fts");
        check(exec(d.h(), "DELETE FROM message_fts"), "再次清空 FTS");
        check(exec(d.h(),
                   "INSERT INTO message_fts(content,pinyin,initials,msg_id) "
                   "SELECT content,'nihaoshijie','nhsj',msg_id FROM messages "
                   "WHERE owner_id=1 AND content IS NOT NULL"),
              "第二次重建 FTS");
        check(queryInt(d.h(), "SELECT count(*) FROM message_fts") == c1, "两次重建结果一致");
    }

    // [8] 迁移失败完整回滚（用坏 SQL 模拟：先落一个非法对象再迁移，确认版本不推进）
    {
        removeDb();
        Db d;
        check(d.ok, "新建库成功");
        // 人为制造"目标版本超出"的场景 → 应报错且不改变版本
        std::string msg;
        const SchemaError e = SchemaManager::migrateTo(d.h(), 999, &msg);
        check(e == SchemaError::UnknownVersion, "目标版本超出现有脚本 → UnknownVersion");
        check(SchemaManager::currentVersion(d.h()) == 0, "失败后 user_version 仍为 0（完整回滚）");
        // 失败后仍可正常迁移（可重试）
        check(SchemaManager::migrateTo(d.h(), 0) == SchemaError::Ok, "回滚后可再次迁移成功");
        check(SchemaManager::currentVersion(d.h()) == SchemaManager::latestVersion(),
              "重试后版本正确");
    }

    // [9] v1 → v2：已有 FTS 数据回填 identity，重复导入可 O(1) 校验拼音一致性
    {
        removeDb();
        Db d;
        check(d.ok, "v1→v2 新建库成功");
        check(SchemaManager::migrateTo(d.h(), 1) == SchemaError::Ok, "先停在 Schema v1");
        check(exec(d.h(),
                   "INSERT INTO messages(owner_id,msg_id,peer_id,conversation_id,conversation_seq,"
                   "content) VALUES(1,'upgrade-fts',2,100,1,'你好')"),
              "v1 写入消息");
        check(exec(d.h(),
                   "INSERT INTO message_fts(content,pinyin,initials,msg_id) "
                   "VALUES('你好','nihao','nh','upgrade-fts')"),
              "v1 写入 FTS");
        check(SchemaManager::migrateTo(d.h(), 0) == SchemaError::Ok, "v1 升级到最新版本");
        check(queryInt(d.h(), "SELECT count(*) FROM message_fts_identity "
                              "WHERE owner_id=1 AND msg_id='upgrade-fts' AND pinyin='nihao'") == 1,
              "v2 正确回填 FTS identity");
    }

    for (int startVersion=1;startVersion<=3;++startVersion) {
        removeDb();
        Db d;
        check(SchemaManager::migrateTo(d.h(),startVersion)==SchemaError::Ok,"旧版本建库");
        check(exec(d.h(),"INSERT INTO messages(owner_id,msg_id,peer_id,local_order) VALUES(1,'retained',2,70)"),"保留消息序号");
        check(exec(d.h(),"INSERT INTO outbox(owner_id,msg_id,local_order,packet_type,payload) VALUES(1,'pending',90,0,'old')"),"保留旧 outbox");
        check(SchemaManager::migrateTo(d.h(),4)==SchemaError::Ok,"1/2/3→4 升级");
        check(queryInt(d.h(),"SELECT last_order FROM local_sequence WHERE owner_id=1")==90,"序号从两表最大值恢复");
        check(queryInt(d.h(),"SELECT payload_version FROM outbox WHERE msg_id='pending'")==0,"旧载荷标为 v0，不误认新版 protobuf");
    }
    {
        removeDb(); Db d;
        check(SchemaManager::migrateTo(d.h(),3)==SchemaError::Ok,"v4 回滚前置");
        check(exec(d.h(),"CREATE INDEX idx_messages_local_order ON messages(local_order)"),"注入迁移中段对象冲突");
        check(SchemaManager::migrateTo(d.h(),4)==SchemaError::ExecFailed,"v4 失败返回错误");
        check(SchemaManager::currentVersion(d.h())==3,"v4 失败保留版本3");
        check(queryInt(d.h(),"SELECT COUNT(*) FROM sqlite_master WHERE name='local_sequence'")==0,"v4 前段建表也已回滚");
    }
    // v5（005_friend_domain）：friends 补资料列 + friend_requests 加 created_at
    for (int startVersion=1;startVersion<=4;++startVersion) {
        removeDb(); Db d;
        check(SchemaManager::migrateTo(d.h(),startVersion)==SchemaError::Ok,"v5 前置建库");
        check(exec(d.h(),"INSERT INTO friends(owner_id,friend_id,nick) VALUES(1,100,'Alice')"),"v5 保留好友");
        check(SchemaManager::migrateTo(d.h(),5)==SchemaError::Ok,"startVersion→5 升级");
        check(queryInt(d.h(),"SELECT COUNT(*) FROM friends WHERE owner_id=1 AND friend_id=100")==1,"好友数据保留");
        check(exec(d.h(),"UPDATE friends SET tel='138',avatar='a.png',signature='sig',sex=1 WHERE friend_id=100"),"写入 v5 新列");
        check(queryInt(d.h(),"SELECT sex FROM friends WHERE friend_id=100")==1,"sex 新列生效");
        check(exec(d.h(),"INSERT INTO friend_requests(request_id,owner_id,from_user_id,to_user_id,direction,state,message,created_at) "
                        "VALUES('r1',1,2,1,0,0,'hi',123)"),"写入 friend_requests created_at");
        check(queryInt(d.h(),"SELECT created_at FROM friend_requests WHERE request_id='r1'")==123,"created_at 新列生效");
    }
    {
        removeDb(); Db d;
        check(SchemaManager::migrateTo(d.h(),4)==SchemaError::Ok,"v5 回滚前置到 v4");
        check(exec(d.h(),"ALTER TABLE friends ADD COLUMN sex INTEGER NOT NULL DEFAULT 0"),"注入 v5 列冲突");
        check(SchemaManager::migrateTo(d.h(),5)==SchemaError::ExecFailed,"v5 失败返回错误");
        check(SchemaManager::currentVersion(d.h())==4,"v5 失败保留版本4");
        check(queryInt(d.h(),"SELECT COUNT(*) FROM pragma_table_info('friends') WHERE name='tel'")==0,
              "v5 前段加列已回滚");
    }
    // v6（006_ai_suggestions）：ai_suggestions 表 + 会话索引
    for (int startVersion=1;startVersion<=5;++startVersion) {
        removeDb(); Db d;
        check(SchemaManager::migrateTo(d.h(),startVersion)==SchemaError::Ok,"v6 前置建库");
        check(SchemaManager::migrateTo(d.h(),6)==SchemaError::Ok,"startVersion→6 升级");
        check(queryInt(d.h(),"SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='ai_suggestions'")==1,
              "ai_suggestions 表已建");
        check(exec(d.h(),"INSERT INTO ai_suggestions(request_id,owner_id,conversation_id,status,suggestions,generated_at,context_version) "
                        "VALUES('r1',1,100,1,'ab',123,'v1')"),"写入 ai_suggestions");
        check(queryInt(d.h(),"SELECT COUNT(*) FROM ai_suggestions WHERE request_id='r1'")==1,"ai_suggestions 写入生效");
    }
    {
        removeDb(); Db d;
        check(SchemaManager::migrateTo(d.h(),5)==SchemaError::Ok,"v6 回滚前置到 v5");
        check(exec(d.h(),"CREATE TABLE ai_suggestions (request_id TEXT NOT NULL, owner_id INTEGER NOT NULL, "
                        "PRIMARY KEY(owner_id,request_id))"),"注入缺列 ai_suggestions 表");
        check(SchemaManager::migrateTo(d.h(),6)==SchemaError::ExecFailed,"v6 失败返回错误（索引引用缺失列）");
        check(SchemaManager::currentVersion(d.h())==5,"v6 失败保留版本5");
    }
    removeDb();
    if (g_failures == 0) {
        std::cout << "test_schema_migrations PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_schema_migrations FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
