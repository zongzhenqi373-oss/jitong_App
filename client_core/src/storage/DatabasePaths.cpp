#include "client_core/storage/DatabasePaths.h"

#include <cerrno>
#include <cstdio>

#include "client_core/storage/ProcessLock.h"

namespace im {
namespace storage {

const char* nativeDatabaseDirName() { return "native_db"; }

bool isValidOwnerId(std::int64_t ownerId) { return ownerId > 0; }

std::string nativeDatabaseFileName(std::int64_t ownerId)
{
    return "account_" + std::to_string(ownerId) + ".db";
}

bool containsParentTraversal(const std::string& path)
{
    // 逐段检查：任何一段 == ".." 都视为越界
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t sep = path.find('/', start);
        const std::size_t end = (sep == std::string::npos) ? path.size() : sep;
        if (end > start && path.compare(start, end - start, "..") == 0) return true;
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return false;
}

bool resolveNativeDatabasePath(const std::string& filesDir, std::int64_t ownerId,
                               std::string& outPath, std::string& outError)
{
    outPath.clear();
    outError.clear();

    if (filesDir.empty()) {
        outError = "filesDir 为空";
        return false;
    }
    // F14：filesDir 必须是 Android Context 提供的内部 filesDir；此处再拒绝相对路径/越界/NUL。
    // 路径中空格本身合法，不能误判；JNI/SDK 层不得把用户输入作为 filesDir 传入。
    if (filesDir[0] != '/') {
        outError = "filesDir 必须是绝对路径";
        return false;
    }
    if (filesDir.find('\0') != std::string::npos) {
        outError = "filesDir 含 NUL 字符";
        return false;
    }
    if (containsParentTraversal(filesDir)) {
        outError = "filesDir 含越界路径";
        return false;
    }
    if (!isValidOwnerId(ownerId)) {
        outError = "ownerId 非法（必须为正整数）";
        return false;
    }

    // 去掉结尾多余的 '/'，保持拼装稳定
    std::string base = filesDir;
    while (base.size() > 1 && base.back() == '/') base.pop_back();

    std::string path = base;
    path += '/';
    path += nativeDatabaseDirName();
    path += '/';
    path += nativeDatabaseFileName(ownerId);

    if (containsParentTraversal(path)) {
        outError = "拼装结果含越界路径";
        return false;
    }
    outPath = path;
    return true;
}

bool clearShadowDatabase(const std::string& filesDir, std::int64_t ownerId, std::string* err)
{
    std::string path;
    std::string pathErr;
    if (!resolveNativeDatabasePath(filesDir, ownerId, path, pathErr)) {
        if (err) *err = pathErr;
        return false;
    }
    // F10/R04：先取得跨进程排他锁；持锁期间删 DB/WAL/SHM，绝不 unlink .lock。
    // .lock 作为 flock 互斥锚点永久保留——若删除，持旧 inode 锁的进程与打开新 inode 的
    // 进程会同时成功，破坏第二进程互斥。
    ProcessLock lock;
    std::string lockErr;
    if (!lock.tryAcquire(path, &lockErr)) {
        if (err) *err = "获取进程锁失败，无法清理影子库：" + lockErr;
        return false;
    }
    int failures = 0;
    auto removeQuiet = [&](const std::string& p) {
        if (::remove(p.c_str()) != 0 && errno != ENOENT) ++failures; // 仅 ENOENT 视为成功
    };
    removeQuiet(path);
    removeQuiet(path + "-wal");
    removeQuiet(path + "-shm");
    lock.release();
    if (failures > 0) {
        if (err) *err = "清理影子库有 " + std::to_string(failures) + " 个文件删除失败";
        return false;
    }
    return true;
}

} // namespace storage
} // namespace im
