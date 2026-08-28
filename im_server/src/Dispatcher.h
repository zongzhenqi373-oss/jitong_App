#pragma once
// 纯协议路由：只负责协议号注册、查找和统一异常边界。
// 业务规则分别位于 handlers/ 下，路由层不再直接访问 Database/Presence/Server。

#include <cstdint>
#include <memory>
#include <string>
#include "routing/ProtocolRouter.h"

namespace imsrv {
class AuthHandler;
class AiHandler;
class Database;
class FriendHandler;
class HttpFileServer;
class MessageHandler;
class Presence;
class RoamHandler;
class Session;
class SystemHandler;
class TokenService;

class Dispatcher {
public:
    Dispatcher(Database& db, Presence& presence, TokenService& tokens, HttpFileServer& files);
    ~Dispatcher();

    void handle(const std::shared_ptr<Session>& session,
                std::uint32_t type, const std::string& payload);
    bool hasHandler(std::uint32_t type) const;

private:
    std::unique_ptr<AuthHandler> m_auth;
    std::unique_ptr<AiHandler> m_ai;
    std::unique_ptr<MessageHandler> m_message;
    std::unique_ptr<FriendHandler> m_friend;
    std::unique_ptr<RoamHandler> m_roam;
    std::unique_ptr<SystemHandler> m_system;
    ProtocolRouter m_router;
};
} // namespace imsrv
