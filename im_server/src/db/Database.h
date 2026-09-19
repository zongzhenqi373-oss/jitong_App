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

#include "db/DbWriteQueue.h"

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
    std::string thumbnailFileId;
    std::string thumbnailPath;
    std::int64_t thumbnailSize = 0;
    std::string thumbnailSha256;
    int thumbnailW = 0;
    int thumbnailH = 0;
    std::string largeThumbnailFileId;
    std::string largeThumbnailPath;
    std::int64_t largeThumbnailSize = 0;
    std::string largeThumbnailSha256;
    int largeThumbnailW = 0;
    int largeThumbnailH = 0;
};

class Database {
public:
    struct MediaObject {
        std::string path;
        std::string sha256;
        std::int64_t size = 0;
        std::string contentType;
    };
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
    bool bindDevicePublicKey(int userId, const std::string& deviceId,
                             const std::vector<std::uint8_t>& publicKeyDer);
    bool getDevicePublicKey(int userId, const std::string& deviceId,
                            std::vector<std::uint8_t>& outPublicKeyDer);

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
    bool findMediaObject(const std::string& sha256, std::int64_t size, MediaObject& out);

    // ---------------- 分片上传会话（断点续传） ----------------
    struct UploadSession {
        std::string uploadId;
        int uploaderId = 0;
        std::string deviceId;
        int receiverId = 0;
        std::string fileName;
        std::int64_t fileSize = 0;
        std::string sha256;
        std::string contentType;
        bool isImage = false;
        std::int64_t chunkSize = 0;
        int chunkCount = 0;
        std::int64_t expiresAt = 0;
        std::string state;   // open / finalized / cancelled
        std::string fileId;  // finalize 后回填
        std::string tmpPath;
        std::int64_t createdAt = 0;
    };

    enum class ChunkStoreResult {
        Stored,     // 首次接收并已持久化
        Duplicate,  // 同片同内容重试，幂等成功（未重复写）
        Conflict,   // 同片号不同内容，拒绝
        Rejected,   // 会话不可写（不存在/非 open/已过期）
    };

    /** 创建上传会话（open 状态）。 */
    bool createUploadSession(const UploadSession& s);
    /** 同人同设备同文件存在 open 会话时复用（create 重试幂等）。 */
    bool findOpenUploadSession(int uploaderId, const std::string& deviceId, int receiverId,
                               const std::string& sha256, std::int64_t size, UploadSession& out);
    bool getUploadSession(const std::string& uploadId, UploadSession& out);
    /**
     * 持久化一个已完成分片：同片同内容幂等 Duplicate；同片不同内容 Conflict；
     * 会话非 open/过期 Rejected。会话状态校验与分片写入在同一写事务完成。
     */
    ChunkStoreResult storeUploadChunk(const std::string& uploadId, int index,
                                      std::int64_t size, const std::string& sha256);
    /** 已完成分片编号（升序）。 */
    bool listUploadChunkIndices(const std::string& uploadId, std::vector<int>& out);
    /**
     * open→finalized 并回填 file_id（同一写事务）。
     * 已 finalized：幂等返回 true 且 existingFileId 为原 file_id；已取消/不存在：false。
     */
    bool finalizeUploadSession(const std::string& uploadId, const std::string& fileId,
                               std::string* existingFileId);
    /** open→cancelled；已 cancelled 幂等 true；finalized/不存在 false。 */
    bool cancelUploadSession(const std::string& uploadId);
    /** 过期但仍占临时文件的 open/cancelled 会话（GC 用）。 */
    bool listExpiredUploadSessions(std::int64_t now, std::vector<UploadSession>& out);
    /** 删除会话及其分片记录（GC 在临时文件移除成功后调用）。 */
    bool deleteUploadSession(const std::string& uploadId);
    /** 登记内容寻址媒体对象（finalize 即入库，秒传不依赖消息已发送）。 */
    bool registerMediaObject(const std::string& sha256, std::int64_t size,
                             const std::string& path, const std::string& contentType);

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
