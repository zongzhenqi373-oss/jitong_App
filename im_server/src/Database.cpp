#include "Database.h"
#include "sha256.h"

#include <sqlite3.h>
#include <argon2.h>
#include <openssl/rand.h>
#include <ctime>
#include <random>
#include <stdexcept>

namespace imsrv {

// 协议结果码（与 client_core/Protocol.h、IMServer/def.h 保持一致，避免双端漂移）
namespace rc {
    constexpr int REGISTER_SUCC = 1;
    constexpr int REGISTER_NICK_EXIT = 2;
    constexpr int REGISTER_TEL_EXIT = 3;
    constexpr int LOGIN_SUCCESS = 0;
    constexpr int LOGIN_NOTEXIT = 1;
    constexpr int LOGIN_PASSERROR = 2;
}

namespace {

std::string generateSaltHex()
{
    std::random_device rd;
    unsigned char bytes[16];
    for (auto& b : bytes) b = static_cast<unsigned char>(rd() & 0xFF);
    static const char* hex = "0123456789abcdef";
    std::string salt;
    salt.reserve(32);
    for (unsigned char b : bytes) {
        salt.push_back(hex[(b >> 4) & 0x0F]);
        salt.push_back(hex[b & 0x0F]);
    }
    return salt;
}

constexpr std::uint32_t ARGON2_TIME_COST = 3;
constexpr std::uint32_t ARGON2_MEMORY_KIB = 64 * 1024;
constexpr std::uint32_t ARGON2_PARALLELISM = 1;
constexpr std::uint32_t ARGON2_SALT_BYTES = 16;
constexpr std::uint32_t ARGON2_HASH_BYTES = 32;
constexpr int DB_SCHEMA_VERSION = 1;

std::string hashPasswordArgon2id(const std::string& passwordSecret)
{
    unsigned char salt[ARGON2_SALT_BYTES];
    if (RAND_bytes(salt, sizeof(salt)) != 1)
        throw std::runtime_error("Argon2id salt generation failed");
    const std::size_t encodedLen = argon2_encodedlen(
        ARGON2_TIME_COST, ARGON2_MEMORY_KIB, ARGON2_PARALLELISM,
        ARGON2_SALT_BYTES, ARGON2_HASH_BYTES, Argon2_id);
    std::string encoded(encodedLen, '\0');
    const int rc = argon2id_hash_encoded(
        ARGON2_TIME_COST, ARGON2_MEMORY_KIB, ARGON2_PARALLELISM,
        passwordSecret.data(), passwordSecret.size(), salt, sizeof(salt),
        ARGON2_HASH_BYTES, encoded.data(), encoded.size());
    if (rc != ARGON2_OK) throw std::runtime_error(argon2_error_message(rc));
    encoded.resize(std::char_traits<char>::length(encoded.c_str()));
    return encoded;
}

bool verifyPasswordArgon2id(const std::string& encoded, const std::string& passwordSecret)
{
    return argon2id_verify(encoded.c_str(), passwordSecret.data(), passwordSecret.size()) == ARGON2_OK;
}

// 读取数据库版本
int readDatabaseVersion(sqlite3* db)
{
    sqlite3_stmt* statement = nullptr;

    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &statement, nullptr) != SQLITE_OK) {
        return -1;
    }
    int version = -1;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        version = sqlite3_column_int(statement, 0);
    }

    sqlite3_finalize(statement);
    return version;
}

// 迁移会话 id
bool migrateConversationIds(sqlite3* db)
{
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return false;
    }

    auto rollback = [db] {
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    };

    const char* migrationSql =
        "UPDATE messages "
        "SET conversation_id = "
        "  (CASE "
        "     WHEN sender_id < receiver_id THEN sender_id "
        "     ELSE receiver_id "
        "   END) * 4294967296 "
        "  + "
        "  (CASE "
        "     WHEN sender_id < receiver_id THEN receiver_id "
        "     ELSE sender_id "
        "   END);";

    if (sqlite3_exec(db, migrationSql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        rollback();
        return false;
    }

    if (sqlite3_exec(db, "PRAGMA user_version=1;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        rollback();
        return false;
    }

    if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        rollback();
        return false;
    }

    return true;
}

} // namespace

Database::~Database()
{
    for (auto& c : m_pool) {
        if (c->db) sqlite3_close(c->db);
    }
}

bool Database::execOn(sqlite3* db, const char* sql)
{
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

Database::Conn& Database::acquire()
{
    return *m_pool[m_next.fetch_add(1) % m_pool.size()];
}

bool Database::open(const std::string& dbPath, int poolSize)
{
    if (!m_pool.empty()) return true;
    if (poolSize < 1) poolSize = 1;

    for (int i = 0; i < poolSize; ++i) {
        auto conn = std::make_unique<Conn>();
        if (sqlite3_open(dbPath.c_str(), &conn->db) != SQLITE_OK) return false;

        // SQLite 写并发关键配置：WAL（读写不互斥）+ busy_timeout（写冲突等待而非报错）
        execOn(conn->db, "PRAGMA journal_mode=WAL;");
        execOn(conn->db, "PRAGMA busy_timeout=5000;");
        execOn(conn->db, "PRAGMA foreign_keys=ON;");

        m_pool.push_back(std::move(conn));
    }

    // 建表（在第一个连接上执行）
    Conn& c = *m_pool[0];
    std::lock_guard<std::mutex> lock(c.mtx);
    const bool schemaOk = execOn(c.db,
        "CREATE TABLE IF NOT EXISTS t_user("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL UNIQUE,"
        "  tel TEXT NOT NULL UNIQUE,"
        "  passwd TEXT NOT NULL,"
        "  salt TEXT NOT NULL DEFAULT '',"
        "  password_algo TEXT NOT NULL DEFAULT 'argon2id',"
        "  feeling TEXT NOT NULL DEFAULT '',"
        "  iconid INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE TABLE IF NOT EXISTS t_friend("
        "  idA INTEGER NOT NULL,"
        "  idB INTEGER NOT NULL,"
        "  PRIMARY KEY(idA, idB)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_friend_idB ON t_friend(idB);"
        "CREATE TABLE IF NOT EXISTS friend_requests("
        "  requester_id INTEGER NOT NULL,"
        "  target_id INTEGER NOT NULL,"
        "  status INTEGER NOT NULL DEFAULT 0,"
        "  created_at INTEGER NOT NULL,"
        "  updated_at INTEGER NOT NULL,"
        "  PRIMARY KEY(requester_id,target_id)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_friend_request_target_status "
        "ON friend_requests(target_id,status,created_at);"
        "CREATE INDEX IF NOT EXISTS idx_friend_request_sender_status "
        "ON friend_requests(requester_id,status,created_at);"
        // 全量消息表：漫游/离线单表，conversation_id 会话维度索引（对齐 QQNT PeerUidIndex+MsgTime）
        "CREATE TABLE IF NOT EXISTS messages("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  msg_id TEXT UNIQUE,"
        "  conversation_id INTEGER NOT NULL,"
        "  sender_id INTEGER NOT NULL,"
        "  receiver_id INTEGER NOT NULL,"
        "  type INTEGER NOT NULL DEFAULT 0,"
        "  content TEXT,"
        "  media_path TEXT,"
        "  img_w INTEGER NOT NULL DEFAULT 0,"
        "  img_h INTEGER NOT NULL DEFAULT 0,"
        "  ts INTEGER NOT NULL,"
        "  is_delivered INTEGER NOT NULL DEFAULT 0,"
        "  is_read INTEGER NOT NULL DEFAULT 0,"
        "  seq INTEGER NOT NULL DEFAULT 0,"
        "  file_id TEXT,"
        "  file_size INTEGER NOT NULL DEFAULT 0,"
        "  content_type TEXT NOT NULL DEFAULT '',"
        "  sha256 TEXT NOT NULL DEFAULT ''"
        ");"
        // 漫游索引（roamMessages）
        "CREATE INDEX IF NOT EXISTS idx_msg_conv_ts ON messages(conversation_id, ts);"
        // 唯一索引本身即可服务漫游查询，无需再维护一份字段相同的普通索引。
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_msg_conv_seq_unique ON messages(conversation_id, seq);"
        // 漫游会话列表（roamConversations）按 sender_id 分组取末条，需 sender_id 索引避免全表扫描
        "CREATE INDEX IF NOT EXISTS idx_msg_sender ON messages(sender_id);"
        // 认证会话表
        "CREATE TABLE IF NOT EXISTS auth_sessions("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  session_id TEXT NOT NULL UNIQUE,"
        "  family_id TEXT NOT NULL,"
        "  user_id INTEGER NOT NULL,"
        "  device_id TEXT NOT NULL,"
        "  access_hash TEXT NOT NULL UNIQUE,"
        "  access_expires_at INTEGER NOT NULL,"
        "  refresh_hash TEXT NOT NULL UNIQUE,"
        "  refresh_expires_at INTEGER NOT NULL,"
        "  generation INTEGER NOT NULL DEFAULT 1,"
        "  revoked INTEGER NOT NULL DEFAULT 0,"
        "  created_at INTEGER NOT NULL,"
        "  last_used_at INTEGER NOT NULL"
        ");"
        // 用户设备索引
        "CREATE INDEX IF NOT EXISTS idx_auth_user_device "
        "ON auth_sessions(user_id,device_id);"
        "CREATE INDEX IF NOT EXISTS idx_auth_family "
        "ON auth_sessions(family_id);"
        // 刷新令牌历史
        "CREATE TABLE IF NOT EXISTS auth_refresh_history("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  family_id TEXT NOT NULL,"
        "  token_hash TEXT NOT NULL UNIQUE,"
        "  generation INTEGER NOT NULL,"
        "  used_at INTEGER NOT NULL"
        ");"
        // 刷新令牌历史索引
        "CREATE INDEX IF NOT EXISTS idx_refresh_family "
        "ON auth_refresh_history(family_id);"
        //建立文件id索引
        "CREATE INDEX IF NOT EXISTS idx_msg_file_id ON messages(file_id);"
    );
    if (!schemaOk) return false;
    // 兼容旧数据库；重复执行时 duplicate column 错误可安全忽略。
    execOn(c.db, "ALTER TABLE t_user ADD COLUMN password_algo TEXT NOT NULL DEFAULT 'legacy_sha256';");
    execOn(c.db, "ALTER TABLE messages ADD COLUMN content_type TEXT NOT NULL DEFAULT '';");
    execOn(c.db, "ALTER TABLE messages ADD COLUMN sha256 TEXT NOT NULL DEFAULT '';");
    // 清理旧版重复/不匹配索引。离线消息跨会话按服务端时间+行号稳定排序，
    // 因此索引也按 receiver/is_delivered/ts/id 排列。
    execOn(c.db, "DROP INDEX IF EXISTS idx_msg_conv_seq;");
    execOn(c.db, "DROP INDEX IF EXISTS idx_msg_recv;");
    if (!execOn(c.db,
        "CREATE INDEX IF NOT EXISTS idx_msg_recv_delivery "
        "ON messages(receiver_id,is_delivered,ts,id);")) return false;

    const int databaseVersion =
    readDatabaseVersion(c.db);

    if (databaseVersion < 0) {
        return false;
    }

    if (databaseVersion < DB_SCHEMA_VERSION) {
        if (!migrateConversationIds(c.db)) {
            return false;
        }
    }

    // 单写线程：跟只读连接池指向同一个数据库文件，建表完成后再打开
    if (!m_writeQueue.open(dbPath)) return false;

    return true;
}

void Database::seedIfEmpty()
{
    m_writeQueue.submit([&](sqlite3* db) {
        // 已有用户则跳过（幂等）
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t_user;", -1, &st, nullptr) != SQLITE_OK) return;
        int count = 0;
        if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
        if (count > 0) return;

        struct Seed { const char* nick; const char* tel; const char* feeling; };
        const Seed users[] = {
            {"张三", "13800000001", "我是张三"},
            {"李四", "13800000002", "我是李四"},
            {"王五", "13800000003", "我是王五"},
        };

        execOn(db, "BEGIN;");
        int ids[3] = {0, 0, 0};
        for (int i = 0; i < 3; ++i) {
            const std::string stored = hashPasswordArgon2id(im::sha256Hex("123456"));
            sqlite3_stmt* ins = nullptr;
            sqlite3_prepare_v2(db,
                "INSERT INTO t_user(name, tel, passwd, salt, password_algo, feeling, iconid) VALUES(?,?,?,?,?,?,?);",
                -1, &ins, nullptr);
            sqlite3_bind_text(ins, 1, users[i].nick, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins, 2, users[i].tel, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins, 3, stored.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins, 4, "", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins, 5, "argon2id", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins, 6, users[i].feeling, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(ins, 7, i + 1);
            sqlite3_step(ins);
            sqlite3_finalize(ins);
            ids[i] = static_cast<int>(sqlite3_last_insert_rowid(db));
        }
        // 好友关系（双向）：张三↔李四、李四↔王五
        const int pairs[][2] = {{ids[0], ids[1]}, {ids[1], ids[0]}, {ids[1], ids[2]}, {ids[2], ids[1]}};
        for (const auto& p : pairs) {
            sqlite3_stmt* ins = nullptr;
            sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO t_friend(idA, idB) VALUES(?,?);", -1, &ins, nullptr);
            sqlite3_bind_int(ins, 1, p[0]);
            sqlite3_bind_int(ins, 2, p[1]);
            sqlite3_step(ins);
            sqlite3_finalize(ins);
        }
        execOn(db, "COMMIT;");
    });
}

int Database::registerUser(const std::string& nick, const std::string& tel, const std::string& passHash)
{
    std::string stored;
    try{
        stored = hashPasswordArgon2id(passHash);
    }catch(const std::exception& e){
        return -1;
    }
    return m_writeQueue.submit([nick,tel,stored](sqlite3* db) -> int {
        // 昵称唯一性
        sqlite3_stmt* st = nullptr;
        auto rollback = [db] { sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);};
        if(sqlite3_prepare_v2(db, "SELECT 1 FROM t_user WHERE name=?;", -1, &st, nullptr) != SQLITE_OK){
            rollback();
            return -1;
        }
        sqlite3_bind_text(st, 1, nick.c_str(), -1, SQLITE_TRANSIENT);
        bool exists = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
        if (exists) return rc::REGISTER_NICK_EXIT;

        // 电话唯一性
        if(sqlite3_prepare_v2(db, "SELECT 1 FROM t_user WHERE tel=?;", -1, &st, nullptr) != SQLITE_OK){
            rollback();
            return -1;
        }
        sqlite3_bind_text(st, 1, tel.c_str(), -1, SQLITE_TRANSIENT);
        exists = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
        if (exists) return rc::REGISTER_TEL_EXIT;

        sqlite3_stmt* ins = nullptr;
        if(sqlite3_prepare_v2(db,
            "INSERT INTO t_user(name, tel, passwd, salt, password_algo, feeling, iconid) VALUES(?,?,?,?,?,?,3);",
            -1, &ins, nullptr) != SQLITE_OK)
            {
                rollback();
                return -1;
            }
        sqlite3_bind_text(ins, 1, nick.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 2, tel.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 3, stored.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 4, "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 5, "argon2id", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 6, "努力实现财富自由", -1, SQLITE_TRANSIENT);
        const bool ok = sqlite3_step(ins) == SQLITE_DONE;
        sqlite3_finalize(ins);
        return ok ? rc::REGISTER_SUCC : -1;
    });
}

int Database::loginUser(const std::string& tel, const std::string& passHash, int& outUserId)
{
    std::string stored;
    std::string salt;
    std::string algorithm;
    {
        Conn& c = acquire();
        std::lock_guard<std::mutex> lock(c.mtx);
        sqlite3_stmt* st = nullptr;

        if(sqlite3_prepare_v2(c.db, "SELECT passwd, salt, id, password_algo FROM t_user WHERE tel=?;", -1, &st, nullptr) != SQLITE_OK){
            return -1;
        }
        sqlite3_bind_text(st, 1, tel.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_ROW) {
            sqlite3_finalize(st);
            return rc::LOGIN_NOTEXIT;
        }

        const auto* storedText = sqlite3_column_text(st, 0);
        const auto* saltText = sqlite3_column_text(st, 1);
        const auto* algorithmText = sqlite3_column_text(st, 3);
        
        stored = storedText ? reinterpret_cast<const char*>(storedText) : "";
        salt = saltText ? reinterpret_cast<const char*>(saltText) : "";
        outUserId = sqlite3_column_int(st, 2);
        algorithm = algorithmText ? reinterpret_cast<const char*>(algorithmText) : "legacy_sha256";
        sqlite3_finalize(st);
    }

    if (algorithm == "argon2id")
        return verifyPasswordArgon2id(stored, passHash) ? rc::LOGIN_SUCCESS : rc::LOGIN_PASSERROR;

    if (im::sha256Hex(salt + passHash) != stored) return rc::LOGIN_PASSERROR;

    // 老账号验证成功后立即升级；迁移失败不阻断本次合法登录，下次继续尝试。
    try {
        //Argon2id在调用线程计算，不占用数据库写线程
        const std::string upgraded = hashPasswordArgon2id(passHash);
        const int userId = outUserId;
        m_writeQueue.submit([upgraded, userId](sqlite3* db)-> bool{
            sqlite3_stmt* st = nullptr;

            //更新密码算法
            if(sqlite3_prepare_v2(
                db,
                "UPDATE t_user SET passwd=?,salt='',password_algo='argon2id' WHERE id=? AND password_algo='legacy_sha256';",
                -1, &st, nullptr
            ) != SQLITE_OK){
                return false;
            }
            sqlite3_bind_text(st, 1, upgraded.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st, 2, userId);
            const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
            sqlite3_finalize(st);
            return ok;
        });
    }catch(const std::exception&){
        //密码已经验证成功，迁移失败不阻止本次登录
        //下次登录继续尝试迁移
    }
    return rc::LOGIN_SUCCESS;
}

bool Database::getUser(int id, UserRecord& out)
{
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);

    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(c.db, "SELECT id, iconid, name, feeling FROM t_user WHERE id=?;", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, id);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return false;
    }
    out.id = sqlite3_column_int(st, 0);
    out.iconId = sqlite3_column_int(st, 1);
    const unsigned char* nick = sqlite3_column_text(st, 2);
    const unsigned char* feeling = sqlite3_column_text(st, 3);
    out.nick = nick ? reinterpret_cast<const char*>(nick) : "";
    out.feeling = feeling ? reinterpret_cast<const char*>(feeling) : "";
    sqlite3_finalize(st);
    return true;
}

bool Database::isFriend(int idA, int idB)
{
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);

    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(c.db, "SELECT 1 FROM t_friend WHERE idA=? AND idB=?;", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, idA);
    sqlite3_bind_int(st, 2, idB);
    const bool found = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return found;
}

std::vector<FriendRecord> Database::getFriends(int id)
{
    std::vector<FriendRecord> out;
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);

    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(c.db,
        "SELECT u.id, u.iconid, u.name, u.feeling FROM t_friend f "
        "JOIN t_user u ON u.id = f.idB WHERE f.idA = ?;",
        -1, &st, nullptr);
    sqlite3_bind_int(st, 1, id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        FriendRecord fr;
        fr.id = sqlite3_column_int(st, 0);
        fr.iconId = sqlite3_column_int(st, 1);
        const unsigned char* nick = sqlite3_column_text(st, 2);
        const unsigned char* feeling = sqlite3_column_text(st, 3);
        fr.nick = nick ? reinterpret_cast<const char*>(nick) : "";
        fr.feeling = feeling ? reinterpret_cast<const char*>(feeling) : "";
        out.push_back(std::move(fr));
    }
    sqlite3_finalize(st);
    return out;
}

bool Database::addFriendBidirectional(int idA, int idB)
{
    return m_writeQueue.submit([idA,idB](sqlite3* db) -> bool {
        //开启事务
        if(sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK){
            return false;
        }

        //回滚函数
        auto rollback = [db] { sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);};

        const std::pair<int,int> pairs[] = {{idA, idB}, {idB, idA}};
        //插入好友关系
        for (const auto& p : pairs) {
            sqlite3_stmt* st = nullptr;
            //插入好友关系，插入失败直接回滚
            if(sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO t_friend(idA, idB) VALUES(?,?);", -1, &st, nullptr) != SQLITE_OK){
                rollback();
                return false;
            }
            sqlite3_bind_int(st, 1, p.first);
            sqlite3_bind_int(st, 2, p.second);
            //检查插入结果，失败直接回滚，SQLITE_DONE表示成功
            if (sqlite3_step(st) != SQLITE_DONE) {
                sqlite3_finalize(st);
                rollback();
                return false;
            }
            sqlite3_finalize(st);
        }
        //提交事务,失败直接回滚
        if(sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK){
            rollback();
            return false;
        }
        return true;
    });
}

bool Database::createFriendRequest(int requesterId, int targetId)
{
    return m_writeQueue.submit([requesterId, targetId](sqlite3* db) -> bool {
        if (requesterId <= 0 || targetId <= 0 || requesterId == targetId) return false;
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "INSERT INTO friend_requests(requester_id,target_id,status,created_at,updated_at) "
            "VALUES(?,?,0,?,?) ON CONFLICT(requester_id,target_id) DO UPDATE SET "
            "status=0,created_at=excluded.created_at,updated_at=excluded.updated_at;";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return false;
        const auto now = static_cast<sqlite3_int64>(std::time(nullptr));
        sqlite3_bind_int(st, 1, requesterId);
        sqlite3_bind_int(st, 2, targetId);
        sqlite3_bind_int64(st, 3, now);
        sqlite3_bind_int64(st, 4, now);
        const bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        return ok;
    });
}

std::vector<FriendRequestRecord> Database::pendingFriendRequests(int userId)
{
    std::vector<FriendRequestRecord> out;
    if (userId <= 0) return out;
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT r.requester_id,r.target_id,a.name,b.name,r.created_at "
        "FROM friend_requests r "
        "JOIN t_user a ON a.id=r.requester_id "
        "JOIN t_user b ON b.id=r.target_id "
        "WHERE r.status=0 AND (r.requester_id=? OR r.target_id=?) "
        "ORDER BY r.created_at DESC;";
    if (sqlite3_prepare_v2(c.db, sql, -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int(st, 1, userId);
    sqlite3_bind_int(st, 2, userId);
    while (sqlite3_step(st) == SQLITE_ROW) {
        FriendRequestRecord record;
        record.requesterId = sqlite3_column_int(st, 0);
        record.targetId = sqlite3_column_int(st, 1);
        const unsigned char* requesterNick = sqlite3_column_text(st, 2);
        const unsigned char* targetNick = sqlite3_column_text(st, 3);
        record.requesterNick = requesterNick ? reinterpret_cast<const char*>(requesterNick) : "";
        record.targetNick = targetNick ? reinterpret_cast<const char*>(targetNick) : "";
        record.createdAt = sqlite3_column_int64(st, 4);
        out.push_back(std::move(record));
    }
    sqlite3_finalize(st);
    return out;
}

bool Database::resolveFriendRequest(int requesterId, int targetId, bool accept)
{
    return m_writeQueue.submit([requesterId, targetId, accept](sqlite3* db) -> bool {
        if (sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) return false;
        auto rollback = [db] { sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr); };
        sqlite3_stmt* update = nullptr;
        if (sqlite3_prepare_v2(db,
            "UPDATE friend_requests SET status=?,updated_at=? "
            "WHERE requester_id=? AND target_id=? AND status=0;",
            -1, &update, nullptr) != SQLITE_OK) { rollback(); return false; }
        sqlite3_bind_int(update, 1, accept ? 1 : 2);
        sqlite3_bind_int64(update, 2, static_cast<sqlite3_int64>(std::time(nullptr)));
        sqlite3_bind_int(update, 3, requesterId);
        sqlite3_bind_int(update, 4, targetId);
        const bool updated = sqlite3_step(update) == SQLITE_DONE && sqlite3_changes(db) == 1;
        sqlite3_finalize(update);
        if (!updated) { rollback(); return false; }

        if (accept) {
            sqlite3_stmt* insert = nullptr;
            if (sqlite3_prepare_v2(db,
                "INSERT OR IGNORE INTO t_friend(idA,idB) VALUES(?,?);",
                -1, &insert, nullptr) != SQLITE_OK) { rollback(); return false; }
            const std::pair<int,int> pairs[] = {{requesterId,targetId},{targetId,requesterId}};
            for (const auto& pair : pairs) {
                sqlite3_reset(insert);
                sqlite3_clear_bindings(insert);
                sqlite3_bind_int(insert, 1, pair.first);
                sqlite3_bind_int(insert, 2, pair.second);
                if (sqlite3_step(insert) != SQLITE_DONE) {
                    sqlite3_finalize(insert); rollback(); return false;
                }
            }
            sqlite3_finalize(insert);

            // 双方恰好同时申请时，接受任意一条后另一条也不应继续显示为“等待中”。
            sqlite3_stmt* reverse = nullptr;
            if (sqlite3_prepare_v2(db,
                "UPDATE friend_requests SET status=1,updated_at=? "
                "WHERE requester_id=? AND target_id=? AND status=0;",
                -1, &reverse, nullptr) != SQLITE_OK) { rollback(); return false; }
            sqlite3_bind_int64(reverse, 1, static_cast<sqlite3_int64>(std::time(nullptr)));
            sqlite3_bind_int(reverse, 2, targetId);
            sqlite3_bind_int(reverse, 3, requesterId);
            const bool reverseOk = sqlite3_step(reverse) == SQLITE_DONE;
            sqlite3_finalize(reverse);
            if (!reverseOk) { rollback(); return false; }
        }
        if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            rollback(); return false;
        }
        return true;
    });
}

//删除好友
bool Database::removeFriendBidirectional(int idA, int idB)
{
    return m_writeQueue.submit([idA,idB](sqlite3* db) -> bool{
        if(idA <= 0 || idB <= 0 || idA == idB){
            return false;
        }
        //开启事务
        if(sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK){
            return false;
        }
        //回滚函数
        auto rollback = [db] { sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);};
        //删除双向好友关系
        sqlite3_stmt* st = nullptr;
        const char* sql = "DELETE FROM t_friend WHERE (idA=? AND idB=?) OR (idA=? AND idB=?);";
        if(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK){
            rollback();
            return false;
        }
        sqlite3_bind_int(st, 1, idA);
        sqlite3_bind_int(st, 2, idB);
        sqlite3_bind_int(st, 3, idB);
        sqlite3_bind_int(st, 4, idA);
        if(sqlite3_step(st) != SQLITE_DONE){
            sqlite3_finalize(st);
            rollback();
            return false;
        }
        sqlite3_finalize(st);
        //提交事务
        if(sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK){
            rollback();
            return false;
        }
        return true;
    });
}

//根据昵称获取用户id
int Database::getUserIdByNick(const std::string& nick)
{
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);

    sqlite3_stmt* st = nullptr;
    if(sqlite3_prepare_v2(c.db, "SELECT id FROM t_user WHERE name=?;", -1, &st, nullptr) != SQLITE_OK){
        return 0;
    }
    sqlite3_bind_text(st, 1, nick.c_str(), -1, SQLITE_TRANSIENT);
    int id = 0;
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return id;
}

std::string Database::getUserNickById(int id)
{
    std::vector<UserRecord> users;
    if(!getUser(id, users.emplace_back())){
        return "";
    }

    std::string nick;
    
    if(!users.empty()){
        nick = users[0].nick;
    }

    return nick;
}

//保存消息
bool Database::saveMessage(StoredMessage& m, bool delivered)
{
    // 迁移到单写线程（DbWriteQueue）：任意时刻只可能有一个写操作在执行，
    // "幂等读 + 取号 + 插入"这几步天然串行，不再需要 BEGIN IMMEDIATE 抢全局写锁来防
    // 竞态——BEGIN IMMEDIATE 原来是为了在多连接并发时提前占锁，现在只有一个写连接，
    // 没有"提前占锁"的对象了，改回普通 BEGIN 即可，只是为了失败时能整体回滚。
    return m_writeQueue.submit([&](sqlite3* db) -> bool {
        if (!execOn(db, "BEGIN IMMEDIATE;")) return false; // 失败直接返回

        //回滚函数
        auto rollback = [db] { sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);};

        const std::int64_t convId = makeConversationId(m.senderId, m.receiverId);

        // 幂等：若该 msg_id 已存在（漫游/重发），直接读回已有 seq，不再分配新号
        {
            sqlite3_stmt* q = nullptr;
            if(sqlite3_prepare_v2(db, "SELECT seq FROM messages WHERE msg_id=?;", -1, &q, nullptr) != SQLITE_OK){
                rollback();
                return false;
            }
            sqlite3_bind_text(q, 1, m.msgId.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(q) == SQLITE_ROW) {
                m.seq = sqlite3_column_int64(q, 0);
                sqlite3_finalize(q);
                execOn(db, "COMMIT;");
                return true;
            }
            sqlite3_finalize(q);
        }

        // 会话级单调递增 seq：当前会话 MAX(seq)+1（单写线程内串行，保证不重号）
        std::int64_t nextSeq = 1;
        {
            sqlite3_stmt* q = nullptr;
            if(sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(seq),0)+1 FROM messages WHERE conversation_id=?;", -1, &q, nullptr) != SQLITE_OK){
                rollback();
                return false;
            }
            sqlite3_bind_int64(q, 1, convId);
            if (sqlite3_step(q) == SQLITE_ROW)
                nextSeq = sqlite3_column_int64(q, 0);
            sqlite3_finalize(q);
        }
        m.seq = nextSeq;

        sqlite3_stmt* st = nullptr;
        // msg_id UNIQUE 保证幂等；会话 seq 唯一索引是并发顺序的数据库级最后防线。
        if(sqlite3_prepare_v2(db,
            "INSERT INTO messages"
            "(msg_id, conversation_id, sender_id, receiver_id, type, content, media_path, img_w, img_h, ts, is_delivered, is_read, seq, file_id, file_size, content_type, sha256)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,0,?,?,?,?,?);",
            -1, &st, nullptr) != SQLITE_OK){
                rollback();
                return false;
            }
        sqlite3_bind_text(st, 1, m.msgId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, convId);
        sqlite3_bind_int(st, 3, m.senderId);
        sqlite3_bind_int(st, 4, m.receiverId);
        sqlite3_bind_int(st, 5, m.type);
        sqlite3_bind_text(st, 6, m.content.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 7, m.mediaPath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 8, m.imgW);
        sqlite3_bind_int(st, 9, m.imgH);
        sqlite3_bind_int64(st, 10, m.ts);
        sqlite3_bind_int(st, 11, delivered ? 1 : 0);
        sqlite3_bind_int64(st, 12, m.seq);
        sqlite3_bind_text(st, 13, m.fileId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 14, m.fileSize);
        sqlite3_bind_text(st, 15, m.contentType.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 16, m.sha256.c_str(), -1, SQLITE_TRANSIENT);
        const bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok) { rollback(); return false; }
        if (!execOn(db, "COMMIT;")) { rollback(); return false; }
        return true;
    });
}

//拉取未送达消息
std::vector<StoredMessage> Database::pullUndelivered(int receiverId)
{
    std::vector<StoredMessage> out;
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);

    sqlite3_stmt* st = nullptr;
    if(sqlite3_prepare_v2(c.db,
        "SELECT msg_id, sender_id, receiver_id, type, content, media_path, img_w, img_h, ts, seq, file_id, file_size, content_type, sha256 "
        "FROM messages WHERE receiver_id=? AND is_delivered=0 ORDER BY ts ASC, id ASC;",
        -1, &st, nullptr) != SQLITE_OK){
        return out;
    }
    sqlite3_bind_int(st, 1, receiverId);
    while (sqlite3_step(st) == SQLITE_ROW) {
        StoredMessage m;
        const unsigned char* msgId = sqlite3_column_text(st, 0);
        m.msgId = msgId ? reinterpret_cast<const char*>(msgId) : "";
        m.senderId = sqlite3_column_int(st, 1);
        m.receiverId = sqlite3_column_int(st, 2);
        m.type = sqlite3_column_int(st, 3);
        const unsigned char* content = sqlite3_column_text(st, 4);
        const unsigned char* path = sqlite3_column_text(st, 5);
        m.content = content ? reinterpret_cast<const char*>(content) : "";
        m.mediaPath = path ? reinterpret_cast<const char*>(path) : "";
        m.imgW = sqlite3_column_int(st, 6);
        m.imgH = sqlite3_column_int(st, 7);
        m.ts = sqlite3_column_int64(st, 8);
        m.seq = sqlite3_column_int64(st, 9);
        const unsigned char* fileId = sqlite3_column_text(st, 10);
        m.fileId = fileId ? reinterpret_cast<const char*>(fileId) : "";
        m.fileSize = sqlite3_column_int64(st, 11);
        const unsigned char* contentType = sqlite3_column_text(st, 12);
        const unsigned char* sha256 = sqlite3_column_text(st, 13);
        m.contentType = contentType ? reinterpret_cast<const char*>(contentType) : "";
        m.sha256 = sha256 ? reinterpret_cast<const char*>(sha256) : "";
        out.push_back(std::move(m));
    }
    sqlite3_finalize(st);
    return out;
}

//标记消息已送达
void Database::markDelivered(const std::vector<std::string>& msgIds)
{
    if (msgIds.empty()) return;
    m_writeQueue.submit([&](sqlite3* db) {
        sqlite3_stmt* st = nullptr;
        if(sqlite3_prepare_v2(db, "UPDATE messages SET is_delivered=1 WHERE msg_id=?;", -1, &st, nullptr) != SQLITE_OK) return;
        for (const auto& id : msgIds) {
            sqlite3_reset(st);
            sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    });
}

// 从结果集当前行装载一条 StoredMessage（列序须与 SELECT 一致：
// msg_id, sender_id, receiver_id, type, content, media_path, img_w, img_h, ts, seq,
// file_id, file_size, content_type, sha256）
static StoredMessage readMessageRow(sqlite3_stmt* st)
{
    StoredMessage m;
    const unsigned char* msgId = sqlite3_column_text(st, 0);
    m.msgId = msgId ? reinterpret_cast<const char*>(msgId) : "";
    m.senderId = sqlite3_column_int(st, 1);
    m.receiverId = sqlite3_column_int(st, 2);
    m.type = sqlite3_column_int(st, 3);
    const unsigned char* content = sqlite3_column_text(st, 4);
    const unsigned char* path = sqlite3_column_text(st, 5);
    m.content = content ? reinterpret_cast<const char*>(content) : "";
    m.mediaPath = path ? reinterpret_cast<const char*>(path) : "";
    m.imgW = sqlite3_column_int(st, 6);
    m.imgH = sqlite3_column_int(st, 7);
    m.ts = sqlite3_column_int64(st, 8);
    m.seq = sqlite3_column_int64(st, 9);
    const unsigned char* fileId = sqlite3_column_text(st, 10);
    m.fileId = fileId ? reinterpret_cast<const char*>(fileId) : "";
    m.fileSize = sqlite3_column_int64(st, 11);
    const unsigned char* contentType = sqlite3_column_text(st, 12);
    const unsigned char* sha256 = sqlite3_column_text(st, 13);
    m.contentType = contentType ? reinterpret_cast<const char*>(contentType) : "";
    m.sha256 = sha256 ? reinterpret_cast<const char*>(sha256) : "";
    return m;
}

//漫游会话
std::vector<StoredMessage> Database::roamConversations(int userId)
{
    std::vector<StoredMessage> out;
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);

    // 每会话末条：内层两个子查询各走 idx_msg_sender / idx_msg_recv 避免 OR 全表扫描，
    // UNION ALL 合并后按 conversation_id 取全局 MAX(id)（同会话我发/我收各一末条时取更晚者）。
    sqlite3_stmt* st = nullptr;
    if(sqlite3_prepare_v2(c.db,
        "SELECT m.msg_id, m.sender_id, m.receiver_id, m.type, m.content, m.media_path, "
        "m.img_w, m.img_h, m.ts, m.seq, m.file_id, m.file_size, m.content_type, m.sha256 FROM messages m JOIN ("
        "  SELECT conversation_id, MAX(id) AS mid FROM ("
        "    SELECT conversation_id, id FROM messages WHERE sender_id=?"
        "    UNION ALL"
        "    SELECT conversation_id, id FROM messages WHERE receiver_id=?"
        "  ) GROUP BY conversation_id"
        ") t ON m.id = t.mid ORDER BY m.ts DESC;",
        -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int(st, 1, userId);
    sqlite3_bind_int(st, 2, userId);
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(readMessageRow(st));
    sqlite3_finalize(st);
    return out;
}

std::vector<StoredMessage> Database::roamMessages(int userId, int peerId, std::int64_t beforeSeq, int limit)
{
    std::vector<StoredMessage> out;
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);

    const std::int64_t convId = makeConversationId(userId, peerId);

    // 会话内比游标更早的 N 条（seq 倒序），走 idx_msg_conv_seq
    sqlite3_stmt* st = nullptr;
    if(sqlite3_prepare_v2(c.db,
        "SELECT msg_id, sender_id, receiver_id, type, content, media_path, img_w, img_h, ts, seq, file_id, file_size, content_type, sha256 "
        "FROM messages WHERE conversation_id=? AND seq < ? ORDER BY seq DESC LIMIT ?;",
        -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int64(st, 1, convId);
    sqlite3_bind_int64(st, 2, beforeSeq);
    sqlite3_bind_int(st, 3, limit);
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(readMessageRow(st));
    sqlite3_finalize(st);
    return out;
}

bool Database::getMessageByMsgId(const std::string& msgId, StoredMessage& out)
{
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);
    sqlite3_stmt* st = nullptr;
    if(sqlite3_prepare_v2(c.db,
        "SELECT msg_id, sender_id, receiver_id, type, content, media_path, img_w, img_h, ts, seq, file_id, file_size, content_type, sha256 "
        "FROM messages WHERE msg_id=?;",
        -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, msgId.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* mid = sqlite3_column_text(st, 0);
        out.msgId = mid ? reinterpret_cast<const char*>(mid) : "";
        out.senderId = sqlite3_column_int(st, 1);
        out.receiverId = sqlite3_column_int(st, 2);
        out.type = sqlite3_column_int(st, 3);
        const unsigned char* content = sqlite3_column_text(st, 4);
        const unsigned char* path = sqlite3_column_text(st, 5);
        out.content = content ? reinterpret_cast<const char*>(content) : "";
        out.mediaPath = path ? reinterpret_cast<const char*>(path) : "";
        out.imgW = sqlite3_column_int(st, 6);
        out.imgH = sqlite3_column_int(st, 7);
        out.ts = sqlite3_column_int64(st, 8);
        out.seq = sqlite3_column_int64(st, 9);
        const unsigned char* fid = sqlite3_column_text(st, 10);
        out.fileId = fid ? reinterpret_cast<const char*>(fid) : "";
        out.fileSize = sqlite3_column_int64(st, 11);
        const unsigned char* contentType = sqlite3_column_text(st, 12);
        const unsigned char* sha256 = sqlite3_column_text(st, 13);
        out.contentType = contentType ? reinterpret_cast<const char*>(contentType) : "";
        out.sha256 = sha256 ? reinterpret_cast<const char*>(sha256) : "";
        ok = true;
    }
    sqlite3_finalize(st);
    return ok;
}

bool Database::getMessageByFileId(const std::string& fileId, StoredMessage& out){
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);
    sqlite3_stmt* st = nullptr;
    if(sqlite3_prepare_v2(c.db,
        "SELECT msg_id, sender_id, receiver_id, type, content, media_path, img_w, img_h, ts, seq, file_id, file_size, content_type, sha256 "
        "FROM messages WHERE file_id=?;",
        -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, fileId.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        // 按现有 getMessageByMsgId 的赋值逻辑填写 out
        const unsigned char* mid = sqlite3_column_text(st, 0);
        out.msgId = mid ? reinterpret_cast<const char*>(mid) : "";
        out.senderId = sqlite3_column_int(st, 1);
        out.receiverId = sqlite3_column_int(st, 2);
        out.type = sqlite3_column_int(st, 3);
        const unsigned char* content = sqlite3_column_text(st, 4);
        const unsigned char* path = sqlite3_column_text(st, 5);
        out.content = content ? reinterpret_cast<const char*>(content) : "";
        out.mediaPath = path ? reinterpret_cast<const char*>(path) : "";
        out.imgW = sqlite3_column_int(st, 6);
        out.imgH = sqlite3_column_int(st, 7);
        out.ts = sqlite3_column_int64(st, 8);
        out.seq = sqlite3_column_int64(st, 9);
        const unsigned char* fid = sqlite3_column_text(st, 10);
        out.fileId = fid ? reinterpret_cast<const char*>(fid) : "";
        out.fileSize = sqlite3_column_int64(st, 11);
        const unsigned char* contentType = sqlite3_column_text(st, 12);
        const unsigned char* sha256 = sqlite3_column_text(st, 13);
        out.contentType = contentType ? reinterpret_cast<const char*>(contentType) : "";
        out.sha256 = sha256 ? reinterpret_cast<const char*>(sha256) : "";
        ok = true;
    }
    sqlite3_finalize(st);
    return ok;
}

bool Database::createAuthSession(
    const std::string& sessionId, const std::string& familyId, int userId,
    const std::string& deviceId, const std::string& accessHash,
    std::int64_t accessExpiresAt, const std::string& refreshHash,
    std::int64_t refreshExpiresAt)
{
    return m_writeQueue.submit([&](sqlite3* db) -> bool {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO auth_sessions(session_id,family_id,user_id,device_id,access_hash,"
            "access_expires_at,refresh_hash,refresh_expires_at,generation,revoked,created_at,last_used_at) "
            "VALUES(?,?,?,?,?,?,?,?,1,0,?,?);", -1, &st, nullptr) != SQLITE_OK) return false;
        const auto now = static_cast<sqlite3_int64>(std::time(nullptr));
        sqlite3_bind_text(st, 1, sessionId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, familyId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 3, userId);
        sqlite3_bind_text(st, 4, deviceId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 5, accessHash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 6, accessExpiresAt);
        sqlite3_bind_text(st, 7, refreshHash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 8, refreshExpiresAt);
        sqlite3_bind_int64(st, 9, now);
        sqlite3_bind_int64(st, 10, now);
        const bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        return ok;
    });
}

namespace {
bool readAuthSession(sqlite3_stmt* st, Database::AuthSessionRecord& out)
{
    if (sqlite3_step(st) != SQLITE_ROW) return false;
    auto textAt = [st](int col) {
        const auto* value = sqlite3_column_text(st, col);
        return value ? std::string(reinterpret_cast<const char*>(value)) : std::string{};
    };
    out.sessionId = textAt(0);
    out.familyId = textAt(1);
    out.userId = sqlite3_column_int(st, 2);
    out.deviceId = textAt(3);
    out.accessHash = textAt(4);
    out.accessExpiresAt = sqlite3_column_int64(st, 5);
    out.refreshHash = textAt(6);
    out.refreshExpiresAt = sqlite3_column_int64(st, 7);
    out.generation = sqlite3_column_int(st, 8);
    out.revoked = sqlite3_column_int(st, 9) != 0;
    return true;
}
}

bool Database::findByAccessHash(const std::string& accessHash, AuthSessionRecord& out)
{
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(c.db,
        "SELECT session_id,family_id,user_id,device_id,access_hash,access_expires_at,"
        "refresh_hash,refresh_expires_at,generation,revoked FROM auth_sessions WHERE access_hash=?;",
        -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, accessHash.c_str(), -1, SQLITE_TRANSIENT);
    const bool found = readAuthSession(st, out);
    sqlite3_finalize(st);
    return found;
}

bool Database::findByRefreshHash(const std::string& refreshHash, AuthSessionRecord& out)
{
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(c.db,
        "SELECT session_id,family_id,user_id,device_id,access_hash,access_expires_at,"
        "refresh_hash,refresh_expires_at,generation,revoked FROM auth_sessions WHERE refresh_hash=?;",
        -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, refreshHash.c_str(), -1, SQLITE_TRANSIENT);
    const bool found = readAuthSession(st, out);
    sqlite3_finalize(st);
    return found;
}

bool Database::rotateRefreshToken(
    const AuthSessionRecord& oldSession, const std::string& oldRefreshHash,
    const std::string& newAccessHash, std::int64_t newAccessExpiresAt,
    const std::string& newRefreshHash, std::int64_t newRefreshExpiresAt)
{
    return m_writeQueue.submit([&](sqlite3* db) -> bool {
        if (!execOn(db, "BEGIN;")) return false;
        auto rollback = [&] { execOn(db, "ROLLBACK;"); };
        const auto now = static_cast<sqlite3_int64>(std::time(nullptr));

        sqlite3_stmt* history = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO auth_refresh_history(family_id,token_hash,generation,used_at) VALUES(?,?,?,?);",
            -1, &history, nullptr) != SQLITE_OK) { 
                rollback(); 
                return false; 
            }
        sqlite3_bind_text(history, 1, oldSession.familyId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(history, 2, oldRefreshHash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(history, 3, oldSession.generation);
        sqlite3_bind_int64(history, 4, now);
        const bool historyOk = sqlite3_step(history) == SQLITE_DONE;
        sqlite3_finalize(history);
        if (!historyOk) { rollback(); return false; }

        sqlite3_stmt* update = nullptr;
        if (sqlite3_prepare_v2(db,
            "UPDATE auth_sessions SET access_hash=?,access_expires_at=?,refresh_hash=?,"
            "refresh_expires_at=?,generation=generation+1,last_used_at=? "
            "WHERE session_id=? AND refresh_hash=? AND generation=? AND revoked=0;",
            -1, &update, nullptr) != SQLITE_OK) { 
                rollback(); 
                return false; 
            }
        sqlite3_bind_text(update, 1, newAccessHash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(update, 2, newAccessExpiresAt);
        sqlite3_bind_text(update, 3, newRefreshHash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(update, 4, newRefreshExpiresAt);
        sqlite3_bind_int64(update, 5, now);
        sqlite3_bind_text(update, 6, oldSession.sessionId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(update, 7, oldRefreshHash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(update, 8, oldSession.generation);
        const bool updateOk = sqlite3_step(update) == SQLITE_DONE && sqlite3_changes(db) == 1;
        sqlite3_finalize(update);
        if (!updateOk || !execOn(db, "COMMIT;")) { rollback(); return false; }
        return true;
    });
}

bool Database::wasRefreshTokenUsed(const std::string& refreshHash, std::string& outFamilyId)
{
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(c.db,
        "SELECT family_id FROM auth_refresh_history WHERE token_hash=?;", -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, refreshHash.c_str(), -1, SQLITE_TRANSIENT);
    const bool found = sqlite3_step(st) == SQLITE_ROW;
    if (found) {
        const auto* value = sqlite3_column_text(st, 0);
        outFamilyId = value ? reinterpret_cast<const char*>(value) : "";
    }
    sqlite3_finalize(st);
    return found;
}

int Database::revokeTokenFamily(const std::string& familyId)
{
    return m_writeQueue.submit([&](sqlite3* db) -> int {
        int userId = 0;
        sqlite3_stmt* qst = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT user_id FROM auth_sessions WHERE family_id=? LIMIT 1;", -1, &qst, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(qst, 1, familyId.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(qst) == SQLITE_ROW) userId = sqlite3_column_int(qst, 0);
            sqlite3_finalize(qst);
        }

        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "UPDATE auth_sessions SET revoked=1 WHERE family_id=?;", -1, &st, nullptr) != SQLITE_OK) return 0;
        sqlite3_bind_text(st, 1, familyId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
        return userId;
    });
}

void Database::revokeSession(const std::string& sessionId)
{
    m_writeQueue.submit([&](sqlite3* db) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "UPDATE auth_sessions SET revoked=1 WHERE session_id=?;", -1, &st, nullptr) != SQLITE_OK) return;
        sqlite3_bind_text(st, 1, sessionId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    });
}

void Database::revokeAllUserSessions(int userId)
{
    m_writeQueue.submit([&](sqlite3* db) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "UPDATE auth_sessions SET revoked=1 WHERE user_id=?;", -1, &st, nullptr) != SQLITE_OK) return;
        sqlite3_bind_int(st, 1, userId);
        sqlite3_step(st);
        sqlite3_finalize(st);
    });
}

} // namespace imsrv
