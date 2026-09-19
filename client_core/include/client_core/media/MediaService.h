#pragma once

// 媒体上传编排（P7 断点续传 Native 侧）。
//
// 职责：把「待发送消息草稿（固定 msg_id）+ 各 variant 上传草稿」推进到终态。
// 状态机见 UploadDraftDto；恢复纪律：重启后先问服务端要已收分片，再只传缺失部分，
// 不能只信本地进度。取消（Cancelled）是终态不自动恢复；网络异常（Failed）可恢复。
//
// 消息发送：所有 variant 均 Finalized 后，以固定 msg_id 通过 NativeRepository
// commitOutgoingDraft 落 messages+Outbox（发送/重试/ACK 由 Outbox 状态机负责，
// 天然保证「文件传好了不重复发消息」）。
//
// 本类单线程语义：pump 系列方法由调用方串行驱动（生产为 MediaService 工作线程，
// 测试直接调用），不允许并发调用。

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "client_core/dto/Dtos.h"
#include "client_core/media/UploadTransport.h"

namespace im::storage { class NativeRepository; }
namespace im::dto { struct MessageDto; }

namespace im::media {

class MediaService {
public:
    /** 进度回调：(msgId, variant, 已完成分片数, 总分片数)。 */
    using ProgressFn = std::function<void(const std::string& msgId, int32_t variant,
                                          int done, int total)>;

    /** 单账号内核：ownerId 为该库属主（与 NativeDatabase 打开账号一致）。 */
    MediaService(std::int64_t ownerId, storage::NativeRepository& repo,
                 IUploadTransport& transport);

    void setProgressCallback(ProgressFn cb) { m_progress = std::move(cb); }

    /** 登记一条消息的上传草稿（可含多个 variant）。幂等：已存在保留进度。 */
    bool enqueue(const std::vector<im::dto::UploadDraftDto>& drafts, std::string* err = nullptr);

    /**
     * 推进一个 msgId 的全部活跃 variant 直到 Sent/Failed/无进展。
     * 返回 true 表示该消息已全部 Sent 或无活跃草稿。
     */
    bool pumpMessage(const std::string& msgId, std::string* err = nullptr);

    /** 恢复扫描：推进全部活跃草稿（启动/网络恢复时调用）。返回成功推进的 msgId 数。 */
    std::size_t resumeAll(std::string* err = nullptr);

    /**
     * 用户取消：草稿写终态 Cancelled 并 best-effort 通知服务端取消会话。
     * 与网络异常（Failed）严格区分：Cancelled 不再被 resumeAll 触碰。
     */
    bool cancel(const std::string& msgId, std::string* err = nullptr);

private:
    bool processVariant(const im::dto::UploadDraftDto& draft, std::string* err);
    /** 全部 variant Finalized 后，以固定 msg_id 落 messages+Outbox（幂等）。 */
    bool sendMessageCard(const std::string& msgId,
                         const std::vector<im::dto::UploadDraftDto>& variants,
                         std::string* err);
    bool readChunk(const im::dto::UploadDraftDto& draft, int index, std::string* out);
    void failDraft(const im::dto::UploadDraftDto& draft, int code);
    void reportProgress(const im::dto::UploadDraftDto& d);

    std::int64_t m_ownerId;
    storage::NativeRepository& m_repo;
    IUploadTransport& m_transport;
    ProgressFn m_progress;
};

} // namespace im::media
