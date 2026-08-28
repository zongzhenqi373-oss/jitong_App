#pragma once
// HTTP(S) 文件服务：文件/图片上传下载走独立的 HTTPS 端点（Range 续传、流式收发），
// 跟主 TCP+TLS 长连接（Session/Dispatcher）完全分开跑，只共享 Database/TokenService。
// 详见 SECURITY_REVIEW.md 第 42 条。

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "httplib.h"

namespace imsrv {

class Server;

class HttpFileServer {
public:
    HttpFileServer(Server& server, std::uint16_t port, std::string certPath, std::string keyPath);
    ~HttpFileServer();

    HttpFileServer(const HttpFileServer&) = delete;
    HttpFileServer& operator=(const HttpFileServer&) = delete;

    bool start();
    void stop();

    // 一次上传的归属记录：上传时从 Bearer token 解出的身份原子性写入，
    // 不接受任何客户端自报字段——onChatRq 后续按 file_id 核对 uploaderId 时据此为准。
    struct UploadRecord {
        int uploaderId = 0;
        int receiverId = 0;
        std::string mediaPath;
        std::string sha256;
        std::int64_t size = 0;
        std::string contentType;
        std::int64_t uploadedAt = 0;
    };

    // 先查询、消息落库成功后再删除，避免 DB 失败时上传凭证被提前消费而无法重试。
    bool findUploadRecord(const std::string& fileId, UploadRecord& out);
    void eraseUploadRecord(const std::string& fileId);

private:
    bool authenticate(const httplib::Request& req, int& outUserId, std::string& outDeviceId) const;
    void handleUpload(const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader);
    void handleDownload(const httplib::Request& req, httplib::Response& res);
    void gcLoop();

    Server& m_server;
    std::uint16_t m_port;
    std::string m_certPath;
    std::string m_keyPath;
    httplib::SSLServer m_svr;
    std::thread m_listenThread;
    std::thread m_gcThread;
    std::atomic<bool> m_running{false};
    std::mutex m_uploadMtx;
    std::unordered_map<std::string, UploadRecord> m_uploads;
};

} // namespace imsrv
