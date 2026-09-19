// 稳定 DTO（P7-G1 可测契约）。
//
// 目的：Legacy(Kotlin) 与 Native(C++) 对同一输入产生的领域对象，必须能逐字段 diff。
// 因此每个 DTO：
//   1. 带 `kVersion`，Golden 用例可检测契约变更；
//   2. 提供 `fields(prefix)` 返回 字段名 → 稳定字符串，供 testing/Diff.h 逐字段比较；
//   3. 字段**完整覆盖** Legacy 实体——P6 的教训是漏了 12 个缩略图字段导致对账失效，
//      因此这里显式列出全部媒体/缩略图字段，不允许"以后再补"。

#ifndef CLIENT_CORE_DTO_DTOS_H
#define CLIENT_CORE_DTO_DTOS_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "client_core/testing/Diff.h"

namespace im {
namespace dto {

/**
 * 消息状态。**必须与 001_initial.sql 与 Room MessageEntity.status 一致**：
 *   0=发送中 1=已送达(对方在线) 2=已接收 3=离线转存。
 * 注意：这里没有"Failed"值——发送失败是通过保留 outbox + 状态回退到 Sending 表达的，
 * 不是用一个独立状态号。不要用数值大小判断"状态前进"（2=已接收 并不比 1=已送达更前进）。
 */
enum class MessageStatus : std::int32_t {
    Sending = 0,      // 发送中
    Sent = 1,         // 已送达（对方在线）
    Received = 2,     // 已接收
    OfflineRelay = 3, // 离线转存
};

/** 传输方向。 */
enum class TransferDirection : std::int32_t { Upload = 0, Download = 1 };

/** 媒体变体档位（三档）。 */
enum class MediaVariant : std::int32_t { Origin = 0, LargeThumbnail = 1, SmallThumbnail = 2 };

/** 任务状态。 */
enum class TaskState : std::int32_t {
    Pending = 0,
    Running = 1,
    Success = 2,
    Failed = 3,
    Cancelled = 4,
};

/** 好友申请方向。 */
enum class RequestDirection : std::int32_t { Incoming = 0, Outgoing = 1 };

/** 好友申请状态。 */
enum class RequestState : std::int32_t { Pending = 0, Accepted = 1, Rejected = 2 };

/** AI 建议状态。 */
enum class AiStatus : std::int32_t { Pending = 0, Success = 1, Error = 2, Aborted = 3 };

// ---------------------------------------------------------------- 消息

struct MessageDto {
    static constexpr int kVersion = 1;

    std::int64_t ownerId = 0;      // 所属账号（消息按账号隔离，必填）
    std::string msgId;
    std::int64_t conversationId = 0;
    std::int64_t peerId = 0;
    std::int64_t seq = 0;          // 服务端 conversation_seq
    std::int64_t ts = 0;           // server_time
    std::int64_t localOrder = 0;   // 本地发送序号（server_time 相同时兜底排序）
    bool fromMe = false;
    std::int32_t type = 0;
    std::string content;
    std::int32_t status = 0;

    // 媒体（原始）
    std::string mediaPath;
    std::int32_t imgW = 0;
    std::int32_t imgH = 0;
    std::string fileId;
    std::string fileName;
    std::int64_t fileSize = 0;
    std::string contentType;
    std::string sha256;

    // 小缩略图（P6 曾漏，必须完整）
    std::string thumbnailFileId;
    std::string thumbnailPath;
    std::int64_t thumbnailSize = 0;
    std::string thumbnailSha256;
    std::int32_t thumbnailW = 0;
    std::int32_t thumbnailH = 0;

    // 大缩略图
    std::string largeThumbnailFileId;
    std::string largeThumbnailPath;
    std::int64_t largeThumbnailSize = 0;
    std::string largeThumbnailSha256;
    std::int32_t largeThumbnailW = 0;
    std::int32_t largeThumbnailH = 0;

    std::string localPath;
    std::int64_t transferred = 0;

    // 全文索引派生列（由 Native 拼音库生成；非权威数据，ADR-04）
    std::string pinyin;
    std::string initials;

    std::map<std::string, std::string> fields(const std::string& prefix = "") const
    {
        using im::testing::stableValue;
        std::map<std::string, std::string> f;
        f[prefix + "version"] = stableValue(kVersion);
        f[prefix + "pinyin"] = pinyin;
        f[prefix + "initials"] = initials;
        f[prefix + "ownerId"] = stableValue(ownerId);
        f[prefix + "msgId"] = msgId;
        f[prefix + "conversationId"] = stableValue(conversationId);
        f[prefix + "peerId"] = stableValue(peerId);
        f[prefix + "seq"] = stableValue(seq);
        f[prefix + "ts"] = stableValue(ts);
        f[prefix + "localOrder"] = stableValue(localOrder);
        f[prefix + "fromMe"] = stableValue(fromMe);
        f[prefix + "type"] = stableValue(type);
        f[prefix + "content"] = content;
        f[prefix + "status"] = stableValue(status);
        f[prefix + "mediaPath"] = mediaPath;
        f[prefix + "imgW"] = stableValue(imgW);
        f[prefix + "imgH"] = stableValue(imgH);
        f[prefix + "fileId"] = fileId;
        f[prefix + "fileName"] = fileName;
        f[prefix + "fileSize"] = stableValue(fileSize);
        f[prefix + "contentType"] = contentType;
        f[prefix + "sha256"] = sha256;
        f[prefix + "thumbnailFileId"] = thumbnailFileId;
        f[prefix + "thumbnailPath"] = thumbnailPath;
        f[prefix + "thumbnailSize"] = stableValue(thumbnailSize);
        f[prefix + "thumbnailSha256"] = thumbnailSha256;
        f[prefix + "thumbnailW"] = stableValue(thumbnailW);
        f[prefix + "thumbnailH"] = stableValue(thumbnailH);
        f[prefix + "largeThumbnailFileId"] = largeThumbnailFileId;
        f[prefix + "largeThumbnailPath"] = largeThumbnailPath;
        f[prefix + "largeThumbnailSize"] = stableValue(largeThumbnailSize);
        f[prefix + "largeThumbnailSha256"] = largeThumbnailSha256;
        f[prefix + "largeThumbnailW"] = stableValue(largeThumbnailW);
        f[prefix + "largeThumbnailH"] = stableValue(largeThumbnailH);
        f[prefix + "localPath"] = localPath;
        f[prefix + "transferred"] = stableValue(transferred);
        return f;
    }
};

// ---------------------------------------------------------------- 会话

struct ConversationDto {
    static constexpr int kVersion = 1;

    std::int64_t conversationId = 0;
    std::int64_t ownerId = 0;
    std::int64_t peerId = 0;
    std::string lastMsg;
    std::int64_t lastTs = 0;
    std::int64_t unread = 0;

    std::map<std::string, std::string> fields(const std::string& prefix = "") const
    {
        using im::testing::stableValue;
        std::map<std::string, std::string> f;
        f[prefix + "version"] = stableValue(kVersion);
        f[prefix + "conversationId"] = stableValue(conversationId);
        f[prefix + "ownerId"] = stableValue(ownerId);
        f[prefix + "peerId"] = stableValue(peerId);
        f[prefix + "lastMsg"] = lastMsg;
        f[prefix + "lastTs"] = stableValue(lastTs);
        f[prefix + "unread"] = stableValue(unread);
        return f;
    }
};

// ---------------------------------------------------------------- 好友

struct FriendDto {
    static constexpr int kVersion = 1;

    std::int64_t friendId = 0;
    std::string nick;
    std::string tel;
    std::string avatar;
    std::string signature;
    std::int32_t sex = 0;
    // Presence 只表达路由/在线能力，**不作为好友事实源**
    bool online = false;

    std::map<std::string, std::string> fields(const std::string& prefix = "") const
    {
        using im::testing::stableValue;
        std::map<std::string, std::string> f;
        f[prefix + "version"] = stableValue(kVersion);
        f[prefix + "friendId"] = stableValue(friendId);
        f[prefix + "nick"] = nick;
        f[prefix + "tel"] = tel;
        f[prefix + "avatar"] = avatar;
        f[prefix + "signature"] = signature;
        f[prefix + "sex"] = stableValue(sex);
        f[prefix + "online"] = stableValue(online);
        return f;
    }
};

// ---------------------------------------------------------------- 好友申请

struct FriendRequestDto {
    static constexpr int kVersion = 1;

    std::string requestId;
    std::int64_t fromUserId = 0;
    std::int64_t toUserId = 0;
    RequestDirection direction = RequestDirection::Incoming;
    RequestState state = RequestState::Pending;
    std::string message;
    std::int64_t createdAt = 0;

    std::map<std::string, std::string> fields(const std::string& prefix = "") const
    {
        using im::testing::stableValue;
        std::map<std::string, std::string> f;
        f[prefix + "version"] = stableValue(kVersion);
        f[prefix + "requestId"] = requestId;
        f[prefix + "fromUserId"] = stableValue(fromUserId);
        f[prefix + "toUserId"] = stableValue(toUserId);
        f[prefix + "direction"] = stableValue(static_cast<std::int32_t>(direction));
        f[prefix + "state"] = stableValue(static_cast<std::int32_t>(state));
        f[prefix + "message"] = message;
        f[prefix + "createdAt"] = stableValue(createdAt);
        return f;
    }
};

// ---------------------------------------------------------------- 搜索命中

struct SearchHit {
    static constexpr int kVersion = 1;

    std::string msgId;
    std::int64_t conversationId = 0;
    std::int64_t peerId = 0;
    std::int64_t ts = 0;
    std::string snippet;
    /** 高亮区间：编码单位必须明确，禁止跨端歧义 */
    enum class HighlightUnit { Utf16 = 0, Utf8 = 1 };
    HighlightUnit highlightUnit = HighlightUnit::Utf16;
    std::vector<std::pair<std::int32_t, std::int32_t>> highlightRanges; // [start, end)

    std::map<std::string, std::string> fields(const std::string& prefix = "") const
    {
        using im::testing::stableValue;
        std::map<std::string, std::string> f;
        f[prefix + "version"] = stableValue(kVersion);
        f[prefix + "msgId"] = msgId;
        f[prefix + "conversationId"] = stableValue(conversationId);
        f[prefix + "peerId"] = stableValue(peerId);
        f[prefix + "ts"] = stableValue(ts);
        f[prefix + "snippet"] = snippet;
        f[prefix + "highlightUnit"] =
            stableValue(static_cast<std::int32_t>(highlightUnit));
        std::string ranges;
        for (const auto& r : highlightRanges) {
            if (!ranges.empty()) ranges += ",";
            ranges += std::to_string(r.first) + ":" + std::to_string(r.second);
        }
        f[prefix + "highlightRanges"] = ranges;
        return f;
    }
};

// ---------------------------------------------------------------- 媒体任务

struct MediaTaskDto {
    static constexpr int kVersion = 1;

    std::string taskId;
    std::string msgId;
    TransferDirection direction = TransferDirection::Upload;
    MediaVariant variant = MediaVariant::Origin;
    TaskState state = TaskState::Pending;
    std::int64_t bytesTotal = 0;
    std::int64_t bytesDone = 0;
    std::string filePath;
    std::string fileHash;
    std::int32_t errorCode = 0;
    // 真实媒体元数据（由编码器/探测提供，禁止用路径后缀或占位值）
    std::string realMime;    // 如 image/avif、image/jpeg、image/png
    std::int32_t width = 0;
    std::int32_t height = 0;

    std::map<std::string, std::string> fields(const std::string& prefix = "") const
    {
        using im::testing::stableValue;
        std::map<std::string, std::string> f;
        f[prefix + "version"] = stableValue(kVersion);
        f[prefix + "taskId"] = taskId;
        f[prefix + "msgId"] = msgId;
        f[prefix + "direction"] = stableValue(static_cast<std::int32_t>(direction));
        f[prefix + "variant"] = stableValue(static_cast<std::int32_t>(variant));
        f[prefix + "state"] = stableValue(static_cast<std::int32_t>(state));
        f[prefix + "bytesTotal"] = stableValue(bytesTotal);
        f[prefix + "bytesDone"] = stableValue(bytesDone);
        f[prefix + "filePath"] = filePath;
        f[prefix + "fileHash"] = fileHash;
        f[prefix + "errorCode"] = stableValue(errorCode);
        return f;
    }
};

// ---------------------------------------------------------------- 分片上传草稿

/** 上传草稿状态机。 */
enum class UploadDraftState : std::int32_t {
    Pending = 0,    // 待传（已登记元数据，未建服务端会话）
    Uploading = 1,  // 会话已建，分片进行中
    Finalized = 2,  // 已得 file_id，消息卡片尚未入 Outbox
    Sent = 3,       // 消息已入 Outbox（ACK 由 Outbox 状态机负责）
    Cancelled = 4,  // 用户主动取消：终态，不自动恢复
    Failed = 5,     // 可恢复失败（断网/5xx）：重启后 resume
};

/**
 * 分片上传草稿：断点续传的本地事实源。
 * 图片的原图/大图/小图各占一行（variant 独立状态），共享同一 msgId。
 */
struct UploadDraftDto {
    static constexpr int kVersion = 1;

    std::string msgId;              // 固定消息 ID（重试/恢复不重新生成）
    MediaVariant variant = MediaVariant::Origin;
    std::int64_t conversationId = 0;
    std::int64_t peerId = 0;
    std::string localPath;
    std::string fileName;
    std::int64_t fileSize = 0;
    std::string sha256;
    std::string contentType;
    std::int32_t imageWidth = 0;
    std::int32_t imageHeight = 0;
    std::string uploadId;           // 空 = 尚未创建服务端会话
    std::int64_t chunkSize = 0;
    std::int32_t chunkCount = 0;
    std::vector<std::int32_t> chunksDone; // 已确认完成的分片号（升序）
    std::string fileId;             // finalize 后回填
    UploadDraftState state = UploadDraftState::Pending;
    std::int32_t errorCode = 0;
    std::int64_t createdAt = 0;
    std::int64_t updatedAt = 0;

    bool isTerminal() const {
        return state == UploadDraftState::Sent || state == UploadDraftState::Cancelled;
    }
    /** 重启后需要恢复的活跃状态（Sent/Cancelled 之外）。 */
    bool isActive() const {
        return state == UploadDraftState::Pending || state == UploadDraftState::Uploading ||
               state == UploadDraftState::Finalized || state == UploadDraftState::Failed;
    }
};

// ---------------------------------------------------------------- AI 建议

struct AiSuggestionDto {
    static constexpr int kVersion = 1;

    std::string requestId;
    std::int64_t conversationId = 0;
    std::int64_t peerId = 0;
    std::string tone;
    AiStatus status = AiStatus::Pending;
    std::vector<std::string> suggestions;
    std::int32_t errorCode = 0;
    std::int64_t generatedAt = 0;
    /** 冻结的上下文版本：上下文变化后旧结果不得展示 */
    std::string contextVersion;

    std::map<std::string, std::string> fields(const std::string& prefix = "") const
    {
        using im::testing::stableValue;
        std::map<std::string, std::string> f;
        f[prefix + "version"] = stableValue(kVersion);
        f[prefix + "requestId"] = requestId;
        f[prefix + "conversationId"] = stableValue(conversationId);
        f[prefix + "peerId"] = stableValue(peerId);
        f[prefix + "tone"] = tone;
        f[prefix + "status"] = stableValue(static_cast<std::int32_t>(status));
        std::string joined;
        for (const auto& s : suggestions) {
            if (!joined.empty()) joined += "|";
            joined += s;
        }
        f[prefix + "suggestions"] = joined;
        f[prefix + "errorCode"] = stableValue(errorCode);
        f[prefix + "generatedAt"] = stableValue(generatedAt);
        f[prefix + "contextVersion"] = contextVersion;
        return f;
    }
};

} // namespace dto
} // namespace im

#endif // CLIENT_CORE_DTO_DTOS_H
