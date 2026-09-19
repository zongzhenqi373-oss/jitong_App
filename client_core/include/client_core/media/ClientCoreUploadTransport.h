#pragma once

// 生产分片上传传输：经 ClientCore 的带鉴权媒体 HTTP 通道访问服务端
// /api/v1/uploads/* 协议。仅在 CLIENT_CORE_WITH_MEDIA 下编译。

#if defined(CLIENT_CORE_WITH_MEDIA)

#include <atomic>

#include "client_core/ClientCore.h"
#include "client_core/media/UploadTransport.h"

namespace im::media {

class ClientCoreUploadTransport : public IUploadTransport {
public:
    explicit ClientCoreUploadTransport(ClientCore& core) : m_core(core) {}

    /** 实际经网络发送的分片/证明字节数（验收指标：首传/续传/秒传对比）。 */
    std::int64_t bytesSent() const { return m_bytesSent.load(); }

    UploadResult createSession(const CreateSessionRequest& req, UploadSessionInfo& out) override;
    UploadResult querySession(const std::string& uploadId, UploadSessionInfo& out) override;
    UploadResult uploadChunk(const std::string& uploadId, int index,
                             const std::string& chunkSha256, const std::string& bytes,
                             const CancelFlag& cancel) override;
    UploadResult prove(const std::string& challengeId, const std::string& bytes,
                       std::string& fileIdOut) override;
    UploadResult finalize(const std::string& uploadId, std::string& fileIdOut) override;
    UploadResult cancelSession(const std::string& uploadId) override;

private:
    ClientCore& m_core;
    std::atomic<std::int64_t> m_bytesSent{0};
};

} // namespace im::media

#endif // CLIENT_CORE_WITH_MEDIA
