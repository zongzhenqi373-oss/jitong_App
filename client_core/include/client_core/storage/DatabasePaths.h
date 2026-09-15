// Native 数据库路径与 ownerId 校验（P6-T02 账号隔离）。
//
// 只做**纯字符串与数值校验**，不触碰文件系统：便于在桌面单测里覆盖全部越界场景。
//
// 安全约束（对应 24.4）：
//   - 库文件只能落在 filesDir/native_db/ 下，文件名固定为 account_<ownerId>.db；
//   - ownerId 必须是正整数（服务端分配的账号 id），负数/0/超范围一律拒绝；
//   - 任何包含 ".." 的路径（无论是 filesDir 还是拼装结果）一律拒绝，**在创建文件之前**拒绝；
//   - 不因 ownerId 不同而复用同一文件，不同账号物理隔离。

#ifndef CLIENT_CORE_DATABASE_PATHS_H
#define CLIENT_CORE_DATABASE_PATHS_H

#include <cstdint>
#include <string>

namespace im {
namespace storage {

/** ownerId 是否合法（正整数）。 */
bool isValidOwnerId(std::int64_t ownerId);

/** 库文件名（不含目录）：account_<ownerId>.db */
std::string nativeDatabaseFileName(std::int64_t ownerId);

/** 目录名（相对 filesDir）：native_db */
const char* nativeDatabaseDirName();

/**
 * 拼装并校验库文件路径。
 * @param filesDir 平台提供的 filesDir 绝对路径（不得为空、不得含 ".."）
 * @param ownerId  账号 id
 * @param outPath  成功时输出完整路径
 * @param outError 失败时输出原因（不含任何敏感信息）
 * @return 校验通过返回 true
 */
bool resolveNativeDatabasePath(const std::string& filesDir, std::int64_t ownerId,
                               std::string& outPath, std::string& outError);

/** 路径是否含有 ".." 段（用于拒绝越界）。 */
bool containsParentTraversal(const std::string& path);

/**
 * 清理影子库文件（迁移回滚/关闭时用）。
 *
 * 只删除 `filesDir/native_db/account_<ownerId>.db` 及其 `-wal`/`-shm`/`.lock` 旁路文件，
 * **绝不**触碰生产 Room 库（`jitong_<ownerId>.db`）或媒体文件。越界/非法 ownerId 直接拒绝。
 *
 * @return 校验失败返回 false；文件不存在或删除失败仍返回 true（清理语义幂等，不因此报错）
 */
bool clearShadowDatabase(const std::string& filesDir, std::int64_t ownerId, std::string* err);

} // namespace storage
} // namespace im

#endif // CLIENT_CORE_DATABASE_PATHS_H
