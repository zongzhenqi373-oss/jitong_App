#include "media/HttpFileServer.h"

#include "core/Server.h"
#include "db/Database.h"
#include "common/Log.h"
#include "media/MediaUtil.h"
#include "auth/TokenService.h"
#include "sha256.h"
#include "image_format.h"
#include "client_core/Protocol.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <vector>

namespace imsrv {

namespace {

// 按扩展名猜一个 Content-Type（下载响应用；上传时的真实类型来自客户端 Content-Type 头）
std::string guessContentType(const std::string& path)
{
    const std::string ext = std::filesystem::path(path).extension().string();
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".bmp") return "image/bmp";
    if (ext == ".webp") return "image/webp";
    if (ext == ".avif") return "image/avif";
    return "application/octet-stream";
}

std::int64_t nowSec() { return static_cast<std::int64_t>(std::time(nullptr)); }

// 排空未消费的请求体：分片端点在鉴权/校验失败时必须读完再响应，
// 否则客户端仍在写大 body，连接被 RST 后客户端只能看到 EPIPE 而非错误码。
void drainBody(const httplib::ContentReader& reader)
{
    reader([](const char*, std::size_t) { return true; });
}

} // namespace

HttpFileServer::HttpFileServer(Server& server, std::uint16_t port, std::string certPath, std::string keyPath)
    : m_server(server)
    , m_port(port)
    , m_certPath(std::move(certPath))
    , m_keyPath(std::move(keyPath))
    , m_svr(m_certPath.c_str(), m_keyPath.c_str())
{
    m_svr.Post("/api/v1/upload",
        [this](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader) {
            handleUpload(req, res, reader);
        });
    m_svr.Post("/api/v1/upload/preflight",
        [this](const httplib::Request& req, httplib::Response& res) { handlePreflight(req, res); });
    m_svr.Post(R"(/api/v1/upload/proof/([a-zA-Z0-9._-]+))",
        [this](const httplib::Request& req, httplib::Response& res) { handleProof(req, res); });
    m_svr.Get(R"(/api/v1/download/([a-zA-Z0-9._-]+))",
        [this](const httplib::Request& req, httplib::Response& res) { handleDownload(req, res); });

    // 分片上传会话（断点续传）。upload_id 是 64 位小写 hex（服务端生成）。
    m_svr.Post("/api/v1/uploads",
        [this](const httplib::Request& req, httplib::Response& res) { handleCreateUpload(req, res); });
    m_svr.Put(R"(/api/v1/uploads/([0-9a-f]{64})/chunks/(\d+))",
        [this](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader) {
            handleUploadChunk(req, res, reader);
        });
    m_svr.Get(R"(/api/v1/uploads/([0-9a-f]{64}))",
        [this](const httplib::Request& req, httplib::Response& res) { handleGetUpload(req, res); });
    m_svr.Post(R"(/api/v1/uploads/([0-9a-f]{64})/finalize)",
        [this](const httplib::Request& req, httplib::Response& res) { handleFinalizeUpload(req, res); });
    m_svr.Delete(R"(/api/v1/uploads/([0-9a-f]{64}))",
        [this](const httplib::Request& req, httplib::Response& res) { handleCancelUpload(req, res); });
}

// ---------------------------------------------------------------------------
// 分片上传会话（秒传优先、未命中分片上传、断点续传）
// ---------------------------------------------------------------------------

std::string HttpFileServer::issueInstantChallenge(int userId, int receiverId,
                                                  const std::string& sha, std::int64_t size)
{
    Database::MediaObject object;
    if (!m_server.db().findMediaObject(sha, size, object)) return "";
    constexpr std::int64_t kProofBytes = 64 * 1024;
    const std::int64_t length = std::min(kProofBytes, size);
    const std::string seed = im::sha256Hex(std::to_string(userId) + "|" + sha + "|" +
                                           std::to_string(nowSec()));
    const std::uint64_t n = std::stoull(seed.substr(0, 16), nullptr, 16);
    const std::int64_t offset = size > length ? static_cast<std::int64_t>(n % (size - length + 1)) : 0;
    const std::string challengeId = im::sha256Hex(seed + "|" + std::to_string(offset));
    {
        std::lock_guard<std::mutex> lg(m_uploadMtx);
        m_challenges[challengeId] = ProofChallenge{userId, receiverId, object.path, sha, size,
            object.contentType, offset, length, nowSec()};
    }
    return "{\"instant\":true,\"challenge_id\":\"" + challengeId +
        "\",\"offset\":" + std::to_string(offset) + ",\"length\":" + std::to_string(length) + "}";
}

void HttpFileServer::handleCreateUpload(const httplib::Request& req, httplib::Response& res)
{
    int userId = 0;
    std::string deviceId;
    if (!authenticate(req, userId, deviceId)) { res.status = 401; return; }

    const std::string rawName = req.get_header_value("X-File-Name");
    const std::string safeName = std::filesystem::path(rawName).filename().string();
    int receiverId = 0;
    std::int64_t size = 0;
    try {
        receiverId = std::stoi(req.get_header_value("X-Receiver-Id"));
        size = std::stoll(req.get_header_value("X-File-Size"));
    } catch (...) { res.status = 400; return; }
    const std::string sha = req.get_header_value("X-File-Sha256");
    if (safeName.empty() || safeName.size() > 255 || sha.size() != 64 ||
        size <= 0 || size > im::proto::FILE_MAX_SIZE) { res.status = 400; return; }
    if (!m_server.db().isFriend(userId, receiverId)) { res.status = 403; return; }

    // 1) 秒传预检：命中则直接发 PoP 挑战，不创建会话
    const std::string instant = issueInstantChallenge(userId, receiverId, sha, size);
    if (!instant.empty()) {
        res.set_content(instant, "application/json");
        return;
    }

    // 2) 同人同设备同文件已有 open 会话：复用（create 重试幂等），返回已接收分片
    Database::UploadSession session;
    if (!m_server.db().findOpenUploadSession(userId, deviceId, receiverId, sha, size, session)) {
        constexpr std::int64_t kChunkSize = 1024 * 1024; // 1 MiB
        static std::atomic<std::uint64_t> counter{0};
        session.uploadId = im::sha256Hex(std::to_string(userId) + "|" + deviceId + "|" +
            std::to_string(receiverId) + "|" + sha + "|" + std::to_string(size) + "|" +
            std::to_string(nowSec()) + "|" + std::to_string(counter.fetch_add(1)));
        session.uploaderId = userId;
        session.deviceId = deviceId;
        session.receiverId = receiverId;
        session.fileName = safeName;
        session.fileSize = size;
        session.sha256 = sha;
        session.contentType = req.get_header_value("Content-Type");
        session.isImage = session.contentType.rfind("image/", 0) == 0;
        session.chunkSize = kChunkSize;
        session.chunkCount = static_cast<int>((size + kChunkSize - 1) / kChunkSize);
        session.expiresAt = nowSec() + 24 * 3600;
        session.state = "open";
        session.tmpPath = m_server.uploadDir() + "/chunks/" + session.uploadId + ".part";
        session.createdAt = nowSec();
        if (!m_server.db().createUploadSession(session)) {
            res.status = 500;
            return;
        }
    }

    std::vector<int> received;
    m_server.db().listUploadChunkIndices(session.uploadId, received);
    std::string receivedJson = "[";
    for (std::size_t i = 0; i < received.size(); ++i) {
        if (i) receivedJson += ",";
        receivedJson += std::to_string(received[i]);
    }
    receivedJson += "]";
    res.set_content("{\"instant\":false,\"upload_id\":\"" + session.uploadId +
        "\",\"chunk_size\":" + std::to_string(session.chunkSize) +
        ",\"chunk_count\":" + std::to_string(session.chunkCount) +
        ",\"expires_at\":" + std::to_string(session.expiresAt) +
        ",\"received\":" + receivedJson + "}", "application/json");
}

void HttpFileServer::handleUploadChunk(const httplib::Request& req, httplib::Response& res,
                                       const httplib::ContentReader& reader)
{
    int userId = 0;
    std::string deviceId;
    if (!authenticate(req, userId, deviceId)) { drainBody(reader); res.status = 401; return; }

    const std::string uploadId = req.matches[1];
    int index = -1;
    try { index = std::stoi(req.matches[2]); } catch (...) { drainBody(reader); res.status = 400; return; }

    Database::UploadSession session;
    if (!m_server.db().getUploadSession(uploadId, session)) { drainBody(reader); res.status = 404; return; }
    if (session.uploaderId != userId || session.deviceId != deviceId) {
        drainBody(reader); res.status = 403; return; // upload_id 不是授权凭证，不能凭 ID 给别人续传
    }
    if (session.state == "cancelled") { drainBody(reader); res.status = 410; return; }
    if (session.state != "open") { drainBody(reader); res.status = 409; return; }
    if (session.expiresAt < nowSec()) { drainBody(reader); res.status = 410; return; }
    if (index < 0 || index >= session.chunkCount) { drainBody(reader); res.status = 400; return; }

    // 服务端按会话元数据计算该片应有大小，不信任客户端自报偏移
    const std::int64_t expectedSize =
        std::min(session.chunkSize, session.fileSize - index * session.chunkSize);
    std::int64_t declared = -1;
    try { declared = std::stoll(req.get_header_value("Content-Length")); } catch (...) {}
    if (declared != expectedSize) { drainBody(reader); res.status = 400; return; }
    const std::string chunkSha = req.get_header_value("X-Chunk-Sha256");
    if (chunkSha.size() != 64) { drainBody(reader); res.status = 400; return; }

    // 收流并算摘要（分片上限 1MiB，整载入内存可控）
    std::string body;
    body.reserve(static_cast<std::size_t>(expectedSize));
    im::Sha256 hasher;
    const bool readOk = reader([&](const char* data, std::size_t len) -> bool {
        if (body.size() + len > static_cast<std::size_t>(expectedSize)) return false;
        body.append(data, len);
        hasher.update(data, len);
        return true;
    });
    if (!readOk || static_cast<std::int64_t>(body.size()) != expectedSize) {
        res.status = 400;
        return;
    }
    static const char* hex = "0123456789abcdef";
    std::string actual;
    actual.reserve(64);
    for (unsigned char b : hasher.final()) {
        actual.push_back(hex[(b >> 4) & 0xF]);
        actual.push_back(hex[b & 0xF]);
    }
    if (actual != chunkSha) { res.status = 400; return; } // 摘要与内容不符

    // 幂等判定在写事务内完成：Stored/Duplicate/Conflict/Rejected
    const auto store = m_server.db().storeUploadChunk(uploadId, index, expectedSize, chunkSha);
    if (store == Database::ChunkStoreResult::Conflict) { res.status = 409; return; }
    if (store == Database::ChunkStoreResult::Rejected) { res.status = 409; return; }
    if (store == Database::ChunkStoreResult::Stored) {
        // 按编号定位写入（seekp），不按客户端偏移追加；先落盘成功才算完成
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(session.tmpPath).parent_path(), ec);
        std::fstream fs(session.tmpPath,
            std::ios::binary | std::ios::in | std::ios::out);
        if (!fs) {
            fs.open(session.tmpPath, std::ios::binary | std::ios::out);
        }
        if (!fs) { res.status = 500; return; }
        fs.seekp(index * session.chunkSize, std::ios::beg);
        fs.write(body.data(), static_cast<std::streamsize>(body.size()));
        fs.flush();
        if (!fs) { res.status = 500; return; }
    }
    res.status = 200;
    res.set_content("{\"stored\":true}", "application/json");
}

void HttpFileServer::handleGetUpload(const httplib::Request& req, httplib::Response& res)
{
    int userId = 0;
    std::string deviceId;
    if (!authenticate(req, userId, deviceId)) { res.status = 401; return; }
    const std::string uploadId = req.matches[1];
    Database::UploadSession session;
    if (!m_server.db().getUploadSession(uploadId, session)) { res.status = 404; return; }
    if (session.uploaderId != userId || session.deviceId != deviceId) { res.status = 403; return; }

    std::vector<int> received;
    m_server.db().listUploadChunkIndices(uploadId, received);
    std::string receivedJson = "[";
    for (std::size_t i = 0; i < received.size(); ++i) {
        if (i) receivedJson += ",";
        receivedJson += std::to_string(received[i]);
    }
    receivedJson += "]";
    res.set_content("{\"upload_id\":\"" + session.uploadId + "\",\"state\":\"" + session.state +
        "\",\"chunk_size\":" + std::to_string(session.chunkSize) +
        ",\"chunk_count\":" + std::to_string(session.chunkCount) +
        ",\"expires_at\":" + std::to_string(session.expiresAt) +
        ",\"file_id\":\"" + session.fileId + "\"" +
        ",\"received\":" + receivedJson + "}", "application/json");
}

void HttpFileServer::handleFinalizeUpload(const httplib::Request& req, httplib::Response& res)
{
    int userId = 0;
    std::string deviceId;
    if (!authenticate(req, userId, deviceId)) { res.status = 401; return; }
    const std::string uploadId = req.matches[1];
    Database::UploadSession session;
    if (!m_server.db().getUploadSession(uploadId, session)) { res.status = 404; return; }
    if (session.uploaderId != userId || session.deviceId != deviceId) { res.status = 403; return; }
    if (session.state == "cancelled") { res.status = 410; return; }

    // 幂等：已 finalized 直接返回原 file_id
    if (session.state == "finalized") {
        res.set_content("{\"file_id\":\"" + session.fileId + "\",\"sha256\":\"" + session.sha256 +
            "\",\"size\":" + std::to_string(session.fileSize) +
            ",\"content_type\":\"" + session.contentType + "\"}", "application/json");
        return;
    }

    // 分片齐全校验
    std::vector<int> received;
    m_server.db().listUploadChunkIndices(uploadId, received);
    if (static_cast<int>(received.size()) != session.chunkCount) {
        res.status = 409; // 缺片
        res.set_content("{\"error\":\"missing_chunks\",\"received\":" +
            std::to_string(received.size()) + ",\"chunk_count\":" +
            std::to_string(session.chunkCount) + "}", "application/json");
        return;
    }

    // 整文件重算 SHA-256 + 大小校验（不信任分片摘要的拼接结论）
    std::ifstream ifs(session.tmpPath, std::ios::binary | std::ios::ate);
    if (!ifs || static_cast<std::int64_t>(ifs.tellg()) != session.fileSize) {
        res.status = 409;
        res.set_content("{\"error\":\"size_mismatch\"}", "application/json");
        return;
    }
    ifs.seekg(0, std::ios::beg);
    im::Sha256 hasher;
    std::vector<char> buf(64 * 1024);
    while (ifs) {
        ifs.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto got = ifs.gcount();
        if (got > 0) hasher.update(buf.data(), static_cast<std::size_t>(got));
    }
    static const char* hex = "0123456789abcdef";
    std::string actual;
    actual.reserve(64);
    for (unsigned char b : hasher.final()) {
        actual.push_back(hex[(b >> 4) & 0xF]);
        actual.push_back(hex[b & 0xF]);
    }
    if (actual != session.sha256) {
        res.status = 409;
        res.set_content("{\"error\":\"sha256_mismatch\"}", "application/json");
        return;
    }

    // file_id 由 upload_id 派生：同会话多次 finalize 得到同一 file_id（天然幂等）
    const std::string fileId = im::sha256Hex(uploadId + "|finalize");

    // 原子转正：图片内容寻址去重，普通文件按 file_id 命名
    std::error_code ec;
    std::string finalPath;
    if (session.isImage) {
        std::string headBytes(16, '\0');
        { std::ifstream h(session.tmpPath, std::ios::binary); h.read(headBytes.data(), 16); }
        const std::string ext = im::imageExtForBytes(headBytes);
        finalPath = m_server.uploadDir() + "/img/" + session.sha256 + ext;
        std::filesystem::create_directories(m_server.uploadDir() + "/img", ec);
        if (std::filesystem::exists(finalPath, ec)) {
            std::filesystem::remove(session.tmpPath, ec);
        } else {
            std::filesystem::rename(session.tmpPath, finalPath, ec);
            if (ec) {
                std::filesystem::copy_file(session.tmpPath, finalPath,
                    std::filesystem::copy_options::overwrite_existing, ec);
                std::filesystem::remove(session.tmpPath, ec);
            }
        }
    } else {
        finalPath = m_server.uploadDir() + "/file/" + fileId + "_" + session.fileName;
        std::filesystem::create_directories(m_server.uploadDir() + "/file", ec);
        std::filesystem::rename(session.tmpPath, finalPath, ec);
        if (ec) {
            std::filesystem::copy_file(session.tmpPath, finalPath,
                std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(session.tmpPath, ec);
        }
    }

    // 入库：session→finalized（同事务回填 file_id）+ 内容寻址媒体索引（秒传立即可命中）
    std::string existing;
    if (!m_server.db().finalizeUploadSession(uploadId, fileId, &existing)) {
        res.status = 409; // 会话状态被并发改变（取消等）
        return;
    }
    if (!existing.empty()) {
        res.set_content("{\"file_id\":\"" + existing + "\",\"sha256\":\"" + session.sha256 +
            "\",\"size\":" + std::to_string(session.fileSize) +
            ",\"content_type\":\"" + session.contentType + "\"}", "application/json");
        return;
    }
    m_server.db().registerMediaObject(session.sha256, session.fileSize, finalPath,
                                      session.contentType);
    {
        std::lock_guard<std::mutex> lg(m_uploadMtx);
        m_uploads[fileId] = UploadRecord{userId, session.receiverId, finalPath, session.sha256,
            session.fileSize, session.contentType, nowSec()};
    }
    log("[http] 分片上传完成 uid=", userId, " -> ", session.receiverId,
        " file=", session.fileName, " size=", session.fileSize, " file_id=", fileId);
    res.set_content("{\"file_id\":\"" + fileId + "\",\"sha256\":\"" + session.sha256 +
        "\",\"size\":" + std::to_string(session.fileSize) +
        ",\"content_type\":\"" + session.contentType + "\"}", "application/json");
}

void HttpFileServer::handleCancelUpload(const httplib::Request& req, httplib::Response& res)
{
    int userId = 0;
    std::string deviceId;
    if (!authenticate(req, userId, deviceId)) { res.status = 401; return; }
    const std::string uploadId = req.matches[1];
    Database::UploadSession session;
    if (!m_server.db().getUploadSession(uploadId, session)) { res.status = 404; return; }
    if (session.uploaderId != userId || session.deviceId != deviceId) { res.status = 403; return; }
    if (session.state == "finalized") {
        res.status = 409; // 已完成会话不可取消
        res.set_content("{\"error\":\"already_finalized\",\"file_id\":\"" + session.fileId + "\"}",
            "application/json");
        return;
    }
    if (!m_server.db().cancelUploadSession(uploadId)) { res.status = 409; return; }
    std::error_code ec;
    std::filesystem::remove(session.tmpPath, ec);
    log("[http] 上传会话取消 upload_id=", uploadId, " uid=", userId);
    res.set_content("{\"cancelled\":true}", "application/json");
}

void HttpFileServer::handlePreflight(const httplib::Request& req, httplib::Response& res)
{
    int userId = 0;
    std::string deviceId;
    if (!authenticate(req, userId, deviceId)) { res.status = 401; return; }
    int receiverId = 0;
    std::int64_t size = 0;
    try {
        receiverId = std::stoi(req.get_header_value("X-Receiver-Id"));
        size = std::stoll(req.get_header_value("X-File-Size"));
    } catch (...) { res.status = 400; return; }
    const std::string sha = req.get_header_value("X-File-Sha256");
    if (!m_server.db().isFriend(userId, receiverId)) { res.status = 403; return; }
    Database::MediaObject object;
    if (!m_server.db().findMediaObject(sha, size, object)) {
        res.set_content("{\"hit\":false}", "application/json");
        return;
    }

    constexpr std::int64_t kProofBytes = 64 * 1024;
    const std::int64_t length = std::min(kProofBytes, size);
    const std::string seed = im::sha256Hex(std::to_string(userId) + "|" + sha + "|" + std::to_string(nowSec()));
    const std::uint64_t n = std::stoull(seed.substr(0, 16), nullptr, 16);
    const std::int64_t offset = size > length ? static_cast<std::int64_t>(n % (size - length + 1)) : 0;
    const std::string challengeId = im::sha256Hex(seed + "|" + std::to_string(offset));
    {
        std::lock_guard<std::mutex> lg(m_uploadMtx);
        m_challenges[challengeId] = ProofChallenge{userId, receiverId, object.path, sha, size,
            object.contentType, offset, length, nowSec()};
    }
    res.set_content("{\"hit\":true,\"challenge_id\":\"" + challengeId +
        "\",\"offset\":" + std::to_string(offset) + ",\"length\":" + std::to_string(length) + "}",
        "application/json");
}

void HttpFileServer::handleProof(const httplib::Request& req, httplib::Response& res)
{
    int userId = 0;
    std::string deviceId;
    if (!authenticate(req, userId, deviceId)) { res.status = 401; return; }
    const std::string challengeId = req.matches[1];
    ProofChallenge challenge;
    {
        std::lock_guard<std::mutex> lg(m_uploadMtx);
        auto it = m_challenges.find(challengeId);
        if (it == m_challenges.end()) { res.status = 404; return; }
        challenge = it->second;
        m_challenges.erase(it); // 单次使用，防重放
    }
    if (challenge.uploaderId != userId || nowSec() - challenge.createdAt > 300 ||
        static_cast<std::int64_t>(req.body.size()) != challenge.length) {
        res.status = 403; return;
    }
    std::ifstream input(challenge.path, std::ios::binary);
    input.seekg(challenge.offset);
    std::string expected(static_cast<std::size_t>(challenge.length), '\0');
    input.read(expected.data(), static_cast<std::streamsize>(challenge.length));
    if (!input || expected != req.body) { res.status = 409; return; }

    static std::atomic<std::uint64_t> proofCounter{0};
    const std::string fileId = im::sha256Hex(challengeId + "|" + std::to_string(proofCounter.fetch_add(1)));
    {
        std::lock_guard<std::mutex> lg(m_uploadMtx);
        m_uploads[fileId] = UploadRecord{userId, challenge.receiverId, challenge.path,
            challenge.sha256, challenge.size, challenge.contentType, nowSec()};
    }
    log("[http] 秒传 PoP 通过 uid=", userId, " file_id=", fileId, " sha256=", challenge.sha256);
    res.set_content("{\"file_id\":\"" + fileId + "\",\"sha256\":\"" + challenge.sha256 +
        "\",\"size\":" + std::to_string(challenge.size) + ",\"content_type\":\"" +
        challenge.contentType + "\",\"instant\":true}", "application/json");
}

HttpFileServer::~HttpFileServer()
{
    stop();
}

bool HttpFileServer::start()
{
    if (!m_svr.is_valid()) {
        log("[http] TLS 证书/私钥加载失败 cert=", m_certPath, " key=", m_keyPath);
        return false;
    }
    m_running = true;
    m_listenThread = std::thread([this]() {
        log("[http] 文件服务监听 port=", m_port);
        if (!m_svr.listen("0.0.0.0", m_port)) {
            log("[http] 监听失败 port=", m_port);
        }
    });
    m_gcThread = std::thread(&HttpFileServer::gcLoop, this);
    return true;
}

void HttpFileServer::stop()
{
    if (!m_running.exchange(false)) return;
    m_svr.stop();
    if (m_listenThread.joinable()) m_listenThread.join();
    if (m_gcThread.joinable()) m_gcThread.join();
}

bool HttpFileServer::authenticate(const httplib::Request& req, int& outUserId, std::string& outDeviceId) const
{
    const std::string authHeader = req.get_header_value("Authorization");
    static const std::string kPrefix = "Bearer ";
    if (authHeader.size() <= kPrefix.size() || authHeader.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    const std::string token = authHeader.substr(kPrefix.size());

    outDeviceId = req.get_header_value("X-Device-Id");
    if (outDeviceId.empty() || outDeviceId.size() > 128) return false;

    std::string sessionId;
    std::int64_t expiresAt = 0;
    // sessionId 传空串：只校验 token+device_id，跟 Dispatcher::onTokenLoginRq 用的是同一套函数/口径
    return m_server.tokenService().validateAccess(token, "", outDeviceId, outUserId, sessionId, expiresAt);
}

void HttpFileServer::handleUpload(const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader)
{
    int userId = 0;
    std::string deviceId;
    if (!authenticate(req, userId, deviceId)) {
        res.status = 401;
        return;
    }

    const std::string rawName = req.get_header_value("X-File-Name");
    const std::string safeName = std::filesystem::path(rawName).filename().string();
    if (safeName.empty() || safeName.size() > 255) {
        res.status = 400;
        return;
    }

    int receiverId = 0;
    try {
        receiverId = std::stoi(req.get_header_value("X-Receiver-Id"));
    } catch (...) {
        res.status = 400;
        return;
    }
    if (!m_server.db().isFriend(userId, receiverId)) {
        res.status = 403;
        return;
    }

    const std::string contentLengthHeader = req.get_header_value("Content-Length");
    std::int64_t declaredSize = -1;
    try {
        declaredSize = contentLengthHeader.empty() ? -1 : std::stoll(contentLengthHeader);
    } catch (...) {
        declaredSize = -1;
    }
    if (declaredSize < 0 || declaredSize > im::proto::FILE_MAX_SIZE) {
        res.status = 413;
        return;
    }

    // file_id 服务端生成，不接受客户端指定，避免 ID 猜测/冲突
    static std::atomic<std::uint64_t> counter{0};
    const std::string fileId = im::sha256Hex(
        std::to_string(userId) + "|" + std::to_string(receiverId) + "|" + safeName + "|" +
        std::to_string(nowSec()) + "|" + std::to_string(counter.fetch_add(1)));

    const std::string contentType = req.get_header_value("Content-Type");
    const bool isImage = contentType.rfind("image/", 0) == 0;
    const std::string tmpDir = m_server.uploadDir() + (isImage ? "/img/tmp" : "/file/tmp");
    std::error_code dirEc;
    std::filesystem::create_directories(tmpDir, dirEc);
    const std::string tmpPath = tmpDir + "/" + fileId + ".part";

    std::ofstream ofs(tmpPath, std::ios::binary);
    if (!ofs) {
        res.status = 500;
        return;
    }

    im::Sha256 hasher;
    std::string headBytes; // 前 16 字节留作图片魔数嗅探，避免收完再读一次盘
    std::int64_t received = 0;
    const bool readOk = reader([&](const char* data, std::size_t len) -> bool {
        received += static_cast<std::int64_t>(len);
        if (received > im::proto::FILE_MAX_SIZE) return false; // 超限：中止接收
        if (headBytes.size() < 16) {
            headBytes.append(data, std::min(len, std::size_t(16) - headBytes.size()));
        }
        ofs.write(data, static_cast<std::streamsize>(len));
        hasher.update(data, len);
        return true;
    });
    ofs.close();

    std::error_code rmEc;
    if (!readOk || received != declaredSize) {
        std::filesystem::remove(tmpPath, rmEc);
        res.status = 400;
        return;
    }

    const std::vector<unsigned char> digest = hasher.final();
    static const char* hex = "0123456789abcdef";
    std::string sha;
    sha.reserve(64);
    for (unsigned char b : digest) {
        sha.push_back(hex[(b >> 4) & 0xF]);
        sha.push_back(hex[b & 0xF]);
    }

    std::string finalPath;
    if (isImage) {
        // 内容寻址去重：跟 Dispatcher::saveImage 用的是同一套命名规则
        const std::string ext = im::imageExtForBytes(headBytes);
        finalPath = m_server.uploadDir() + "/img/" + sha + ext;
        std::error_code existsEc;
        if (std::filesystem::exists(finalPath, existsEc)) {
            std::filesystem::remove(tmpPath, rmEc); // 已有相同内容，丢弃这次收到的临时文件
        } else {
            std::filesystem::rename(tmpPath, finalPath, rmEc);
            if (rmEc) {
                std::filesystem::copy_file(tmpPath, finalPath, std::filesystem::copy_options::overwrite_existing, rmEc);
                std::filesystem::remove(tmpPath, rmEc);
            }
        }
    } else {
        finalPath = m_server.uploadDir() + "/file/" + fileId + "_" + safeName;
        std::filesystem::rename(tmpPath, finalPath, rmEc);
        if (rmEc) {
            std::filesystem::copy_file(tmpPath, finalPath, std::filesystem::copy_options::overwrite_existing, rmEc);
            std::filesystem::remove(tmpPath, rmEc);
        }
    }

    {
        std::lock_guard<std::mutex> lg(m_uploadMtx);
        m_uploads[fileId] = UploadRecord{userId, receiverId, finalPath, sha, received, contentType, nowSec()};
    }

    log("[http] 上传完成 uid=", userId, " -> ", receiverId, " file=", safeName, " size=", received, " file_id=", fileId);

    res.status = 200;
    res.set_content(
        "{\"file_id\":\"" + fileId + "\",\"sha256\":\"" + sha + "\",\"size\":" + std::to_string(received) +
        ",\"content_type\":\"" + contentType + "\"}",
        "application/json");
}

void HttpFileServer::handleDownload(const httplib::Request& req, httplib::Response& res)
{
    int userId = 0;
    std::string deviceId;
    if (!authenticate(req, userId, deviceId)) {
        res.status = 401;
        return;
    }

    const std::string fileId = req.matches[1];
    if (!isSafeFileId(fileId)) {
        res.status = 400;
        return;
    }

    StoredMessage m;
    if (!m_server.db().getMessageByFileId(fileId, m) || m.mediaPath.empty()) {
        res.status = 404;
        return;
    }
    if (userId != m.senderId && userId != m.receiverId) {
        // file_id 本身不是访问凭证，必须是这条消息的参与者才能下载
        res.status = 403;
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(m.mediaPath, ec)) {
        res.status = 404;
        return;
    }

    const std::string contentType = guessContentType(m.mediaPath);
    res.set_header("Accept-Ranges", "bytes");
    res.set_header("ETag", "\"" + fileId + "\"");
    if (m.type == 2) {
        res.set_header("Content-Disposition", "attachment; filename=\"" + m.content + "\"");
    }
    // set_file_content 走 cpp-httplib 内置的静态文件发送路径，自动处理 Range/206，
    // 服务端不需要整文件读进内存
    res.set_file_content(m.mediaPath, contentType);
}

bool HttpFileServer::findUploadRecord(const std::string& fileId, UploadRecord& out)
{
    std::lock_guard<std::mutex> lg(m_uploadMtx);
    auto it = m_uploads.find(fileId);
    if (it == m_uploads.end()) return false;
    out = it->second;
    return true;
}

void HttpFileServer::eraseUploadRecord(const std::string& fileId)
{
    std::lock_guard<std::mutex> lg(m_uploadMtx);
    m_uploads.erase(fileId);
}

void HttpFileServer::gcLoop()
{
    // 孤儿上传清理：上传成功但从未被 ChatInfoRq 认领的记录（比如客户端上传后崩溃/取消发送），
    // 超过 24h 视为废弃，删除记录和落盘文件。
    constexpr std::int64_t kTtlSeconds = 24 * 3600;
    while (m_running.load()) {
        for (int i = 0; i < 600 && m_running.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!m_running.load()) break;

        const std::int64_t now = nowSec();
        std::vector<std::string> expiredPaths;
        {
            std::lock_guard<std::mutex> lg(m_uploadMtx);
            for (auto it = m_uploads.begin(); it != m_uploads.end();) {
                if (now - it->second.uploadedAt > kTtlSeconds) {
                    expiredPaths.push_back(it->second.mediaPath);
                    it = m_uploads.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = m_challenges.begin(); it != m_challenges.end();) {
                if (now - it->second.createdAt > 300) it = m_challenges.erase(it);
                else ++it;
            }
        }
        std::error_code ec;
        for (const auto& p : expiredPaths) {
            std::filesystem::remove(p, ec);
            log("[http] 清理孤儿上传 ", p);
        }

        // 过期分片会话：删临时文件 + DB 记录（取消/未完成都算）
        std::vector<Database::UploadSession> expiredSessions;
        if (m_server.db().listExpiredUploadSessions(now, expiredSessions)) {
            for (const auto& s : expiredSessions) {
                std::filesystem::remove(s.tmpPath, ec);
                if (m_server.db().deleteUploadSession(s.uploadId)) {
                    log("[http] 清理过期上传会话 ", s.uploadId, " tmp=", s.tmpPath);
                }
            }
        }
    }
}

} // namespace imsrv
