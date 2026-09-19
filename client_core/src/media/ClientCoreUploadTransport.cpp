#include "client_core/media/ClientCoreUploadTransport.h"

#if defined(CLIENT_CORE_WITH_MEDIA)

#include <cstdlib>

namespace im::media {

namespace {

UploadResult toResult(const ClientCore::MediaHttpResponse& r)
{
    UploadResult out;
    out.status = r.status;
    out.ok = r.status >= 200 && r.status < 300;
    // 4xx 是确定性拒绝（参数/授权/状态语义），5xx 与本地错误可恢复
    out.retryable = !(r.status >= 400 && r.status < 500);
    if (!out.ok) out.error = "http " + std::to_string(r.status);
    return out;
}

std::string jsonStr(const std::string& body, const std::string& key)
{
    const std::string pat = "\"" + key + "\":\"";
    auto pos = body.find(pat);
    if (pos == std::string::npos) return "";
    pos += pat.size();
    const auto end = body.find('"', pos);
    return end == std::string::npos ? "" : body.substr(pos, end - pos);
}

std::int64_t jsonInt(const std::string& body, const std::string& key, std::int64_t def = 0)
{
    const std::string pat = "\"" + key + "\":";
    auto pos = body.find(pat);
    if (pos == std::string::npos) return def;
    pos += pat.size();
    const auto end = body.find_first_of(",}", pos);
    try {
        return std::stoll(body.substr(pos, end == std::string::npos ? end : end - pos));
    } catch (...) {
        return def;
    }
}

bool jsonBool(const std::string& body, const std::string& key)
{
    return body.find("\"" + key + "\":true") != std::string::npos;
}

std::vector<int> jsonIntArray(const std::string& body, const std::string& key)
{
    std::vector<int> out;
    const std::string pat = "\"" + key + "\":[";
    auto lb = body.find(pat);
    if (lb == std::string::npos) return out;
    lb += pat.size();
    const auto rb = body.find(']', lb);
    if (rb == std::string::npos) return out;
    std::string cur;
    for (std::size_t i = lb; i < rb; ++i) {
        if (body[i] == ',') {
            if (!cur.empty()) { out.push_back(std::atoi(cur.c_str())); cur.clear(); }
        } else if (body[i] != ' ') {
            cur += body[i];
        }
    }
    if (!cur.empty()) out.push_back(std::atoi(cur.c_str()));
    return out;
}

void fillSession(const std::string& body, UploadSessionInfo& out)
{
    out.uploadId = jsonStr(body, "upload_id");
    out.chunkSize = jsonInt(body, "chunk_size");
    out.chunkCount = static_cast<int>(jsonInt(body, "chunk_count"));
    out.expiresAt = jsonInt(body, "expires_at");
    out.state = jsonStr(body, "state");
    if (out.state.empty()) out.state = "open";
    out.fileId = jsonStr(body, "file_id");
    out.received = jsonIntArray(body, "received");
}

} // namespace

UploadResult ClientCoreUploadTransport::createSession(const CreateSessionRequest& req,
                                                      UploadSessionInfo& out)
{
    const auto r = m_core.mediaHttpRequest("POST", "/api/v1/uploads",
        {{"X-File-Name", req.fileName},
         {"X-Receiver-Id", std::to_string(req.receiverId)},
         {"X-File-Size", std::to_string(req.fileSize)},
         {"X-File-Sha256", req.sha256}},
        "", req.contentType.empty() ? "application/octet-stream" : req.contentType);
    UploadResult res = toResult(r);
    if (!res.ok) return res;
    if (jsonBool(r.body, "instant")) {
        out.instant = true;
        out.challengeId = jsonStr(r.body, "challenge_id");
        out.proofOffset = jsonInt(r.body, "offset");
        out.proofLength = jsonInt(r.body, "length");
        return res;
    }
    out.instant = false;
    fillSession(r.body, out);
    if (out.uploadId.empty() || out.chunkSize <= 0 || out.chunkCount <= 0) {
        res.ok = false;
        res.retryable = false;
        res.error = "create 响应缺字段";
    }
    return res;
}

UploadResult ClientCoreUploadTransport::querySession(const std::string& uploadId,
                                                     UploadSessionInfo& out)
{
    const auto r = m_core.mediaHttpRequest("GET", "/api/v1/uploads/" + uploadId, {}, "", "");
    UploadResult res = toResult(r);
    if (!res.ok) return res;
    fillSession(r.body, out);
    return res;
}

UploadResult ClientCoreUploadTransport::uploadChunk(const std::string& uploadId, int index,
                                                    const std::string& chunkSha256,
                                                    const std::string& bytes,
                                                    const CancelFlag& cancel)
{
    if (cancel && cancel()) {
        UploadResult r; r.status = -1; r.retryable = false; r.error = "cancelled";
        return r;
    }
    const auto r = m_core.mediaHttpRequest("PUT",
        "/api/v1/uploads/" + uploadId + "/chunks/" + std::to_string(index),
        {{"X-Chunk-Sha256", chunkSha256}}, bytes, "application/octet-stream");
    if (r.status >= 200 && r.status < 300)
        m_bytesSent.fetch_add(static_cast<std::int64_t>(bytes.size()));
    return toResult(r);
}

UploadResult ClientCoreUploadTransport::prove(const std::string& challengeId,
                                              const std::string& bytes, std::string& fileIdOut)
{
    const auto r = m_core.mediaHttpRequest("POST", "/api/v1/upload/proof/" + challengeId,
                                           {}, bytes, "application/octet-stream");
    UploadResult res = toResult(r);
    if (res.ok) {
        fileIdOut = jsonStr(r.body, "file_id");
        if (fileIdOut.empty()) {
            res.ok = false;
            res.retryable = false;
            res.error = "proof 响应缺 file_id";
        } else {
            m_bytesSent.fetch_add(static_cast<std::int64_t>(bytes.size()));
        }
    }
    return res;
}

UploadResult ClientCoreUploadTransport::finalize(const std::string& uploadId,
                                                 std::string& fileIdOut)
{
    const auto r = m_core.mediaHttpRequest("POST", "/api/v1/uploads/" + uploadId + "/finalize",
                                           {}, "", "application/json");
    UploadResult res = toResult(r);
    if (res.ok) {
        fileIdOut = jsonStr(r.body, "file_id");
        if (fileIdOut.empty()) {
            res.ok = false;
            res.retryable = false;
            res.error = "finalize 响应缺 file_id";
        }
    }
    return res;
}

UploadResult ClientCoreUploadTransport::cancelSession(const std::string& uploadId)
{
    const auto r = m_core.mediaHttpRequest("DELETE", "/api/v1/uploads/" + uploadId, {}, "", "");
    return toResult(r);
}

} // namespace im::media

#endif // CLIENT_CORE_WITH_MEDIA
