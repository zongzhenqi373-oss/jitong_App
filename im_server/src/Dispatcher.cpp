#include "Dispatcher.h"

#include "Log.h"
#include "Session.h"
#include "client_core/Protocol.h"
#include "handlers/AuthHandler.h"
#include "handlers/AiHandler.h"
#include "handlers/FriendHandler.h"
#include "handlers/MessageHandler.h"
#include "handlers/RoamHandler.h"
#include "handlers/SystemHandler.h"
#include "im.pb.h"

namespace imsrv {
using namespace im::proto;

Dispatcher::Dispatcher(Database& db, Presence& presence,
                       TokenService& tokens, HttpFileServer& files)
    : m_auth(std::make_unique<AuthHandler>(db, presence, tokens))
    , m_ai(std::make_unique<AiHandler>(db))
    , m_message(std::make_unique<MessageHandler>(db, presence, files))
    , m_friend(std::make_unique<FriendHandler>(db, presence))
    , m_roam(std::make_unique<RoamHandler>(db))
    , m_system(std::make_unique<SystemHandler>(presence))
{
    m_router.add(DEF_PROT_REGISTER_RQ,
        [this](const auto& s, const auto& p) { m_auth->onRegister(s, p); });
    m_router.add(DEF_PROT_LOGIN_RQ,
        [this](const auto& s, const auto& p) { m_auth->onLogin(s, p); });
    m_router.add(DEF_PROT_TOKEN_LOGIN_RQ,
        [this](const auto& s, const auto& p) { m_auth->onTokenLogin(s, p); });
    m_router.add(DEF_PROT_TOKEN_REFRESH_RQ,
        [this](const auto& s, const auto& p) { m_auth->onTokenRefresh(s, p); });
    m_router.add(DEF_PROT_LOGOUT_RQ,
        [this](const auto& s, const auto& p) { m_auth->onLogout(s, p); });

    m_router.add(DEF_PROT_AI_REPLY_RQ,
        [this](const auto& s, const auto& p) { m_ai->onReply(s, p); });
    m_router.add(DEF_PROT_AI_CANCEL_RQ,
        [this](const auto& s, const auto& p) { m_ai->onCancel(s, p); });

    m_router.add(DEF_PROT_CHAT_INFO_RQ,
        [this](const auto& s, const auto& p) { m_message->onChat(s, p); });

    m_router.add(DEF_PROT_ADD_FRIEND_RQ,
        [this](const auto& s, const auto& p) { m_friend->onAddFriend(s, p); });
    m_router.add(DEF_PROT_ADD_FRIEND_RS,
        [this](const auto& s, const auto& p) { m_friend->onAddFriendReply(s, p); });
    m_router.add(DEF_PROT_FRIEND_REQUEST_LIST_RQ,
        [this](const auto& s, const auto& p) { m_friend->onFriendRequestList(s, p); });

    m_router.add(DEF_PROT_DELETE_FRIEND_RQ,
        [this](const auto& s, const auto& p) { m_friend->onDeleteFriend(s, p); });

    m_router.add(DEF_PROT_ROAM_CONV_RQ,
        [this](const auto& s, const auto& p) { m_roam->onConversations(s, p); });
    m_router.add(DEF_PROT_ROAM_MSG_RQ,
        [this](const auto& s, const auto& p) { m_roam->onMessages(s, p); });

    m_router.add(DEF_PROT_FRIEND_OFFLINE,
        [this](const auto& s, const auto& p) { m_system->onOffline(s, p); });
    m_router.add(DEF_PROT_HEARTBEAT_RQ,
        [this](const auto& s, const auto& p) { m_system->onHeartbeat(s, p); });

}

Dispatcher::~Dispatcher() = default;

bool Dispatcher::hasHandler(std::uint32_t type) const
{
    return m_router.contains(type);
}

void Dispatcher::handle(const std::shared_ptr<Session>& session,
                        std::uint32_t type, const std::string& payload)
{
    m_router.dispatch(session, type, payload);
}

} // namespace imsrv
