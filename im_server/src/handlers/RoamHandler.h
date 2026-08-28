#pragma once

#include <memory>
#include <string>

namespace imsrv {
class Database;
class Session;

class RoamHandler {
public:
    explicit RoamHandler(Database& db);
    void onConversations(const std::shared_ptr<Session>& session, const std::string& payload);
    void onMessages(const std::shared_ptr<Session>& session, const std::string& payload);

private:
    Database& m_db;
};
} // namespace imsrv
