#include "handlers/FriendHandler.h"

#include <string>

#include "Database.h"
#include "Log.h"
#include "Presence.h"
#include "Session.h"
#include "client_core/Protocol.h"
#include "handlers/HandlerUtils.h"
#include "im.pb.h"

namespace imsrv {
using namespace im::proto;

FriendHandler::FriendHandler(Database& db, Presence& presence)
    : m_db(db), m_presence(presence)
{
}

void FriendHandler::onAddFriend(const std::shared_ptr<Session>& session,
                                const std::string& payload)
{
    AddFriendRq rq;
    if (!handlers::parsePayload(payload, rq)) return;
    const int userId = session->userId();
    if (userId <= 0) return;
    rq.set_myid(userId);

    const int friendId = m_db.getUserIdByNick(
        utf8Truncate(rq.frinick(), USER_NICK_LEN - 1));
    AddFriendRs error;
    error.set_mynick(rq.frinick());
    if (friendId == 0) {
        error.set_result(ADD_FRIEND_NOTEXIT);
    } else if (userId == friendId) {
        error.set_result(ADD_FRIEND_SELF);
    } else if (m_db.isFriend(userId, friendId)) {
        error.set_result(ADD_FRIEND_ALREADY);
    } else {
        if (!m_db.createFriendRequest(userId, friendId)) {
            error.set_result(ADD_FRIEND_DB_ERROR);
        } else {
            error.set_result(ADD_FRIEND_PENDING);
            error.set_myid(friendId);
            UserRecord targetUser;
            if (m_db.getUser(friendId, targetUser)) error.set_mynick(targetUser.nick);
            // 在线时额外推送提醒；真正的待处理状态仍以数据库列表为准。
            if (auto target = m_presence.get(friendId)) {
                target->deliver(DEF_PROT_ADD_FRIEND_RQ, rq.SerializeAsString());
            }
            log("[好友] 申请已持久化 user=", userId, " target=", friendId);
        }
    }
    session->deliver(DEF_PROT_ADD_FRIEND_RS, error.SerializeAsString());
}

void FriendHandler::onAddFriendReply(const std::shared_ptr<Session>& session,
                                     const std::string& payload)
{
    AddFriendRs rs;
    if (!handlers::parsePayload(payload, rs)) return;
    const int userId = session->userId();
    if (userId <= 0) return;
    rs.set_myid(userId);

    const bool accepted = rs.result() == ADD_FRIEND_AGREE;
    if (rs.result() != ADD_FRIEND_AGREE && rs.result() != ADD_FRIEND_REJECT) return;
    if (!m_db.resolveFriendRequest(rs.destid(), userId, accepted)) {
        log("[好友] 拒绝无待处理申请的回复 requester=", rs.destid(), " target=", userId);
        return;
    }
    if (accepted) {
        handlers::sendFriendInfo(m_db, session, userId, STATUS_ONLINE);
        for (const auto& friendRecord : m_db.getFriends(userId)) {
            handlers::sendFriendInfo(m_db, session, friendRecord.id,
                m_presence.isOnline(friendRecord.id) ? STATUS_ONLINE : STATUS_OFFLINE);
        }
        if (auto requester = m_presence.get(rs.destid())) {
            handlers::sendFriendInfo(m_db, requester, rs.destid(), STATUS_ONLINE);
            for (const auto& friendRecord : m_db.getFriends(rs.destid())) {
                handlers::sendFriendInfo(m_db, requester, friendRecord.id,
                    m_presence.isOnline(friendRecord.id) ? STATUS_ONLINE : STATUS_OFFLINE);
            }
        }
    }
    if (auto requester = m_presence.get(rs.destid())) {
        requester->deliver(DEF_PROT_ADD_FRIEND_RS, rs.SerializeAsString());
        log("[好友] 回复 requester=", rs.destid(), " result=", rs.result());
    }
}

void FriendHandler::onFriendRequestList(const std::shared_ptr<Session>& session,
                                        const std::string& payload)
{
    FriendRequestListRq request;
    if (!handlers::parsePayload(payload, request)) return;
    const int userId = session->userId();
    if (userId <= 0) return;
    FriendRequestListRs response;
    for (const auto& row : m_db.pendingFriendRequests(userId)) {
        auto* item = response.add_requests();
        item->set_requester_id(row.requesterId);
        item->set_target_id(row.targetId);
        item->set_requester_nick(row.requesterNick);
        item->set_target_nick(row.targetNick);
        item->set_created_at(row.createdAt);
    }
    session->deliver(DEF_PROT_FRIEND_REQUEST_LIST_RS, response.SerializeAsString());
}

void FriendHandler::onDeleteFriend(const std::shared_ptr<Session> &session, const std::string &payload)
{
    DeleteFriendRq rq;
    if(!handlers::parsePayload(payload, rq)){
        return;
    }
    const int userId = session->userId();
    const int friendId = rq.friend_id();
    const std::string nick = m_db.getUserNickById(userId);

    DeleteFriendRs rs;
    rs.set_operator_nick(nick);
    rs.set_operator_id(userId);
    rs.set_friend_id(friendId);
    
    if (userId <= 0 || friendId <= 0 || userId == friendId) {
        rs.set_result(DELETE_FRIEND_INVALID);
        session->deliver(DEF_PROT_DELETE_FRIEND_RS, rs.SerializeAsString());
        return;
    }
    if(!m_db.isFriend(userId, friendId)) {
        rs.set_result(DELETE_FRIEND_NOT_FRIEND);
        session->deliver(DEF_PROT_DELETE_FRIEND_RS, rs.SerializeAsString());
        return;
    }
    if(!m_db.removeFriendBidirectional(userId, friendId)) {
        rs.set_result(DELETE_FRIEND_DB_ERROR);
        session->deliver(DEF_PROT_DELETE_FRIEND_RS, rs.SerializeAsString());
        return;
    }
    rs.set_result(DELETE_FRIEND_SUCCESS);
    session->deliver(DEF_PROT_DELETE_FRIEND_RS, rs.SerializeAsString());

    //对方在线也要立即从其好友列表移除当前用户
    if(auto friendSession = m_presence.get(friendId)) {
        DeleteFriendRs rs_notice;
        rs_notice.set_friend_id(userId);
        rs_notice.set_operator_id(userId);
        rs_notice.set_operator_nick(nick);
        rs_notice.set_result(DELETE_FRIEND_SUCCESS);
        friendSession->deliver(DEF_PROT_DELETE_FRIEND_RS, rs_notice.SerializeAsString());
    }
    
    log("[好友] 删除 user=", userId, " friend=", friendId);
}

} // namespace imsrv
