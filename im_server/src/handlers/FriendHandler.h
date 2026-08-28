#pragma once

#include <memory>
#include <string>

namespace imsrv {
class Database;
class Presence;
class Session;

class FriendHandler {
public:
    FriendHandler(Database& db, Presence& presence);
    void onAddFriend(const std::shared_ptr<Session>& session, const std::string& payload);
    void onAddFriendReply(const std::shared_ptr<Session>& session, const std::string& payload);
    void onFriendRequestList(const std::shared_ptr<Session>& session, const std::string& payload);
    void onDeleteFriend(const std::shared_ptr<Session>& session, const std::string& payload);

private:
    Database& m_db;
    Presence& m_presence;
};
} // namespace imsrv
