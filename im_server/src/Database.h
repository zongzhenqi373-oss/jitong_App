#pragma once
// SQLite 数据访问层：连接池 + WAL 并发配置 + 全部 SQL
// 对应考察点：服务端 db 字段/索引设计、SQLite 写并发（WAL + busy_timeout）
//
// 并发说明：SQLite 同时刻只允许一个写者。读操作走连接池（每连接独立互斥锁，WAL 下读写/
// 读读互不阻塞）；写操作正在逐步收口到 DbWriteQueue（单写线程，见 DbWriteQueue.h）——
// 迁移到 DbWriteQueue 的写方法不再需要自己写 BEGIN IMMEDIATE 来防并发竞态，因为任意时刻
// 只可能有一个写操作在执行，这是架构上的天然保证。尚未迁移的写方法仍走连接池 + 事务兜底。

#include <mutex>
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

#include "DbWriteQueue.h"

struct sqlite3;

namespace imsrv {

// 用户/好友信息（对应 pb FriendInfo 的业务形态）
struct UserRecord {
    int id = 0;
    int iconId = 0;
    std::string nick;
    std::string feeling;
};

struct FriendRecord {
    int id = 0;
    int iconId = 0;
    int status = 1; // 0 在线 1 离线（由 Presence 填充，db 不存）
    std::string nick;
    std::string feeling;
};

struct FriendRequestRecord {
    int requesterId = 0;
    int targetId = 0;
    std::string requesterNick;
    std::string targetNick;
    std::int64_t createdAt = 0;
};

// 全量消息行（漫游/离线共用单表，is_delivered 区分）
struct StoredMessage {
    std::string msgId;
    int senderId = 0;
    int receiverId = 0;
    int type = 0;           // 0 文本 1 图片
    std::string content;    // 文本内容
    std::string mediaPath;  // 图片落盘路径
    int imgW = 0;
    int imgH = 0;
    std::int64_t ts = 0;
    std::int64_t seq = 0;   // 会话级单调递增序列号（服务端分配，接收方据此排序）
    std::string fileId;          // type=2 文件消息：服务端文件标识（=发送方 msg_id）
    std::int64_t fileSize = 0;   // type=2：文件字节数
    std::string contentType;
    std::string sha256;
};

class Database {
public:
    Database() = default;
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // 打开数据库（连接池大小 poolSize），建表 + WAL 配置；重复打开安全
    bool open(const std::string& dbPath, int poolSize = 2);

    // 首次运行播种：3 个种子用户（密码 123456 加盐哈希）+ 好友关系，幂等
    void seedIfEmpty();

    // ---------------- 注册/登录 ----------------
    // 返回 proto::REGISTER_SUCC / REGISTER_NICK_EXIT / REGISTER_TEL_EXIT（-1 表示 db 错误）
    // passHash 为客户端已算的 sha256(明文)；服务端再 盐+二次哈希 存库
    int registerUser(const std::string& nick, const std::string& tel, const std::string& passHash);

    // 校验登录：outUserId 输出用户 id；返回 proto::LOGIN_SUCCESS / LOGIN_NOTEXIT / LOGIN_PASSERROR
    int loginUser(const std::string& tel, const std::string& passHash, int& outUserId);

    // ---------------- 资料/好友 ----------------
    bool getUser(int id, UserRecord& out);
    bool isFriend(int idA, int idB);
    std::vector<FriendRecord> getFriends(int id);
    bool addFriendBidirectional(int idA, int idB);
    bool createFriendRequest(int requesterId, int targetId);
    std::vector<FriendRequestRecord> pendingFriendRequests(int userId);
    bool resolveFriendRequest(int requesterId, int targetId, bool accept);
    //删除好友
    bool removeFriendBidirectional(int idA, int idB);
    // 按昵称查用户 id（0=不存在）
    int getUserIdByNick(const std::string& nick);
    //根据用户id取出用户昵称
    std::string getUserNickById(int id);

    // ---------------- 消息（漫游/离线单表） ----------------
    // 保存消息；conversation_id 由收发双方 id 生成（min*K+max，双向同值）。
    // 服务端按会话分配单调递增 seq 并回填到 m.seq。
    bool saveMessage(StoredMessage& m, bool delivered);

    // 拉取未投递消息（按时间升序），并可标记为已投递
    std::vector<StoredMessage> pullUndelivered(int receiverId);
    void markDelivered(const std::vector<std::string>& msgIds);

    // ---------------- 消息漫游（M6） ----------------
    // 该用户每个会话的最后一条（会话列表预览，按 ts 倒序）
    std::vector<StoredMessage> roamConversations(int userId);
    // 某会话（userId↔peerId）比 beforeSeq 更早的 limit 条（seq 倒序）
    std::vector<StoredMessage> roamMessages(int userId, int peerId, std::int64_t beforeSeq, int limit);

    // 按 msg_id 取单条消息；不存在返回 false
    bool getMessageByMsgId(const std::string& msgId, StoredMessage& out);

    //按文件id取单条富媒体消息；不存在返回 false
    bool getMessageByFileId(const std::string& fileId, StoredMessage& out);

    // ---------------- 认证 ----------------
    struct AuthSessionRecord {
        std::string sessionId;
        std::string familyId;
        int userId = 0;
        std::string deviceId;
        std::string accessHash;
        std::int64_t accessExpiresAt = 0;
        std::string refreshHash;
        std::int64_t refreshExpiresAt = 0;
        int generation = 0;
        bool revoked = false;
    };

    // 创建认证会话
    bool createAuthSession(
        const std::string& sessionId,
        const std::string& familyId,
        int userId,
        const std::string& deviceId,
        const std::string& accessHash,
        std::int64_t accessExpiresAt,
        const std::string& refreshHash,
        std::int64_t refreshExpiresAt
    );

    // 根据 access_hash 查会话
    bool findByAccessHash(
        const std::string& accessHash,
        AuthSessionRecord& out
    );

    // 根据 refresh_hash 查会话
    bool findByRefreshHash(
        const std::string& refreshHash,
        AuthSessionRecord& out
    );

    // 刷新 refresh_token
    bool rotateRefreshToken(
        const AuthSessionRecord& oldSession,
        const std::string& oldRefreshHash,
        const std::string& newAccessHash,
        std::int64_t newAccessExpiresAt,
        const std::string& newRefreshHash,
        std::int64_t newRefreshExpiresAt
    );

    // 检查 refresh_token 是否已使用
    bool wasRefreshTokenUsed(
        const std::string& refreshHash,
        std::string& outFamilyId
    );

    // 撤销 token 家族；返回被撤销家族所属的 userId（找不到该家族则返回 0），
    // 供调用方据此立即踢掉这个用户当前在线的连接，而不是只等它自然过期
    int revokeTokenFamily(
        const std::string& familyId
    );

    // 撤销会话
    void revokeSession(
        const std::string& sessionId
    );

    // 撤销用户所有会话
    void revokeAllUserSessions(int userId);

private:
    struct Conn {
        sqlite3* db = nullptr;
        std::mutex mtx;
    };

    Conn& acquire(); // 轮询取连接（只读方法用）
    bool execOn(sqlite3* db, const char* sql);

    std::vector<std::unique_ptr<Conn>> m_pool;
    std::atomic<size_t> m_next{0};
    DbWriteQueue m_writeQueue; // 单写线程；写方法逐步从 acquire() 迁移到这里
};

// 生成会话 id，小id在高32位，大id在低32位，防止id碰撞，保证唯一性
inline std::int64_t makeConversationId(int idA, int idB)
{
    if(idA <= 0 || idB <= 0){
        return 0;
    }
    
    const std::uint32_t low = static_cast<std::uint32_t>(idA < idB ? idA : idB);
    const std::uint32_t high = static_cast<std::uint32_t>(idA > idB ? idA : idB);
    const std::uint64_t packed = (static_cast<std::int64_t>(low) << 32) | static_cast<std::int64_t>(high);

    return static_cast<std::int64_t>(packed);
}

} // namespace imsrv
