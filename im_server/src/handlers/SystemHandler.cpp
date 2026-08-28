#include "handlers/SystemHandler.h"

#include "Presence.h"
#include "Session.h"
#include "handlers/HandlerUtils.h"
#include "im.pb.h"

namespace imsrv {
using namespace im::proto;

SystemHandler::SystemHandler(Presence& presence) : m_presence(presence) {}

void SystemHandler::onOffline(const std::shared_ptr<Session>& session,
                              const std::string& payload)
{
    FriendOffline packet;
    if (!handlers::parsePayload(payload, packet)) return;
    m_presence.offline(session->userId(), session);
    session->close();
}

void SystemHandler::onHeartbeat(const std::shared_ptr<Session>& session, const std::string&)
{
    if (session->userId() > 0) m_presence.touch(session->userId());
    session->deliver(DEF_PROT_HEARTBEAT_RS, "");
}

} // namespace imsrv
