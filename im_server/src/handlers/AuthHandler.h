#pragma once

#include <memory>
#include <string>

namespace imsrv {
class Database;
class Presence;
class Session;
class TokenService;

class AuthHandler {
public:
    AuthHandler(Database& db, Presence& presence, TokenService& tokens);
    void onRegister(const std::shared_ptr<Session>& session, const std::string& payload);
    void onLogin(const std::shared_ptr<Session>& session, const std::string& payload);
    void onTokenLogin(const std::shared_ptr<Session>& session, const std::string& payload);
    void onTokenRefresh(const std::shared_ptr<Session>& session, const std::string& payload);
    void onLogout(const std::shared_ptr<Session>& session, const std::string& payload);

private:
    void activateSession(const std::shared_ptr<Session>& session, int userId);
    Database& m_db;
    Presence& m_presence;
    TokenService& m_tokens;
};
} // namespace imsrv
