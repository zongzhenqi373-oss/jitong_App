#pragma once

// 分片上传传输接口：MediaService 的网络边界。
// 生产实现走 ClientCore HTTP（TLS + token 鉴权）；测试注入 fake。
// 所有方法同步阻塞，由 MediaService 工作线程调用；取消经 CancelFlag 轮询。

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace im::media {

/** 传输结果。status<0 表示本地/网络错误（可恢复）；>=400 为服务端 HTTP 状态。 */
struct UploadResult {
    bool ok = false;
    bool retryable = true;  // false=确定性失败（4xx 语义错误），true=可恢复（断网/5xx/本地 IO）
    int status = 0;         // HTTP 状态或 -1 本地错误
    std::string error;      // 不含正文
};

/** 创建/查询会话的返回。instant=true 表示秒传预检命中（需再走 proof）。 */
struct UploadSessionInfo {
    bool instant = false;
    std::string challengeId;  // instant=true 时的 PoP 挑战
    std::int64_t proofOffset = 0;
    std::int64_t proofLength = 0;
    std::string uploadId;     // instant=false 时的服务端会话
    std::int64_t chunkSize = 0;
    int chunkCount = 0;
    std::int64_t expiresAt = 0;
    std::string state;        // query 时：open/finalized/cancelled
    std::string fileId;       // finalized 时回填
    std::vector<int> received; // 服务端已确认分片（恢复的事实源）
};

struct CreateSessionRequest {
    std::int64_t receiverId = 0;
    std::string fileName;
    std::int64_t fileSize = 0;
    std::string sha256;
    std::string contentType;
};

/** 取消轮询：返回 true 表示用户已取消，传输应尽快中止并返回 retryable=false。 */
using CancelFlag = std::function<bool()>;

class IUploadTransport {
public:
    virtual ~IUploadTransport() = default;

    /** 创建上传会话（内置秒传预检）；返回会话或 instant 挑战。 */
    virtual UploadResult createSession(const CreateSessionRequest& req,
                                       UploadSessionInfo& out) = 0;
    /** 查询会话（恢复时的事实源）。404 → ok=false, status=404。 */
    virtual UploadResult querySession(const std::string& uploadId, UploadSessionInfo& out) = 0;
    /**
     * 上传一个分片。chunk 内容经 readChunk 回调流式提供（避免整片常驻内存的强制要求
     * 由实现方决定；fake 可直接读 vector）。分片摘要由 MediaService 预计算传入。
     */
    virtual UploadResult uploadChunk(const std::string& uploadId, int index,
                                     const std::string& chunkSha256,
                                     const std::string& bytes, const CancelFlag& cancel) = 0;
    /** 秒传片段证明。 */
    virtual UploadResult prove(const std::string& challengeId, const std::string& bytes,
                               std::string& fileIdOut) = 0;
    /** finalize：全部完成后调用；返回 file_id。 */
    virtual UploadResult finalize(const std::string& uploadId, std::string& fileIdOut) = 0;
    /** 取消会话（用户主动取消后 best-effort 调用）。 */
    virtual UploadResult cancelSession(const std::string& uploadId) = 0;
};

} // namespace im::media
