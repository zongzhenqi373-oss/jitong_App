#include "TokenService.h"
#include "Database.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <ctime>
#include <stdexcept>
#include <vector>

namespace imsrv {

namespace {

std::string toHex(
    const unsigned char* data,
    std::size_t len
) {
    static const char* hex =
        "0123456789abcdef";

    std::string out;
    out.reserve(len * 2);

    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(hex[data[i] >> 4]);
        out.push_back(hex[data[i] & 0x0F]);
    }

    return out;
}

}

TokenService::TokenService(Database& db)
    : m_db(db)
{
}

std::string TokenService::randomToken()
{
    unsigned char bytes[32];

    if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
        throw std::runtime_error(
            "RAND_bytes failed"
        );
    }

    // 为了实现简单先返回64字符hex。
    // 后续可替换成Base64URL。
    return toHex(bytes, sizeof(bytes));
}

std::string TokenService::tokenHash(
    const std::string& token
) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        throw std::runtime_error(
            "EVP_MD_CTX_new failed"
        );
    }

    const bool ok =
        EVP_DigestInit_ex(
            ctx,
            EVP_sha256(),
            nullptr
        ) == 1 &&
        EVP_DigestUpdate(
            ctx,
            token.data(),
            token.size()
        ) == 1 &&
        EVP_DigestFinal_ex(
            ctx,
            digest,
            &digestLen
        ) == 1;

    EVP_MD_CTX_free(ctx);

    if (!ok) {
        throw std::runtime_error(
            "SHA-256 failed"
        );
    }

    return toHex(digest, digestLen);
}

TokenPair TokenService::issue(
    int userId,
    const std::string& deviceId
) {
    const auto now =
        static_cast<std::int64_t>(
            std::time(nullptr)
        );

    TokenPair pair;
    pair.sessionId = randomToken();
    pair.familyId = randomToken();
    pair.accessToken = randomToken();
    pair.refreshToken = randomToken();
    pair.accessExpiresAt = now + 5 * 60;
    pair.refreshExpiresAt =
        now + 2LL * 24 * 60 * 60;

    const bool saved = m_db.createAuthSession(
        pair.sessionId,
        pair.familyId,
        userId,
        deviceId,
        tokenHash(pair.accessToken),
        pair.accessExpiresAt,
        tokenHash(pair.refreshToken),
        pair.refreshExpiresAt
    );

    if (!saved) {
        throw std::runtime_error(
            "保存Token Session失败"
        );
    }

    return pair;
}

bool TokenService::validateAccess(
    const std::string& token, const std::string& sessionId,
    const std::string& deviceId, int& outUserId,
    std::string& outSessionId, std::int64_t& outExpiresAt)
{
    if (token.empty() || deviceId.empty()) return false;
    Database::AuthSessionRecord record;
    if (!m_db.findByAccessHash(tokenHash(token), record)) return false;
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    if (record.revoked || record.accessExpiresAt <= now ||
        record.deviceId != deviceId ||
        (!sessionId.empty() && record.sessionId != sessionId)) return false;
    outUserId = record.userId;
    outSessionId = record.sessionId;
    outExpiresAt = record.accessExpiresAt;
    return true;
}

bool TokenService::rotateRefresh(
    const std::string& refreshToken, const std::string& deviceId,
    const std::string& requestId, TokenPair& outPair, int& outRevokedUserId)
{
    if (refreshToken.empty() || deviceId.empty() || requestId.empty()) return false;
    const std::string oldHash = tokenHash(refreshToken);
    const auto now = static_cast<std::int64_t>(std::time(nullptr));

    // 单飞 + 短期幂等：响应丢失后，同一 requestId 可拿回第一次生成的同一组 Token。
    std::lock_guard<std::mutex> lock(m_refreshMtx);
    for (auto it = m_refreshResults.begin(); it != m_refreshResults.end();) {
        if (it->second.expiresAt <= now) it = m_refreshResults.erase(it);
        else ++it;
    }
    if (auto it = m_refreshResults.find(requestId); it != m_refreshResults.end()) {
        if (it->second.refreshHash != oldHash || it->second.deviceId != deviceId) return false;
        outPair = it->second.pair;
        return true;
    }

    Database::AuthSessionRecord old;
    if (!m_db.findByRefreshHash(oldHash, old)) {
        std::string familyId;
        if (m_db.wasRefreshTokenUsed(oldHash, familyId) && !familyId.empty()) {
            const int uid = m_db.revokeTokenFamily(familyId);
            if (uid > 0) outRevokedUserId = uid;
        }
        return false;
    }
    if (old.revoked || old.refreshExpiresAt <= now || old.deviceId != deviceId) return false;

    TokenPair next;
    next.sessionId = old.sessionId;
    next.familyId = old.familyId;
    next.accessToken = randomToken();
    next.refreshToken = randomToken();
    next.accessExpiresAt = now + 15 * 60;
    next.refreshExpiresAt = now + 30LL * 24 * 60 * 60;
    if (!m_db.rotateRefreshToken(
            old, oldHash, tokenHash(next.accessToken), next.accessExpiresAt,
            tokenHash(next.refreshToken), next.refreshExpiresAt)) {
        std::string familyId;
        if (m_db.wasRefreshTokenUsed(oldHash, familyId) && !familyId.empty()) {
            const int uid = m_db.revokeTokenFamily(familyId);
            if (uid > 0) outRevokedUserId = uid;
        }
        return false;
    }
    outPair = std::move(next);
    if (m_refreshResults.size() >= 1024) m_refreshResults.erase(m_refreshResults.begin());
    m_refreshResults.emplace(requestId, RefreshResultCacheEntry{
        oldHash, deviceId, outPair, now + 120
    });
    return true;
}

void TokenService::revokeSession(const std::string& sessionId)
{
    if (!sessionId.empty()) m_db.revokeSession(sessionId);
}

void TokenService::revokeAllForUser(int userId)
{
    if (userId > 0) m_db.revokeAllUserSessions(userId);
}

}    
