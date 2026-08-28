#include "handlers/AuthHandler.h"

#include "Database.h"
#include "Log.h"
#include "Presence.h"
#include "Session.h"
#include "auth/TokenService.h"
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

    if (!validMainlandMobile(rq.tel()) || !validSha256Proof(rq.pass())) {
        LoginRs rs;
        rs.set_result(LOGIN_INVALID);
        session->deliver(DEF_PROT_LOGIN_RS, rs.SerializeAsString());
        log("[认证] 拒绝非法登录参数");
        return;
    }

    int userId = 0;
    const int result = m_db.loginUser(
        rq.tel(), rq.pass(), userId);
    if (result != LOGIN_SUCCESS) {
        LoginRs rs;
        rs.set_result(result);
        session->deliver(DEF_PROT_LOGIN_RS, rs.SerializeAsString());
        log("[认证] 登录失败 tel=", rq.tel(), " 结果=", result);
        return;
    }
    if (rq.device_id().empty() || rq.device_id().size() > 128 || session->authenticated()) {
        LoginRs rs;
        rs.set_result(LOGIN_PASSERROR);
        session->deliver(DEF_PROT_LOGIN_RS, rs.SerializeAsString());
        log("[认证] 拒绝非法设备或重复登录 currentUser=", session->userId());
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
    const bool ok = rq.device_id().size() <= 128 &&
        m_tokens.validateAccess(rq.access_token(), "", rq.device_id(),
                                userId, sessionId, expiresAt);
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
    const bool ok = rq.device_id().size() <= 128 && rq.request_id().size() <= 128 &&
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
    const bool ownsSession = tokenValid && session->authenticated() &&
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
