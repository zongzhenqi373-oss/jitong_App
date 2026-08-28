#include "client_core/SqliteStorage.h"

#include <sqlite3.h>

namespace im {

SqliteStorage::~SqliteStorage()
{
    close();
}

bool SqliteStorage::open(const std::string& dbPath)
{
    if (m_db) return true;
    if (sqlite3_open(dbPath.c_str(), &m_db) != SQLITE_OK) {
        close();
        return false;
    }
    return createTables();
}

void SqliteStorage::close()
{
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

bool SqliteStorage::exec(const char* sql)
{
    if (!m_db) return false;
    char* err = nullptr;
    const int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

bool SqliteStorage::createTables()
{
    // self_info：当前登录用户资料（单行，id 为主键）
    // friends：好友列表
    // chat_messages：聊天记录（owner_id 区分多账号本机共存）
    return exec(
        "CREATE TABLE IF NOT EXISTS self_info("
        "  user_id INTEGER PRIMARY KEY,"
        "  icon_id INTEGER NOT NULL DEFAULT 0,"
        "  nick TEXT NOT NULL DEFAULT '',"
        "  feeling TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE TABLE IF NOT EXISTS friends("
        "  user_id INTEGER PRIMARY KEY,"
        "  icon_id INTEGER NOT NULL DEFAULT 0,"
        "  status INTEGER NOT NULL DEFAULT 1,"
        "  nick TEXT NOT NULL DEFAULT '',"
        "  feeling TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE TABLE IF NOT EXISTS chat_messages("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  owner_id INTEGER NOT NULL,"
        "  peer_id INTEGER NOT NULL,"
        "  outgoing INTEGER NOT NULL DEFAULT 0,"
        "  content TEXT NOT NULL DEFAULT '',"
        "  ts INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_chat_owner_peer ON chat_messages(owner_id, peer_id, ts);");
}

bool SqliteStorage::saveSelfInfo(const UserInfo& info)
{
    if (!m_db) return false;
    const char* sql =
        "INSERT INTO self_info(user_id, icon_id, nick, feeling) VALUES(?,?,?,?) "
        "ON CONFLICT(user_id) DO UPDATE SET icon_id=excluded.icon_id,"
        " nick=excluded.nick, feeling=excluded.feeling;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(st, 1, info.id);
    sqlite3_bind_int(st, 2, info.iconId);
    sqlite3_bind_text(st, 3, info.nick.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, info.feeling.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool SqliteStorage::saveFriend(const FriendInfo& info)
{
    if (!m_db) return false;
    const char* sql =
        "INSERT INTO friends(user_id, icon_id, status, nick, feeling) VALUES(?,?,?,?,?) "
        "ON CONFLICT(user_id) DO UPDATE SET icon_id=excluded.icon_id,"
        " status=excluded.status, nick=excluded.nick, feeling=excluded.feeling;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(st, 1, info.id);
    sqlite3_bind_int(st, 2, info.iconId);
    sqlite3_bind_int(st, 3, info.status);
    sqlite3_bind_text(st, 4, info.nick.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, info.feeling.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

std::vector<FriendInfo> SqliteStorage::loadFriends()
{
    std::vector<FriendInfo> out;
    if (!m_db) return out;
    const char* sql = "SELECT user_id, icon_id, status, nick, feeling FROM friends;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK) return out;
    while (sqlite3_step(st) == SQLITE_ROW) {
        FriendInfo f;
        f.id = sqlite3_column_int(st, 0);
        f.iconId = sqlite3_column_int(st, 1);
        f.status = sqlite3_column_int(st, 2);
        const unsigned char* nick = sqlite3_column_text(st, 3);
        const unsigned char* feeling = sqlite3_column_text(st, 4);
        f.nick = nick ? reinterpret_cast<const char*>(nick) : "";
        f.feeling = feeling ? reinterpret_cast<const char*>(feeling) : "";
        out.push_back(std::move(f));
    }
    sqlite3_finalize(st);
    return out;
}

bool SqliteStorage::saveChatMessage(int ownerId, int peerId, bool outgoing,
                                    const std::string& contentUtf8, std::int64_t ts)
{
    if (!m_db) return false;
    const char* sql =
        "INSERT INTO chat_messages(owner_id, peer_id, outgoing, content, ts) VALUES(?,?,?,?,?);";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(st, 1, ownerId);
    sqlite3_bind_int(st, 2, peerId);
    sqlite3_bind_int(st, 3, outgoing ? 1 : 0);
    sqlite3_bind_text(st, 4, contentUtf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, ts);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

std::vector<ChatMessage> SqliteStorage::loadChatHistory(int ownerId, int peerId, int limit)
{
    std::vector<ChatMessage> out;
    if (!m_db) return out;
    const char* sqlAll =
        "SELECT id, peer_id, outgoing, content, ts FROM chat_messages "
        "WHERE owner_id=? AND peer_id=? ORDER BY ts ASC, id ASC;";
    const char* sqlLimit =
        "SELECT id, peer_id, outgoing, content, ts FROM ("
        "  SELECT id, peer_id, outgoing, content, ts FROM chat_messages "
        "  WHERE owner_id=? AND peer_id=? ORDER BY ts DESC, id DESC LIMIT ?"
        ") ORDER BY ts ASC, id ASC;";
    const char* sql = limit > 0 ? sqlLimit : sqlAll;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int(st, 1, ownerId);
    sqlite3_bind_int(st, 2, peerId);
    if (limit > 0) sqlite3_bind_int(st, 3, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        ChatMessage m;
        m.id = sqlite3_column_int64(st, 0);
        m.peerId = sqlite3_column_int(st, 1);
        m.outgoing = sqlite3_column_int(st, 2) != 0;
        const unsigned char* content = sqlite3_column_text(st, 3);
        m.content = content ? reinterpret_cast<const char*>(content) : "";
        m.ts = sqlite3_column_int64(st, 4);
        out.push_back(std::move(m));
    }
    sqlite3_finalize(st);
    return out;
}

} // namespace im
