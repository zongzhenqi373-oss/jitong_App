#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace imsrv {

struct TokenPair {
    std::string sessionId;
    std::string familyId;
    std::string accessToken;
    std::string refreshToken;
    std::int64_t accessExpiresAt = 0;
    std::int64_t refreshExpiresAt = 0;
};

class Database;

class TokenService {
public:
    explicit TokenService(Database& db);

    TokenPair issue(
        int userId,
        const std::string& deviceId
    );

    bool validateAccess(
        const std::string& token,
        const std::string& sessionId,
        const std::string& deviceId,
        int& outUserId,
        std::string& outSessionId,
        std::int64_t& outExpiresAt
    );

    // outRevokedUserId：仅当本次调用因检测到 refresh_token 重放而吊销了整个 token 家族时被置为
    // 该家族所属的 userId（否则保持调用方传入的初始值不变），供调用方立即踢掉这个用户当前在线的连接
    bool rotateRefresh(
        const std::string& refreshToken,
        const std::string& deviceId,
        const std::string& requestId,
        TokenPair& outPair,
        int& outRevokedUserId
    );

    void revokeSession(
        const std::string& sessionId
    );

    void revokeAllForUser(int userId);

    static std::string randomToken();
    static std::string tokenHash(
        const std::string& token
    );

private:
    struct RefreshResultCacheEntry {
        std::string refreshHash;
        std::string deviceId;
        TokenPair pair;
        std::int64_t expiresAt = 0;
    };

    Database& m_db;
    std::mutex m_refreshMtx;
    std::unordered_map<std::string, RefreshResultCacheEntry> m_refreshResults;
};

}
