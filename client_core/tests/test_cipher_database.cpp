// CipherDatabase 单元测试（P6-T01）。
//
// 仅在 CLIENT_CORE_WITH_SQLCIPHER=ON 时构建。验证：
//   1. 正确 key：建库 → 写唯一 marker → 关闭 → 重开 → 能读到
//   2. 错误 key：打开失败且返回 NotEncrypted，且**文件未被改动**（fail-close 不删/不覆盖）
//   3. 非法 key 长度：BadKeyLength
//   4. cipher_version 与期望前缀一致
//   5. 磁盘文件不含明文 marker（确实加密）
//
// 说明：桌面侧编译 SQLCipher amalgamation 运行；Android 侧链接 Room 自带 AAR 的
// libsqlcipher.so（版本同为 4.6.1，见 outputs/kernel-round5-encryption-note.md）。
// key 约定：32 字节直接交给 sqlite3_key（口令模式，KDF 派生），与 Room 一致；
// Room↔Native 双向互开由 androidTest RoomNativeFixtureTest 在设备上验证。

#include "client_core/storage/CipherDatabase.h"
#include "client_core/storage/CipherParams.h"

#include <sqlite3.h>

#include <openssl/sha.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
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

const char* kMarker = "P6_CIPHER_MARKER_7C41D9E5";
const char* kPath = "/tmp/test_cipher_database.db";

std::vector<unsigned char> makeKey(unsigned char fill)
{
    return std::vector<unsigned char>(CipherParams::kRawKeyBytes, fill);
}

bool fileContains(const char* path, const std::string& needle)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str().find(needle) != std::string::npos;
}

std::string fileSha256Hex(const char* path)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    const std::string data = ss.str();
    unsigned char digest[SHA256_DIGEST_LENGTH] = {};
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);
    std::string out;
    static const char* hex = "0123456789abcdef";
    for (unsigned char c : digest) {
        out.push_back(hex[(c >> 4) & 0xF]);
        out.push_back(hex[c & 0xF]);
    }
    return out;
}

void removeDb()
{
    std::remove(kPath);
    std::remove((std::string(kPath) + "-wal").c_str());
    std::remove((std::string(kPath) + "-shm").c_str());
}

bool execSql(CipherDatabase& db, const std::string& sql)
{
    // 复用 nativeHandle 直接执行（存储层内部允许；不得跨 JNI 暴露）
    sqlite3* h = db.nativeHandle();
    if (!h) return false;
    char* err = nullptr;
    const int rc = sqlite3_exec(h, sql.c_str(), nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

} // namespace

int main()
{
    std::cout << "=== test_cipher_database ===" << std::endl;
    removeDb();

    // [1] 正确 key 建库 + 写 marker
    {
        CipherDatabase db;
        const CipherError e = db.open(kPath, makeKey(0x11));
        check(e == CipherError::Ok, "正确 key 打开成功");
        if (e != CipherError::Ok) std::cout << "      msg=" << db.lastMessage() << std::endl;
        check(db.isOpen(), "数据库处于打开状态");
        const std::string v = db.cipherVersion();
        std::cout << "      cipher_version=" << v << std::endl;
        check(v.rfind(CipherParams::kExpectedCipherVersion, 0) == 0,
              "cipher_version 前缀与期望一致");
        check(execSql(db, std::string("CREATE TABLE t(id INTEGER PRIMARY KEY, body TEXT)")),
              "建表成功");
        check(execSql(db, std::string("INSERT INTO t(body) VALUES('") + kMarker + "')"),
              "写入 marker 成功");
        db.close();
    }

    // [3] 磁盘文件不含明文 marker（确实加密）
    check(!fileContains(kPath, kMarker), "磁盘文件不含明文 marker（已加密）");

    // [4] 正确 key 重开可读取
    {
        CipherDatabase db;
        check(db.open(kPath, makeKey(0x11)) == CipherError::Ok, "正确 key 重开成功");
        check(execSql(db, std::string("SELECT count(*) FROM t")), "重开后可查询");
    }

    // [5] 错误 key：失败 + 文件未被改动（fail-close，不删不覆盖）
    {
        const std::string before = fileSha256Hex(kPath);
        CipherDatabase db;
        const CipherError e = db.open(kPath, makeKey(0x99));
        check(e == CipherError::NotEncrypted, "错误 key 返回 NotEncrypted");
        check(!db.isOpen(), "错误 key 后连接已关闭");
        check(fileSha256Hex(kPath) == before, "错误 key 后文件摘要不变（未被覆盖/删除）");
        check(std::string(db.lastMessage()).find(kMarker) == std::string::npos,
              "错误消息不含敏感内容");
    }

    // [6] 非法 key 长度
    {
        CipherDatabase db;
        check(db.open(kPath, std::vector<unsigned char>(16, 0x11)) == CipherError::BadKeyLength,
              "16 字节 key → BadKeyLength");
        check(db.open(kPath, std::vector<unsigned char>()) == CipherError::BadKeyLength,
              "空 key → BadKeyLength");
    }

    removeDb();
    if (g_failures == 0) {
        std::cout << "test_cipher_database PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_cipher_database FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
