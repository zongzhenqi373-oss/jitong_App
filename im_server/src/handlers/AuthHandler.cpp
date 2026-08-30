#include "handlers/AuthHandler.h"

#include "db/Database.h"
#include "common/Log.h"
#include "session/Presence.h"
#include "session/Session.h"
#include "auth/TokenService.h"
#include "auth/DeviceProof.h"
#include "client_core/Protocol.h"
#include "handlers/HandlerUtils.h"
#include "im.pb.h"

#include <vector>
#include <algorithm>
#include <cctype>

namespace imsrv {
using namespace im::proto;

namespace {
bool validMainlandMobile(const std::string& tel)
{
    return tel.size() == 11 && tel[0] == '1' && tel[1] >= '3' && tel[1] <= '9' &&
        std::all_of(tel.begin(), tel.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

bool validSha256Proof(const std::string& proof)
{
    return proof.size() == 64 && std::all_of(proof.begin(), proof.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0 || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    });
}

std::string maskedAccount(const std::string& tel)
{
    if (tel.size() < 4) return "invalid";
    return "***" + tel.substr(tel.size() - 4);
}

deviceproof::Bytes bytesOf(const std::string& value)
{
    return deviceproof::Bytes(value.begin(), value.end());
}

bool verifyDevice(const std::shared_ptr<Session>& session, const std::string& operation,
                  const std::string& deviceId, const std::string& credential,
                  const deviceproof::Bytes& publicKey, const std::string& signature,
                  bool bindPublicKey)
{
    const auto proof = deviceproof::message(operation, session->appSessionId(), deviceId,
                                             credential, bindPublicKey ? publicKey : deviceproof::Bytes{});
    return deviceproof::verifyP256(publicKey, proof, bytesOf(signature));
}
} // namespace

AuthHandler::AuthHandler(Database& db, Presence& presence, TokenService& tokens)
    : m_db(db), m_presence(presence), m_tokens(tokens)
{
}

void AuthHandler::onRegister(const std::shared_ptr<Session>& session, const std::string& payload)
{
    RegisterRq rq;
    if (!handlers::parsePayload(payload, rq)) return;
    if (rq.nick().empty() || rq.nick().size() >= USER_NICK_LEN ||
        !validMainlandMobile(rq.tel()) || !validSha256Proof(rq.pass())) {
        RegisterRs rs;
        rs.set_result(REGISTER_INVALID);
        session->deliver(DEF_PROT_REGISTER_RS, rs.SerializeAsString());
        log("[认证] 拒绝非法注册参数");
        return;
    }
    const int result = m_db.registerUser(
        rq.nick(), rq.tel(), rq.pass());
    RegisterRs rs;
    rs.set_result(result);
    session->deliver(DEF_PROT_REGISTER_RS, rs.SerializeAsString());
    log("[认证] 注册 nick=", rq.nick(), " 结果=", result);
}

void AuthHandler::activateSession(const std::shared_ptr<Session>& session, int userId)
{
    if (auto old = m_presence.replace(userId, session); old && old != session) {
        old->deliver(DEF_PROT_KICKED_OFFLINE, "");
        old->closeAfterWrite();
        log("[认证] 用户 id=", userId, " 在别处登录，旧连接被踢");
    }

    handlers::sendFriendInfo(m_db, session, userId, STATUS_ONLINE);
    for (const auto& friendRecord : m_db.getFriends(userId)) {
        const bool online = m_presence.isOnline(friendRecord.id);
        handlers::sendFriendInfo(m_db, session, friendRecord.id,
                                 online ? STATUS_ONLINE : STATUS_OFFLINE);
        if (auto friendSession = m_presence.get(friendRecord.id)) {
            handlers::sendFriendInfo(m_db, friendSession, userId, STATUS_ONLINE);
        }
    }

    const auto undelivered = m_db.pullUndelivered(userId);
    std::vector<std::string> deliveredIds;
    deliveredIds.reserve(undelivered.size());
    for (const auto& message : undelivered) {
        ChatInfoRq out;
        handlers::fillChatInfo(out, message);
        session->deliver(DEF_PROT_CHAT_INFO_RQ, out.SerializeAsString());
        deliveredIds.push_back(message.msgId);
        if (auto sender = m_presence.get(message.senderId)) {
            ChatInfoRs receipt;
            receipt.set_myid(userId);
            receipt.set_friid(message.senderId);
            receipt.set_result(CHAT_RESULT_SUCC);
            receipt.set_msg_id(message.msgId);
            receipt.set_seq(message.seq);
            sender->deliver(DEF_PROT_CHAT_INFO_RS, receipt.SerializeAsString());
        }
    }
    m_db.markDelivered(deliveredIds);
    if (!undelivered.empty()) {
        log("[认证] 离线消息补发 id=", userId, " 条数=", undelivered.size());
    }
}

void AuthHandler::onLogin(const std::shared_ptr<Session>& session, const std::string& payload)
{
    LoginRq rq;
    if (!handlers::parsePayload(payload, rq)) return;

    const std::string& ip = session->peerAddress();
    auto reject = [&session](int result) {
        LoginRs rs;
        rs.set_result(result);
        session->deliver(DEF_PROT_LOGIN_RS, rs.SerializeAsString());
    };

    // IP 已处于封禁窗口时，不继续解析凭证或执行昂贵的 Argon2id。
    if (!m_loginLimiter.allow(ip, "")) {
        reject(LOGIN_RATE_LIMITED);
        log("[认证] 密码登录触发IP限流 ip=", ip);
        return;
    }

    if (!validMainlandMobile(rq.tel()) || !validSha256Proof(rq.pass()) ||
        rq.device_id().empty() || rq.device_id().size() > 128 || session->authenticated() ||
        rq.device_public_key().empty() || rq.device_signature().empty()) {
        m_loginLimiter.recordFailure(ip, "");
        reject(LOGIN_INVALID);
        log("[认证] 拒绝非法登录参数");
        return;
    }

    const auto suppliedPublicKey = bytesOf(rq.device_public_key());
    if (!verifyDevice(session, "password-login", rq.device_id(),
                      rq.tel() + std::string(1, '\0') + rq.pass(), suppliedPublicKey,
                      rq.device_signature(), true)) {
        m_loginLimiter.recordFailure(ip, rq.tel());
        reject(LOGIN_PASSERROR);
        log("[认证] 设备签名无效 account=", maskedAccount(rq.tel()));
        return;
    }

    if (!m_loginLimiter.allow(ip, rq.tel())) {
        reject(LOGIN_RATE_LIMITED);
        log("[认证] 密码登录触发账号/IP限流 ip=", ip,
            " account=", maskedAccount(rq.tel()));
        return;
    }

    int userId = 0;
    const int result = m_db.loginUser(
        rq.tel(), rq.pass(), userId);
    if (result != LOGIN_SUCCESS) {
        m_loginLimiter.recordFailure(ip, rq.tel());
        reject(result);
        log("[认证] 登录失败 account=", maskedAccount(rq.tel()), " 结果=", result);
        return;
    }
    if (!m_db.bindDevicePublicKey(userId, rq.device_id(), suppliedPublicKey)) {
        reject(LOGIN_PASSERROR);
        log("[认证] device_id已绑定其他设备密钥 id=", userId);
        return;
    }

    TokenPair tokens;
    try {
        tokens = m_tokens.issue(userId, rq.device_id());
    } catch (const std::exception& error) {
        log("[认证] Token 签发失败 id=", userId, " error=", error.what());
        LoginRs rs;
        rs.set_result(LOGIN_PASSERROR);
        session->deliver(DEF_PROT_LOGIN_RS, rs.SerializeAsString());
        return;
    }

    session->bindAuth(userId, tokens.sessionId, rq.device_id(), tokens.accessExpiresAt);
    LoginRs rs;
    rs.set_userid(userId);
    rs.set_result(LOGIN_SUCCESS);
    rs.set_access_token(tokens.accessToken);
    rs.set_refresh_token(tokens.refreshToken);
    rs.set_access_token_expire_at(tokens.accessExpiresAt);
    rs.set_refresh_token_expire_at(tokens.refreshExpiresAt);
    rs.set_session_id(tokens.sessionId);
    session->deliver(DEF_PROT_LOGIN_RS, rs.SerializeAsString());
    m_loginLimiter.recordSuccess(ip, rq.tel());
    activateSession(session, userId);
    log("[认证] 登录成功 id=", userId);
}

void AuthHandler::onTokenLogin(const std::shared_ptr<Session>& session, const std::string& payload)
{
    TokenLoginRq rq;
    if (!handlers::parsePayload(payload, rq)) return;
    if (session->authenticated()) {
        TokenLoginRs rs;
        rs.set_result(LOGIN_PASSERROR);
        session->deliver(DEF_PROT_TOKEN_LOGIN_RS, rs.SerializeAsString());
        return;
    }

    int userId = 0;
    std::string sessionId;
    std::int64_t expiresAt = 0;
    bool ok = rq.device_id().size() <= 128 && !rq.device_signature().empty() &&
        m_tokens.validateAccess(rq.access_token(), "", rq.device_id(),
                                userId, sessionId, expiresAt);
    deviceproof::Bytes devicePublicKey;
    ok = ok && m_db.getDevicePublicKey(userId, rq.device_id(), devicePublicKey) &&
        verifyDevice(session, "token-login", rq.device_id(), rq.access_token(),
                     devicePublicKey, rq.device_signature(), false);
    TokenLoginRs rs;
    rs.set_result(ok ? LOGIN_SUCCESS : LOGIN_PASSERROR);
    if (ok) {
        rs.set_userid(userId);
        rs.set_access_token_expire_at(expiresAt);
        session->bindAuth(userId, sessionId, rq.device_id(), expiresAt);
    }
    session->deliver(DEF_PROT_TOKEN_LOGIN_RS, rs.SerializeAsString());
    if (ok) activateSession(session, userId);
}

void AuthHandler::onTokenRefresh(const std::shared_ptr<Session>& session, const std::string& payload)
{
    RefreshTokenRq rq;
    if (!handlers::parsePayload(payload, rq)) return;
    TokenPair tokens;
    int revokedUserId = 0;
    Database::AuthSessionRecord refreshRecord;
    deviceproof::Bytes refreshPublicKey;
    const bool deviceProofOk = rq.device_id().size() <= 128 && rq.request_id().size() <= 128 &&
        !rq.device_signature().empty() &&
        m_db.findByRefreshHash(TokenService::tokenHash(rq.refresh_token()), refreshRecord) &&
        !refreshRecord.revoked && refreshRecord.deviceId == rq.device_id() &&
        m_db.getDevicePublicKey(refreshRecord.userId, rq.device_id(), refreshPublicKey) &&
        verifyDevice(session, "token-refresh", rq.device_id(),
                     rq.refresh_token() + std::string(1, '\0') + rq.request_id(),
                     refreshPublicKey, rq.device_signature(), false);
    const bool ok = deviceProofOk &&
        m_tokens.rotateRefresh(rq.refresh_token(), rq.device_id(), rq.request_id(),
                               tokens, revokedUserId);
    if (revokedUserId > 0) {
        if (auto live = m_presence.get(revokedUserId)) {
            live->deliver(DEF_PROT_KICKED_OFFLINE, "");
            live->closeAfterWrite();
            log("[认证] refresh_token 重放，家族已吊销 uid=", revokedUserId);
        }
    }

    RefreshTokenRs rs;
    rs.set_result(ok ? 0 : 1);
    if (ok) {
        rs.set_access_token(tokens.accessToken);
        rs.set_refresh_token(tokens.refreshToken);
        rs.set_access_token_expire_at(tokens.accessExpiresAt);
        rs.set_refresh_token_expire_at(tokens.refreshExpiresAt);
        rs.set_session_id(tokens.sessionId);
        if (session->authenticated()) {
            session->bindAuth(session->userId(), tokens.sessionId,
                              rq.device_id(), tokens.accessExpiresAt);
        }
    }
    session->deliver(DEF_PROT_TOKEN_REFRESH_RS, rs.SerializeAsString());
}

void AuthHandler::onLogout(const std::shared_ptr<Session>& session, const std::string& payload)
{
    LogoutRq rq;
    if (!handlers::parsePayload(payload, rq)) return;
    Database::AuthSessionRecord record;
    const bool tokenValid = !rq.refresh_token().empty() &&
        m_db.findByRefreshHash(TokenService::tokenHash(rq.refresh_token()), record) &&
        !record.revoked && record.deviceId == rq.device_id();
    deviceproof::Bytes logoutPublicKey;
    const bool proofValid = tokenValid &&
        m_db.getDevicePublicKey(record.userId, rq.device_id(), logoutPublicKey) &&
        verifyDevice(session, "logout", rq.device_id(),
            rq.refresh_token() + std::string(1, '\0') + (rq.logout_all_devices() ? "1" : "0"),
            logoutPublicKey, rq.device_signature(), false);
    const bool ownsSession = proofValid && session->authenticated() &&
        session->userId() == record.userId && session->deviceId() == record.deviceId &&
        session->authSessionId() == record.sessionId;
    if (ownsSession) {
        if (rq.logout_all_devices()) m_tokens.revokeAllForUser(record.userId);
        else m_tokens.revokeSession(record.sessionId);
    }
    LogoutRs rs;
    rs.set_result(ownsSession ? 0 : 1);
    session->deliver(DEF_PROT_LOGOUT_RS, rs.SerializeAsString());
    if (ownsSession) session->closeAfterWrite();
    log("[认证] 登出 id=", session->userId(), " 结果=", ownsSession);
}

} // namespace imsrv
