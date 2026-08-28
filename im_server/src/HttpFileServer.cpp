#include "HttpFileServer.h"

#include "Server.h"
#include "Database.h"
#include "Log.h"
#include "MediaUtil.h"
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
    return "application/octet-stream";
}

std::int64_t nowSec() { return static_cast<std::int64_t>(std::time(nullptr)); }

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
    m_svr.Get(R"(/api/v1/download/([a-zA-Z0-9._-]+))",
        [this](const httplib::Request& req, httplib::Response& res) { handleDownload(req, res); });
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
        }
        std::error_code ec;
        for (const auto& p : expiredPaths) {
            std::filesystem::remove(p, ec);
            log("[http] 清理孤儿上传 ", p);
        }
    }
}

} // namespace imsrv
