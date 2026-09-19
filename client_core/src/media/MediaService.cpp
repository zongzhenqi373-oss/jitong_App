#include "client_core/media/MediaService.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>

#include "sha256.h"
#include "client_core/storage/NativeRepository.h"

namespace im::media {

namespace {

// 错误码（与 UploadResult.status 区分：这些是编排层语义码）
constexpr std::int32_t kErrNetwork = 1001;       // 可恢复网络/本地 IO 失败
constexpr std::int32_t kErrSessionLost = 1002;   // 服务端会话丢失后重建失败
constexpr std::int32_t kErrRejected = 1003;      // 服务端确定性拒绝（4xx）
constexpr std::int32_t kErrLocalIo = 1004;       // 本地文件不可读

bool isImageType(const std::string& contentType)
{
    return contentType.rfind("image/", 0) == 0;
}

} // namespace

MediaService::MediaService(std::int64_t ownerId, storage::NativeRepository& repo,
                           IUploadTransport& transport)
    : m_ownerId(ownerId), m_repo(repo), m_transport(transport) {}

bool MediaService::enqueue(const std::vector<im::dto::UploadDraftDto>& drafts, std::string* err)
{
    if (drafts.empty()) {
        if (err) *err = "empty drafts";
        return false;
    }
    const std::string msgId = drafts.front().msgId;
    for (const auto& d : drafts) {
        if (d.msgId != msgId) {
            if (err) *err = "drafts 必须共享同一 msg_id";
            return false;
        }
        if (!m_repo.upsertUploadDraft(m_ownerId, d, nullptr, err)) return false;
    }
    return true;
}

bool MediaService::readChunk(const im::dto::UploadDraftDto& draft, int index, std::string* out)
{
    const std::int64_t offset = static_cast<std::int64_t>(index) * draft.chunkSize;
    const std::int64_t size = std::min(draft.chunkSize, draft.fileSize - offset);
    if (size <= 0) return false;
    std::ifstream ifs(draft.localPath, std::ios::binary);
    if (!ifs) return false;
    ifs.seekg(offset, std::ios::beg);
    out->resize(static_cast<std::size_t>(size));
    ifs.read(out->data(), size);
    return ifs.gcount() == size;
}

void MediaService::failDraft(const im::dto::UploadDraftDto& draft, int code)
{
    std::string e;
    m_repo.setUploadDraftState(m_ownerId, draft.msgId, draft.variant,
                               im::dto::UploadDraftState::Failed, code, &e);
}

void MediaService::reportProgress(const im::dto::UploadDraftDto& d)
{
    if (m_progress) {
        m_progress(d.msgId, static_cast<std::int32_t>(d.variant),
                   static_cast<int>(d.chunksDone.size()), d.chunkCount);
    }
}

bool MediaService::processVariant(const im::dto::UploadDraftDto& draft, std::string* err)
{
    namespace dto = im::dto;
    if (draft.state == dto::UploadDraftState::Finalized) return true;
    if (draft.isTerminal()) return true;

    im::dto::UploadDraftDto cur = draft;

    // ---- 无会话：创建（内置秒传预检） ----
    if (cur.uploadId.empty()) {
        CreateSessionRequest req;
        req.receiverId = cur.peerId;
        req.fileName = cur.fileName;
        req.fileSize = cur.fileSize;
        req.sha256 = cur.sha256;
        req.contentType = cur.contentType;
        UploadSessionInfo session;
        const UploadResult r = m_transport.createSession(req, session);
        if (!r.ok) {
            failDraft(cur, r.retryable ? kErrNetwork : kErrRejected);
            if (err) *err = r.error;
            return false;
        }
        if (session.instant) {
            // 秒传：读取证明片段并完成 PoP
            std::string proof;
            {
                std::ifstream ifs(cur.localPath, std::ios::binary);
                if (!ifs) { failDraft(cur, kErrLocalIo); return false; }
                ifs.seekg(session.proofOffset, std::ios::beg);
                proof.resize(static_cast<std::size_t>(session.proofLength));
                ifs.read(proof.data(), session.proofLength);
                if (ifs.gcount() != session.proofLength) { failDraft(cur, kErrLocalIo); return false; }
            }
            std::string fileId;
            const UploadResult pr = m_transport.prove(session.challengeId, proof, fileId);
            if (!pr.ok) {
                failDraft(cur, pr.retryable ? kErrNetwork : kErrRejected);
                if (err) *err = pr.error;
                return false;
            }
            return m_repo.setUploadDraftFileId(m_ownerId, cur.msgId, cur.variant, fileId, err);
        }
        if (!m_repo.setUploadDraftSession(m_ownerId, cur.msgId, cur.variant, session.uploadId,
                                          session.chunkSize, session.chunkCount, err)) {
            return false;
        }
        // 复用会话：服务端已收分片合并进本地（服务端为准）
        for (int idx : session.received) {
            std::string e;
            m_repo.markUploadChunkDone(m_ownerId, cur.msgId, cur.variant, idx, &e);
        }
        if (!m_repo.getUploadDraft(m_ownerId, cur.msgId, cur.variant, &cur, err)) return false;
        reportProgress(cur);
    }

    // ---- 有会话：恢复纪律 = 先问服务端，再传缺失 ----
    {
        UploadSessionInfo server;
        const UploadResult q = m_transport.querySession(cur.uploadId, server);
        if (q.ok) {
            if (server.state == "finalized" && !server.fileId.empty()) {
                // 客户端 finalize 后未收到响应的场景：服务端已完成，直接采用
                return m_repo.setUploadDraftFileId(m_ownerId, cur.msgId, cur.variant,
                                                   server.fileId, err);
            }
            if (server.state == "cancelled") {
                // 服务端会话已被取消（如过期 GC）：换新会话重来
                CreateSessionRequest req;
                req.receiverId = cur.peerId; req.fileName = cur.fileName;
                req.fileSize = cur.fileSize; req.sha256 = cur.sha256;
                req.contentType = cur.contentType;
                UploadSessionInfo fresh;
                const UploadResult cr = m_transport.createSession(req, fresh);
                if (!cr.ok || fresh.instant) {
                    failDraft(cur, kErrSessionLost);
                    return false;
                }
                if (!m_repo.resetUploadDraftForNewSession(m_ownerId, cur.msgId, cur.variant,
                                                          fresh.uploadId, fresh.chunkSize,
                                                          fresh.chunkCount, err)) return false;
                if (!m_repo.getUploadDraft(m_ownerId, cur.msgId, cur.variant, &cur, err))
                    return false;
            } else {
                // open：以服务端已收分片为准合并本地
                for (int idx : server.received) {
                    std::string e;
                    m_repo.markUploadChunkDone(m_ownerId, cur.msgId, cur.variant, idx, &e);
                }
            }
        } else if (q.status == 404 || q.status == 410) {
            // 服务端会话不存在/已清理：换新会话并清空进度
            CreateSessionRequest req;
            req.receiverId = cur.peerId; req.fileName = cur.fileName;
            req.fileSize = cur.fileSize; req.sha256 = cur.sha256;
            req.contentType = cur.contentType;
            UploadSessionInfo fresh;
            const UploadResult cr = m_transport.createSession(req, fresh);
            if (!cr.ok || fresh.instant) {
                failDraft(cur, cr.retryable ? kErrSessionLost : kErrRejected);
                return false;
            }
            if (!m_repo.resetUploadDraftForNewSession(m_ownerId, cur.msgId, cur.variant,
                                                      fresh.uploadId, fresh.chunkSize,
                                                      fresh.chunkCount, err)) return false;
            if (!m_repo.getUploadDraft(m_ownerId, cur.msgId, cur.variant, &cur, err))
                return false;
        } else if (!q.ok) {
            failDraft(cur, q.retryable ? kErrNetwork : kErrRejected);
            if (err) *err = q.error;
            return false;
        }
    }

    // ---- Failed（可恢复）且会话有效：转回 Uploading 继续 ----
    if (cur.state == dto::UploadDraftState::Failed && !cur.uploadId.empty()) {
        if (!m_repo.setUploadDraftState(m_ownerId, cur.msgId, cur.variant,
                                        dto::UploadDraftState::Uploading, 0, err)) return false;
        if (!m_repo.getUploadDraft(m_ownerId, cur.msgId, cur.variant, &cur, err)) return false;
    }

    // ---- 上传缺失分片 ----
    if (cur.state == dto::UploadDraftState::Uploading) {
        std::set<int> done(cur.chunksDone.begin(), cur.chunksDone.end());
        for (int i = 0; i < cur.chunkCount; ++i) {
            if (done.count(i)) continue;
            // 用户可能在 pump 期间取消：每片前复查状态
            im::dto::UploadDraftDto fresh;
            if (!m_repo.getUploadDraft(m_ownerId, cur.msgId, cur.variant, &fresh, err))
                return false;
            if (fresh.state != dto::UploadDraftState::Uploading) {
                cur = fresh;
                break; // 已取消或状态被外部推进
            }
            std::string bytes;
            if (!readChunk(cur, i, &bytes)) {
                failDraft(cur, kErrLocalIo);
                if (err) *err = "本地文件读取失败";
                return false;
            }
            const std::string chunkSha = im::sha256Hex(bytes);
            const UploadResult r = m_transport.uploadChunk(cur.uploadId, i, chunkSha, bytes,
                                                           CancelFlag{});
            if (!r.ok) {
                failDraft(cur, r.retryable ? kErrNetwork : kErrRejected);
                if (err) *err = r.error;
                return false;
            }
            if (!m_repo.markUploadChunkDone(m_ownerId, cur.msgId, cur.variant, i, err))
                return false;
            cur.chunksDone.push_back(i);
            reportProgress(cur);
        }
        if (!m_repo.getUploadDraft(m_ownerId, cur.msgId, cur.variant, &cur, err)) return false;
    }

    // ---- 齐片后 finalize ----
    if (cur.state == dto::UploadDraftState::Uploading &&
        static_cast<int>(cur.chunksDone.size()) == cur.chunkCount) {
        std::string fileId;
        const UploadResult f = m_transport.finalize(cur.uploadId, fileId);
        if (!f.ok) {
            failDraft(cur, f.retryable ? kErrNetwork : kErrRejected);
            if (err) *err = f.error;
            return false;
        }
        return m_repo.setUploadDraftFileId(m_ownerId, cur.msgId, cur.variant, fileId, err);
    }
    return true;
}

bool MediaService::sendMessageCard(const std::string& msgId,
                                   const std::vector<im::dto::UploadDraftDto>& variants,
                                   std::string* err)
{
    namespace dto = im::dto;
    // 幂等：消息已存在（重试/已发未收 ACK 场景）→ 仅把草稿推进 Sent
    dto::MessageDto existing;
    if (m_repo.findMessage(m_ownerId, msgId, &existing)) {
        for (const auto& v : variants) {
            std::string e;
            m_repo.setUploadDraftState(m_ownerId, msgId, v.variant,
                                       dto::UploadDraftState::Sent, 0, &e);
        }
        return true;
    }

    const dto::UploadDraftDto* origin = nullptr;
    const dto::UploadDraftDto* large = nullptr;
    const dto::UploadDraftDto* small = nullptr;
    for (const auto& v : variants) {
        if (v.variant == dto::MediaVariant::Origin) origin = &v;
        else if (v.variant == dto::MediaVariant::LargeThumbnail) large = &v;
        else if (v.variant == dto::MediaVariant::SmallThumbnail) small = &v;
    }
    if (!origin || origin->fileId.empty()) {
        if (err) *err = "origin variant 未就绪";
        return false;
    }

    dto::MessageDto m;
    m.ownerId = m_ownerId;
    m.msgId = msgId; // 固定 msg_id：重试不重复发消息的事实源
    m.conversationId = origin->conversationId;
    m.peerId = origin->peerId;
    m.fromMe = true;
    m.type = isImageType(origin->contentType) ? 1 : 2;
    m.status = 0; // Sending
    m.mediaPath = origin->localPath;
    m.fileId = origin->fileId;
    m.fileName = origin->fileName;
    m.fileSize = origin->fileSize;
    m.contentType = origin->contentType;
    m.sha256 = origin->sha256;
    m.imgW = origin->imageWidth;
    m.imgH = origin->imageHeight;
    if (large) {
        m.largeThumbnailFileId = large->fileId;
        m.largeThumbnailPath = large->localPath;
        m.largeThumbnailSize = large->fileSize;
        m.largeThumbnailSha256 = large->sha256;
        m.largeThumbnailW = large->imageWidth;
        m.largeThumbnailH = large->imageHeight;
    }
    if (small) {
        m.thumbnailFileId = small->fileId;
        m.thumbnailPath = small->localPath;
        m.thumbnailSize = small->fileSize;
        m.thumbnailSha256 = small->sha256;
        m.thumbnailW = small->imageWidth;
        m.thumbnailH = small->imageHeight;
    }
    if (!m_repo.commitOutgoingDraft(m, err)) return false;
    for (const auto& v : variants) {
        std::string e;
        if (!m_repo.setUploadDraftState(m_ownerId, msgId, v.variant,
                                        dto::UploadDraftState::Sent, 0, &e)) {
            // 消息已提交，状态推进失败不致命；下次 resume 会从 findMessage 幂等补齐
            if (err) *err = e;
        }
    }
    return true;
}

bool MediaService::pumpMessage(const std::string& msgId, std::string* err)
{
    namespace dto = im::dto;
    // 聚合该消息的全部 variant 草稿
    std::vector<dto::UploadDraftDto> variants;
    for (auto v : {dto::MediaVariant::Origin, dto::MediaVariant::LargeThumbnail,
                   dto::MediaVariant::SmallThumbnail}) {
        dto::UploadDraftDto d;
        std::string e;
        if (m_repo.getUploadDraft(m_ownerId, msgId, v, &d, &e)) variants.push_back(d);
    }
    if (variants.empty()) {
        if (err) *err = "草稿不存在";
        return false;
    }

    bool allTerminal = true;
    bool anyCancelled = false;
    bool allSent = true;
    for (const auto& v : variants) {
        if (v.state == dto::UploadDraftState::Cancelled) anyCancelled = true;
        if (v.state != dto::UploadDraftState::Sent) allSent = false;
        if (!v.isTerminal()) allTerminal = false;
    }
    // 已取消：明确拒绝（与「全部已发」的幂等成功区分，调用方能感知语义差异）
    if (anyCancelled) {
        if (err) *err = "草稿已取消（终态）";
        return false;
    }
    if (allTerminal) return allSent;

    // 逐 variant 推进到 Finalized
    bool allFinalized = true;
    std::vector<dto::UploadDraftDto> latest;
    for (auto& v : variants) {
        if (!processVariant(v, err)) {
            latest.push_back(v);
            allFinalized = false;
            continue;
        }
        dto::UploadDraftDto cur;
        std::string e;
        if (m_repo.getUploadDraft(m_ownerId, msgId, v.variant, &cur, &e)) latest.push_back(cur);
        if (cur.state != dto::UploadDraftState::Finalized &&
            cur.state != dto::UploadDraftState::Sent) allFinalized = false;
    }
    // 全部 Finalized → 以固定 msg_id 发消息卡片
    if (allFinalized) return sendMessageCard(msgId, latest, err);
    return false;
}

std::size_t MediaService::resumeAll(std::string* err)
{
    std::vector<im::dto::UploadDraftDto> active;
    if (!m_repo.loadActiveUploadDrafts(m_ownerId, &active, err)) return 0;
    std::set<std::string> msgIds;
    for (const auto& d : active) msgIds.insert(d.msgId);
    std::size_t progressed = 0;
    for (const auto& id : msgIds) {
        std::string e;
        if (pumpMessage(id, &e)) ++progressed;
        else if (err && err->empty() && !e.empty()) *err = e;
    }
    return progressed;
}

bool MediaService::cancel(const std::string& msgId, std::string* err)
{
    namespace dto = im::dto;
    bool found = false;
    for (auto v : {dto::MediaVariant::Origin, dto::MediaVariant::LargeThumbnail,
                   dto::MediaVariant::SmallThumbnail}) {
        dto::UploadDraftDto d;
        std::string e;
        if (!m_repo.getUploadDraft(m_ownerId, msgId, v, &d, &e)) continue;
        found = true;
        if (d.isTerminal()) continue;
        // 终态 Cancelled：本地先行，服务端取消 best-effort
        if (!m_repo.setUploadDraftState(m_ownerId, msgId, v, dto::UploadDraftState::Cancelled,
                                        0, &e)) {
            if (err) *err = e;
            return false;
        }
        if (!d.uploadId.empty()) m_transport.cancelSession(d.uploadId);
    }
    if (!found && err) *err = "草稿不存在";
    return found;
}

} // namespace im::media
