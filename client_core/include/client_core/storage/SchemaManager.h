// Schema 版本化与迁移（P6-T03）。
//
// 规则（对应 24.5）：
//   - 版本用 `PRAGMA user_version` 表示；每次升级在**单个事务**内执行，
//     失败则整体回滚，`user_version` 与 schema 保持一致；
//   - 禁止破坏性迁移（不 drop-recreate、不捕获异常后删库）；
//   - 执行任何 Schema 写操作前先校验 key 已正确（由 CipherDatabase 保证）。
//
// SQL 源文件在 src/storage/migrations/*.sql，由 CMake 生成为内嵌字符串（不依赖运行时文件）。

#ifndef CLIENT_CORE_SCHEMA_MANAGER_H
#define CLIENT_CORE_SCHEMA_MANAGER_H

#include <string>
#include <vector>

struct sqlite3;

namespace im {
namespace storage {

enum class SchemaError {
    Ok = 0,
    ExecFailed,      // SQL 执行失败（详见 message）
    BeginFailed,     // 无法开启事务
    VersionFailed,   // 读写 user_version 失败
    UnknownVersion,  // 目标版本没有对应迁移脚本
};

const char* toString(SchemaError e);

class SchemaManager {
public:
    /** 数据库当前版本（PRAGMA user_version）。 */
    static int currentVersion(sqlite3* db);

    /** 最新可用版本（已有迁移脚本的最大版本）。 */
    static int latestVersion();

    /**
     * 把库升级到 targetVersion（默认升到最新）。
     * 每个版本一步、每步单事务；任一步失败即回滚并返回错误，**不删库**。
     */
    static SchemaError migrateTo(sqlite3* db, int targetVersion, std::string* message = nullptr);

    /** 已注册的迁移脚本数量（供测试观察）。 */
    static std::size_t migrationCount();

    /**
     * 数据一致性自检（不写库）：
     *   - 检查必需索引是否存在；
     *   - 检查 message_fts 是否为可用的 fts4 表。
     * 返回是否通过；message 给出失败原因。
     */
    static bool verify(sqlite3* db, std::string* message = nullptr);
};

} // namespace storage
} // namespace im

#endif // CLIENT_CORE_SCHEMA_MANAGER_H
