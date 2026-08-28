#include "Database.h"
#include "auth/TokenService.h"
#include "sha256.h"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <sqlite3.h>
#include <string>

namespace {

std::string queryText(const char* dbPath, const char* sql)
{
    sqlite3* db = nullptr;
    sqlite3_stmt* statement = nullptr;
    assert(sqlite3_open(dbPath, &db) == SQLITE_OK);
    assert(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    const auto* value = sqlite3_column_text(statement, 0);
    const std::string result = value ? reinterpret_cast<const char*>(value) : "";
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return result;
}

void insertLegacyUser(const char* dbPath)
{
    sqlite3* db = nullptr;
    assert(sqlite3_open(dbPath, &db) == SQLITE_OK);
    const std::string passHash = im::sha256Hex("legacy-password");
    const std::string stored = im::sha256Hex("legacy-salt" + passHash);
    sqlite3_stmt* statement = nullptr;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO t_user(name,tel,passwd,salt,password_algo) VALUES(?,?,?,?,?);",
        -1, &statement, nullptr) == SQLITE_OK);
    sqlite3_bind_text(statement, 1, "legacy-user", -1, SQLITE_STATIC);
    sqlite3_bind_text(statement, 2, "13900000000", -1, SQLITE_STATIC);
    sqlite3_bind_text(statement, 3, stored.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 4, "legacy-salt", -1, SQLITE_STATIC);
    sqlite3_bind_text(statement, 5, "legacy_sha256", -1, SQLITE_STATIC);
    assert(sqlite3_step(statement) == SQLITE_DONE);
    sqlite3_finalize(statement);
    sqlite3_close(db);
}

} // namespace

int main()
{
    const char* dbPath = "/tmp/im_server_token_test.db";
    std::remove(dbPath);
    std::remove("/tmp/im_server_token_test.db-wal");
    std::remove("/tmp/im_server_token_test.db-shm");

    {
        imsrv::Database db;
        assert(db.open(dbPath, 2));
        db.seedIfEmpty();

        int userId = 0;
        assert(db.loginUser("13800000001", im::sha256Hex("123456"), userId) == 0);
        assert(userId > 0);
        assert(queryText(dbPath,
            "SELECT password_algo FROM t_user WHERE tel='13800000001';") == "argon2id");
        assert(queryText(dbPath,
            "SELECT substr(passwd,1,10) FROM t_user WHERE tel='13800000001';") == "$argon2id$");

        insertLegacyUser(dbPath);
        int legacyUserId = 0;
        assert(db.loginUser(
            "13900000000", im::sha256Hex("legacy-password"), legacyUserId) == 0);
        assert(queryText(dbPath,
            "SELECT password_algo FROM t_user WHERE tel='13900000000';") == "argon2id");
        assert(queryText(dbPath,
            "SELECT substr(passwd,1,10) FROM t_user WHERE tel='13900000000';") == "$argon2id$");

        imsrv::TokenService tokens(db);
        const std::string deviceId = "test-device-1";
        auto first = tokens.issue(userId, deviceId);

        int validatedUser = 0;
        std::string sessionId;
        std::int64_t expiresAt = 0;
        assert(tokens.validateAccess(
            first.accessToken, first.sessionId, deviceId,
            validatedUser, sessionId, expiresAt));
        assert(validatedUser == userId);
        assert(sessionId == first.sessionId);
        assert(!tokens.validateAccess(
            first.accessToken, first.sessionId, "wrong-device",
            validatedUser, sessionId, expiresAt));

        imsrv::TokenPair rotated;
        int revokedUserId1 = 0;
        assert(tokens.rotateRefresh(first.refreshToken, deviceId, "request-1", rotated, revokedUserId1));
        assert(rotated.sessionId == first.sessionId);
        assert(!tokens.validateAccess(
            first.accessToken, first.sessionId, deviceId,
            validatedUser, sessionId, expiresAt));
        assert(tokens.validateAccess(
            rotated.accessToken, rotated.sessionId, deviceId,
            validatedUser, sessionId, expiresAt));

        // 首次刷新响应丢失时，客户端会携带同一 requestId 重试。
        // 服务端必须返回第一次生成的同一组 Token，不能把它判成重放攻击。
        imsrv::TokenPair retried;
        int revokedUserId2 = 0;
        assert(tokens.rotateRefresh(first.refreshToken, deviceId, "request-1", retried, revokedUserId2));
        assert(retried.accessToken == rotated.accessToken);
        assert(retried.refreshToken == rotated.refreshToken);
        assert(tokens.validateAccess(
            retried.accessToken, retried.sessionId, deviceId,
            validatedUser, sessionId, expiresAt));

        // 同一个旧 refresh token 换成不同 requestId，才视为可疑重放并撤销 family。
        imsrv::TokenPair ignored;
        int revokedUserId3 = 0;
        assert(!tokens.rotateRefresh(first.refreshToken, deviceId, "request-2", ignored, revokedUserId3));
        assert(revokedUserId3 == userId); // 重放检测必须能把被吊销 family 归属的 userId 报出来，供调用方立刻踢下线
        assert(!tokens.validateAccess(
            rotated.accessToken, rotated.sessionId, deviceId,
            validatedUser, sessionId, expiresAt));

        auto second = tokens.issue(userId, deviceId);
        tokens.revokeSession(second.sessionId);
        assert(!tokens.validateAccess(
            second.accessToken, second.sessionId, deviceId,
            validatedUser, sessionId, expiresAt));

        auto third = tokens.issue(userId, deviceId);
        tokens.revokeAllForUser(userId);
        assert(!tokens.validateAccess(
            third.accessToken, third.sessionId, deviceId,
            validatedUser, sessionId, expiresAt));
    }

    std::remove(dbPath);
    std::remove("/tmp/im_server_token_test.db-wal");
    std::remove("/tmp/im_server_token_test.db-shm");
    std::cout << "token service tests passed\n";
    return 0;
}
