#pragma once
// IStorage 的 SQLite 默认实现（依赖系统 sqlite3，全平台可用）

#include "IStorage.h"

struct sqlite3;

namespace im {

class SqliteStorage : public IStorage {
public:
    SqliteStorage() = default;
    ~SqliteStorage() override;

    // 禁止拷贝（持有 sqlite3 句柄）
    SqliteStorage(const SqliteStorage&) = delete;
    SqliteStorage& operator=(const SqliteStorage&) = delete;

    bool open(const std::string& dbPath) override;
    void close() override;

    bool saveSelfInfo(const UserInfo& info) override;
    bool saveFriend(const FriendInfo& info) override;
    std::vector<FriendInfo> loadFriends() override;
    bool saveChatMessage(int ownerId, int peerId, bool outgoing,
                         const std::string& contentUtf8, std::int64_t ts) override;
    std::vector<ChatMessage> loadChatHistory(int ownerId, int peerId, int limit) override;

private:
    bool exec(const char* sql);
    bool createTables();

    sqlite3* m_db = nullptr;
};

} // namespace im
