#include "Database.h"

#include <sqlite3.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>

namespace {

const char* DB_PATH = "/tmp/im_server_conversation_migration.db";
constexpr int USER_A1 = 1;
constexpr int USER_A2 = 1048579;
constexpr int USER_B1 = 2;
constexpr int USER_B2 = 3;

void removeTestDatabase()
{
    std::remove(DB_PATH);
    std::remove((std::string(DB_PATH) + "-wal").c_str());
    std::remove((std::string(DB_PATH) + "-shm").c_str());
}

sqlite3* openRawDatabase()
{
    sqlite3* db = nullptr;
    assert(sqlite3_open(DB_PATH, &db) == SQLITE_OK);
    return db;
}

void execute(sqlite3* db, const char* sql)
{
    char* error = nullptr;
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        std::cerr << "SQL failed: " << sql << " error="
                  << (error ? error : sqlite3_errmsg(db)) << std::endl;
    }
    if (error) sqlite3_free(error);
    assert(result == SQLITE_OK);
}

std::int64_t queryInt64(sqlite3* db, const char* sql)
{
    sqlite3_stmt* statement = nullptr;
    assert(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    const std::int64_t value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    return value;
}

} // namespace

int main()
{
    removeTestDatabase();

    const std::int64_t oldConversationId =
        static_cast<std::int64_t>(USER_A1) * (1LL << 20) + USER_A2;
    const std::int64_t oldCollisionId =
        static_cast<std::int64_t>(USER_B1) * (1LL << 20) + USER_B2;
    assert(oldConversationId == oldCollisionId);

    const std::int64_t newConversationA =
        imsrv::makeConversationId(USER_A1, USER_A2);
    const std::int64_t newConversationB =
        imsrv::makeConversationId(USER_B1, USER_B2);
    assert(newConversationA != newConversationB);

    // 先使用当前 Database 创建完整表结构并插入两条消息。
    {
        imsrv::Database db;
        assert(db.open(DB_PATH, 2));

        imsrv::StoredMessage first;
        first.msgId = "migration-conversation-a";
        first.senderId = USER_A1;
        first.receiverId = USER_A2;
        first.content = "old conversation A";
        first.ts = 100;
        assert(db.saveMessage(first, false));
        assert(first.seq == 1);

        imsrv::StoredMessage second;
        second.msgId = "migration-conversation-b";
        second.senderId = USER_B1;
        second.receiverId = USER_B2;
        second.content = "old conversation B";
        second.ts = 200;
        assert(db.saveMessage(second, true));
        assert(second.seq == 1);
    }

    // 模拟旧版本数据库：两个不同会话拥有相同 conversation_id。
    // seq 设置成不同值，以满足旧库中的 (conversation_id,seq) 唯一索引。
    {
        sqlite3* db = openRawDatabase();
        sqlite3_stmt* statement = nullptr;
        assert(sqlite3_prepare_v2(
            db,
            "UPDATE messages SET conversation_id=?, seq="
            "CASE WHEN msg_id='migration-conversation-a' THEN 1 ELSE 2 END "
            "WHERE msg_id IN ('migration-conversation-a','migration-conversation-b');",
            -1,
            &statement,
            nullptr
        ) == SQLITE_OK);
        sqlite3_bind_int64(statement, 1, oldConversationId);
        assert(sqlite3_step(statement) == SQLITE_DONE);
        assert(sqlite3_changes(db) == 2);
        sqlite3_finalize(statement);

        execute(db, "PRAGMA user_version=0;");
        assert(queryInt64(db, "PRAGMA user_version;") == 0);
        assert(queryInt64(
            db,
            "SELECT COUNT(DISTINCT conversation_id) FROM messages "
            "WHERE msg_id LIKE 'migration-conversation-%';"
        ) == 1);
        sqlite3_close(db);
    }

    // 重新打开时应自动执行 0 -> 1 的会话 ID 迁移。
    {
        imsrv::Database db;
        assert(db.open(DB_PATH, 2));

        const auto conversationA = db.roamMessages(USER_A1, USER_A2, INT64_MAX, 10);
        assert(conversationA.size() == 1);
        assert(conversationA[0].msgId == "migration-conversation-a");
        assert(conversationA[0].content == "old conversation A");
        assert(conversationA[0].seq == 1);

        // 反向查询必须命中同一会话。
        const auto conversationAReverse =
            db.roamMessages(USER_A2, USER_A1, INT64_MAX, 10);
        assert(conversationAReverse.size() == 1);
        assert(conversationAReverse[0].msgId == "migration-conversation-a");

        const auto conversationB = db.roamMessages(USER_B1, USER_B2, INT64_MAX, 10);
        assert(conversationB.size() == 1);
        assert(conversationB[0].msgId == "migration-conversation-b");
        assert(conversationB[0].content == "old conversation B");
        assert(conversationB[0].seq == 2);
    }

    // 直接检查物理数据和版本号，确认旧碰撞已经拆成两个会话。
    {
        sqlite3* db = openRawDatabase();
        assert(queryInt64(db, "PRAGMA user_version;") == 1);
        assert(queryInt64(
            db,
            "SELECT conversation_id FROM messages "
            "WHERE msg_id='migration-conversation-a';"
        ) == newConversationA);
        assert(queryInt64(
            db,
            "SELECT conversation_id FROM messages "
            "WHERE msg_id='migration-conversation-b';"
        ) == newConversationB);
        assert(queryInt64(
            db,
            "SELECT COUNT(DISTINCT conversation_id) FROM messages "
            "WHERE msg_id LIKE 'migration-conversation-%';"
        ) == 2);
        sqlite3_close(db);
    }

    // 再打开一次，验证迁移幂等，不会重复改写或丢失消息。
    {
        imsrv::Database db;
        assert(db.open(DB_PATH, 2));
        const auto a = db.roamMessages(USER_A1, USER_A2, INT64_MAX, 10);
        const auto b = db.roamMessages(USER_B1, USER_B2, INT64_MAX, 10);
        assert(a.size() == 1 && a[0].msgId == "migration-conversation-a");
        assert(b.size() == 1 && b[0].msgId == "migration-conversation-b");
    }

    removeTestDatabase();
    std::cout
        << "test_conversation_migration PASSED: oldCollision=" << oldConversationId
        << " newA=" << newConversationA
        << " newB=" << newConversationB
        << std::endl;
    return 0;
}
