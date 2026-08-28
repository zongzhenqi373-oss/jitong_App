#pragma once

#include <memory>
#include <string>

namespace imsrv {
class Database;
class Presence;
class Session;
class HttpFileServer;

class MessageHandler {
public:
    MessageHandler(Database& db, Presence& presence, HttpFileServer& files);
    void onChat(const std::shared_ptr<Session>& session, const std::string& payload);

private:
    Database& m_db;
    Presence& m_presence;
    HttpFileServer& m_files;
};
} // namespace imsrv
