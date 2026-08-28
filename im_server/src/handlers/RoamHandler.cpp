#include "handlers/RoamHandler.h"

#include "Database.h"
#include "Log.h"
#include "Session.h"
#include "handlers/HandlerUtils.h"
#include "im.pb.h"

#include <cstdint>

namespace imsrv {
using namespace im::proto;

RoamHandler::RoamHandler(Database& db) : m_db(db) {}

void RoamHandler::onConversations(const std::shared_ptr<Session>& session,
                                  const std::string&)
{
    const int userId = session->userId();
    if (userId <= 0) return;
    RoamConvRs rs;
    for (const auto& message : m_db.roamConversations(userId)) {
        handlers::fillChatInfo(*rs.add_convs(), message);
    }
    session->deliver(DEF_PROT_ROAM_CONV_RS, rs.SerializeAsString());
    log("[漫游] 会话列表 id=", userId, " 会话数=", rs.convs_size());
}

void RoamHandler::onMessages(const std::shared_ptr<Session>& session,
                             const std::string& payload)
{
    const int userId = session->userId();
    if (userId <= 0) return;
    RoamMsgRq rq;
    if (!handlers::parsePayload(payload, rq)) return;
    const int peerId = rq.peer_id();
    const std::int64_t beforeSeq = rq.before_seq() > 0 ? rq.before_seq() : INT64_MAX;
    int limit = rq.limit() > 0 ? rq.limit() : 20;
    if (limit > 100) limit = 100;

    const auto rows = m_db.roamMessages(userId, peerId, beforeSeq, limit);
    RoamMsgRs rs;
    rs.set_peer_id(peerId);
    rs.set_has_more(static_cast<int>(rows.size()) == limit);
    for (const auto& message : rows) {
        handlers::fillChatInfo(*rs.add_msgs(), message);
    }
    rs.set_min_seq(rows.empty() ? 0 : rows.back().seq);
    session->deliver(DEF_PROT_ROAM_MSG_RS, rs.SerializeAsString());
    log("[漫游] 历史 id=", userId, " peer=", peerId,
        " 返回=", rs.msgs_size(), " hasMore=", rs.has_more());
}

} // namespace imsrv
