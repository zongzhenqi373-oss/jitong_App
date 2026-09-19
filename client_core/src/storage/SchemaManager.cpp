#include "client_core/storage/SchemaManager.h"

#include <sqlite3.h>

#include "migrations_sql.h" // 由 CMake 从 src/storage/migrations/*.sql 生成

namespace im {
namespace storage {

namespace {

struct MigrationStep {
    int version;         // 执行完这一步后 user_version 应达到的版本
    const char* name;    // 便于日志/诊断
    const char* sql;
};

const MigrationStep kMigrations[] = {
    {1, "001_initial", kMigration001Sql},
    {2, "002_fts_identity", kMigration002Sql},
    {3, "003_p7_state_machines", kMigration003Sql},
    {4, "004_local_sequence", kMigration004Sql},
    {5, "005_friend_domain", kMigration005Sql},
    {6, "006_ai_suggestions", kMigration006Sql},
    {7, "007_upload_drafts", kMigration007Sql},
    {8, "008_upload_dimensions", kMigration008Sql},
};

int latestVersionImpl()
{
    int max = 0;
    for (const auto& m : kMigrations) if (m.version > max) max = m.version;
    return max;
}

} // namespace

const char* toString(SchemaError e)
{
    switch (e) {
        case SchemaError::Ok:             return "Ok";
        case SchemaError::ExecFailed:     return "ExecFailed";
        case SchemaError::BeginFailed:    return "BeginFailed";
        case SchemaError::VersionFailed:  return "VersionFailed";
        case SchemaError::UnknownVersion: return "UnknownVersion";
    }
    return "?";
}

int SchemaManager::currentVersion(sqlite3* db)
{
    if (!db) return -1;
    sqlite3_stmt* stmt = nullptr;
    int version = 0;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, nullptr) == SQLITE_OK && stmt) {
        if (sqlite3_step(stmt) == SQLITE_ROW) version = sqlite3_column_int(stmt, 0);
    }
    if (stmt) sqlite3_finalize(stmt);
    return version;
}

int SchemaManager::latestVersion() { return latestVersionImpl(); }

std::size_t SchemaManager::migrationCount()
{
    return sizeof(kMigrations) / sizeof(kMigrations[0]);
}

SchemaError SchemaManager::migrateTo(sqlite3* db, int targetVersion, std::string* message)
{
    if (message) message->clear();
    if (!db) return SchemaError::ExecFailed;

    const int latest = latestVersionImpl();
    if (targetVersion <= 0) targetVersion = latest;
    if (targetVersion > latest) {
        if (message) *message = "目标版本超出已有迁移脚本";
        return SchemaError::UnknownVersion;
    }

    int current = currentVersion(db);
    if (current < 0) {
        if (message) *message = "无法读取 user_version";
        return SchemaError::VersionFailed;
    }

    while (current < targetVersion) {
        const MigrationStep* step = nullptr;
        for (const auto& m : kMigrations) {
            if (m.version == current + 1) { step = &m; break; }
        }
        if (!step) {
            if (message) *message = "缺少到版本 " + std::to_string(current + 1) + " 的迁移脚本";
            return SchemaError::UnknownVersion;
        }

        // ---- 单事务：DDL + user_version 一起提交；失败整体回滚 ----
        char* err = nullptr;
        if (sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, &err) != SQLITE_OK) {
            if (message) {
                *message = std::string("开启事务失败：") + (err ? err : "");
            }
            if (err) sqlite3_free(err);
            return SchemaError::BeginFailed;
        }

        bool ok = true;
        if (sqlite3_exec(db, step->sql, nullptr, nullptr, &err) != SQLITE_OK) {
            if (message) {
                *message = std::string(step->name) + " 执行失败：" + (err ? err : "");
            }
            if (err) sqlite3_free(err);
            ok = false;
        }

        if (ok) {
            const std::string setVersion =
                "PRAGMA user_version = " + std::to_string(step->version) + ";";
            if (sqlite3_exec(db, setVersion.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
                if (message) {
                    *message = std::string("写入 user_version 失败：") + (err ? err : "");
                }
                if (err) sqlite3_free(err);
                ok = false;
            }
        }

        if (!ok) {
            sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
            return SchemaError::ExecFailed;
        }
        if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &err) != SQLITE_OK) {
            if (message) {
                *message = std::string("提交失败：") + (err ? err : "");
            }
            if (err) sqlite3_free(err);
            sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
            return SchemaError::ExecFailed;
        }
        current = step->version;
    }

    return SchemaError::Ok;
}

bool SchemaManager::verify(sqlite3* db, std::string* message)
{
    if (message) message->clear();
    if (!db) return false;

    const char* kRequiredIndexes[] = {
        "idx_messages_owner_msg",
        "idx_messages_conversation_seq",
        "idx_messages_server_time",
        "idx_messages_status",
        "idx_messages_owner_peer_seq",
        "idx_conversations_owner_peer",
        "idx_conversations_recent",
        "idx_outbox_retry",
    };

    for (const char* idx : kRequiredIndexes) {
        sqlite3_stmt* stmt = nullptr;
        const int rc = sqlite3_prepare_v2(
            db, "SELECT count(*) FROM sqlite_master WHERE type='index' AND name=?", -1, &stmt,
            nullptr);
        if (rc != SQLITE_OK || !stmt) {
            if (message) *message = std::string("查询索引失败：") + idx;
            if (stmt) sqlite3_finalize(stmt);
            return false;
        }
        sqlite3_bind_text(stmt, 1, idx, -1, SQLITE_TRANSIENT);
        int count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        if (count != 1) {
            if (message) *message = std::string("缺少必需索引：") + idx;
            return false;
        }
    }

    // message_fts 必须可用（能查到表且能执行 MATCH 查询）
    {
        sqlite3_stmt* stmt = nullptr;
        const int rc = sqlite3_prepare_v2(
            db, "SELECT count(*) FROM message_fts WHERE message_fts MATCH 'x'", -1, &stmt, nullptr);
        if (rc != SQLITE_OK || !stmt) {
            if (message) *message = "message_fts 不可用（非 fts4 表或建表失败）";
            if (stmt) sqlite3_finalize(stmt);
            return false;
        }
        sqlite3_finalize(stmt);
    }
    return true;
}

} // namespace storage
} // namespace im
