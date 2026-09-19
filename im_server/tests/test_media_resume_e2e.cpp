// 媒体断点续传真实链路 E2E（P7）：真服务端 + 真 TLS + 真登录 +
// 生产 ClientCoreUploadTransport + MediaService + 真 Native 加密库。
//
// 覆盖：
//   1. 完整分片上传 → 消息卡片落库（固定 msg_id）→ Range 下载校验内容
//   2. 服务端重启后会话仍在（DB 持久化），客户端恢复只补传缺失分片
//   3. 秒传命中（同一文件再传）→ PoP 证明 → 秒得 file_id
//
// 断言用 assert()；CMake 已对测试目标加 -UNDEBUG。

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "core/Server.h"
#include "media/HttpFileServer.h"
#include "client_core/ClientCore.h"
#include "client_core/media/MediaService.h"
#include "client_core/media/ClientCoreUploadTransport.h"
#include "client_core/storage/NativeDatabase.h"
#include "client_core/storage/NativeRepository.h"

#include "sha256.h"

#include <openssl/evp.h>

namespace {

constexpr std::uint16_t TEST_PORT = 24694;
const char* TEST_SERVERNAME = "im.example.com";
const char* TEST_DB = "/tmp/im_media_resume_e2e.db";
const char* TEST_UPLOADS = "/tmp/im_media_resume_e2e_uploads";
const char* TEST_NATIVE = "/tmp/im_media_resume_e2e_native";

std::vector<unsigned char> ed25519PublicFromPemFile(const char* path)
{
    FILE* fp = std::fopen(path, "r");
    if (!fp) return {};
    EVP_PKEY* pkey = PEM_read_PrivateKey(fp, nullptr, nullptr, nullptr);
    std::fclose(fp);
    if (!pkey) return {};
    std::vector<unsigned char> pub(32, 0);
    std::size_t len = 32;
    const bool ok = EVP_PKEY_get_raw_public_key(pkey, pub.data(), &len) > 0 && len == 32;
    EVP_PKEY_free(pkey);
    return ok ? pub : std::vector<unsigned char>{};
}

im::ClientConfig makeTestConfig()
{
    im::ClientConfig cfg;
    cfg.tlsServerName = TEST_SERVERNAME;
    cfg.caFile = IM_SERVER_TEST_CERT;
    const auto pub = ed25519PublicFromPemFile(IM_SERVER_TEST_APP_IDENTITY_KEY);
    assert(pub.size() == 32);
    cfg.identityKeys[1] = pub;
    return cfg;
}

struct RecordingEvents : im::IClientEvents {
    std::mutex mtx;
    std::condition_variable cv;
    int loginResult = -1;
    int loginUserId = 0;
    void onRegisterResult(int) override {}
    void onLoginResult(int result, int userId) override
    {
        std::lock_guard<std::mutex> lk(mtx);
        loginResult = result;
        loginUserId = userId;
        cv.notify_all();
    }
    void onSelfInfo(const im::UserInfo&) override {}
    void onFriendInfo(const im::FriendInfo&) override {}
    void onChatMessage(int, const std::string&) override {}
    void onImageMessage(int, const std::string&, int, int, const std::string&) override {}
    void onChatSendResult(int, int) override {}
    void onAddFriendRequest(int, const std::string&) override {}
    void onAddFriendResult(int, const std::string&) override {}
    void onFriendOffline(int) override {}
    void onKickedOffline(int) override {}
    void onConnectionClosed() override {}
    bool waitFor(const std::function<bool()>& pred, int timeoutMs = 30000)
    {
        std::unique_lock<std::mutex> lk(mtx);
        return cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), pred);
    }
};

std::string sha256OfFile(const std::string& path)
{
    std::ifstream ifs(path, std::ios::binary);
    im::Sha256 h;
    std::vector<char> buf(65536);
    while (ifs) {
        ifs.read(buf.data(), buf.size());
        if (ifs.gcount() > 0) h.update(buf.data(), ifs.gcount());
    }
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned char b : h.final()) { out += hex[b >> 4]; out += hex[b & 15]; }
    return out;
}

void writeTestFile(const std::string& path, std::size_t size)
{
    std::ofstream ofs(path, std::ios::binary);
    std::uint64_t rng = 0x123456789abcdef0ull;
    std::string block(65536, '\0');
    for (std::size_t off = 0; off < size; off += block.size()) {
        for (auto& c : block) {
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            c = static_cast<char>(rng & 0xff);
        }
        ofs.write(block.data(), std::min(block.size(), size - off));
    }
}

std::string readFile(const std::string& path)
{
    std::ifstream ifs(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
}

im::dto::UploadDraftDto makeDraft(const std::string& msgId, int64_t convId, int peerId,
                                  const std::string& path, int64_t size)
{
    im::dto::UploadDraftDto d;
    d.msgId = msgId;
    d.variant = im::dto::MediaVariant::Origin;
    d.conversationId = convId;
    d.peerId = peerId;
    d.localPath = path;
    d.fileName = "clip.bin";
    d.fileSize = size;
    d.sha256 = sha256OfFile(path);
    d.contentType = "application/octet-stream";
    return d;
}

} // namespace

int main()
{
    std::remove(TEST_DB);
    std::remove((std::string(TEST_DB) + "-wal").c_str());
    std::remove((std::string(TEST_DB) + "-shm").c_str());
    std::filesystem::remove_all(TEST_UPLOADS);
    std::filesystem::remove_all(TEST_NATIVE);
    std::filesystem::create_directories(TEST_NATIVE);

    const auto cfg = makeTestConfig();

    imsrv::Server server(TEST_PORT, 2, 2, TEST_DB, TEST_UPLOADS, IM_SERVER_TEST_CERT,
                         IM_SERVER_TEST_KEY, static_cast<std::uint16_t>(TEST_PORT + 1),
                         IM_SERVER_TEST_APP_IDENTITY_KEY, 1);
    assert(server.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // 登录（种子用户 张三 id=1 → 李四 id=2）
    im::ClientCore core(cfg);
    RecordingEvents events;
    core.setEventSink(&events);
    core.connectToServer("127.0.0.1", TEST_PORT);
    core.sendLogin("13800000001", "123456");
    assert(events.waitFor([&] { return events.loginResult == im::proto::LOGIN_SUCCESS; })); // =0
    const int myId = events.loginUserId;
    assert(myId == 1);

    // Native 加密库 + 仓储 + 生产传输 + 编排
    im::storage::NativeDatabase nativeDb;
    std::string err;
    assert(nativeDb.open(TEST_NATIVE, myId, std::vector<unsigned char>(32, 7), &err));
    im::storage::NativeRepository repo(&nativeDb);
    im::media::ClientCoreUploadTransport transport(core);
    im::media::MediaService media(myId, repo, transport);

    const std::string filePath = std::string(TEST_NATIVE) + "/clip.bin";
    const std::size_t fileSize = 2 * 1048576 + 12345; // 3 片
    writeTestFile(filePath, fileSize);

    // ============ 1. 完整分片上传 + Range 下载校验 ============
    std::int64_t firstMs = 0, resumeMs = 0, instantMs = 0;
    std::int64_t firstBytes = 0, resumeBytes = 0, instantBytes = 0;
    {
        const auto t0 = std::chrono::steady_clock::now();
        const auto b0 = transport.bytesSent();
        assert(media.enqueue({makeDraft("e2e-1", 12, 2, filePath, fileSize)}));
        assert(media.pumpMessage("e2e-1", &err));
        firstMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        firstBytes = transport.bytesSent() - b0;
        im::dto::UploadDraftDto d;
        assert(repo.getUploadDraft(myId, "e2e-1", im::dto::MediaVariant::Origin, &d));
        assert(d.state == im::dto::UploadDraftState::Sent && !d.fileId.empty());
        assert(firstBytes == (std::int64_t)fileSize); // 首传 = 全量字节

        // 消息卡片落库（固定 msg_id，已入 Outbox）
        im::dto::MessageDto msg;
        assert(repo.findMessage(myId, "e2e-1", &msg));
        assert(msg.fileId == d.fileId && msg.type == 2 && msg.fromMe);

        // 通过真实网络发消息卡片：服务端绑定 file_id→消息（下载授权的事实源）
        core.sendFileMessage(2, d.fileId, "clip.bin", d.fileSize, d.contentType, d.sha256,
                             false, 0, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // Range 下载校验内容（断点续传下载侧）
        const std::string dest = std::string(TEST_NATIVE) + "/clip_dl.bin";
        assert(core.downloadMedia(d.fileId, dest));
        assert(readFile(dest) == readFile(filePath));
    }

    // ============ 2. 服务端重启后会话持久化，客户端恢复补传 ============
    // 用不同内容的文件，避免命中 e2e-1 已登记的秒传索引
    const std::string filePath2 = std::string(TEST_NATIVE) + "/clip2.bin";
    {
        std::ofstream ofs(filePath2, std::ios::binary);
        std::uint64_t rng = 0xfeedface12345678ull;
        for (std::size_t i = 0; i < fileSize; ++i) {
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            char c = static_cast<char>(rng & 0xff);
            ofs.write(&c, 1);
        }
    }
    {
        // 先手动建会话并传第 0 片（模拟上次启动的进度）
        assert(media.enqueue({makeDraft("e2e-2", 12, 2, filePath2, fileSize)}));
        im::media::UploadSessionInfo created;
        im::media::CreateSessionRequest req;
        req.receiverId = 2; req.fileName = "clip2.bin"; req.fileSize = fileSize;
        req.sha256 = sha256OfFile(filePath2); req.contentType = "application/octet-stream";
        auto r = transport.createSession(req, created);
        assert(r.ok && !created.instant);
        assert(repo.setUploadDraftSession(myId, "e2e-2", im::dto::MediaVariant::Origin,
                                          created.uploadId, created.chunkSize,
                                          created.chunkCount, &err));
        // 传第 0 片（真实 HTTP）
        std::ifstream ifs(filePath2, std::ios::binary);
        std::string chunk0(created.chunkSize, '\0');
        ifs.read(chunk0.data(), chunk0.size());
        r = transport.uploadChunk(created.uploadId, 0, im::sha256Hex(chunk0), chunk0, {});
        assert(r.ok);
        assert(repo.markUploadChunkDone(myId, "e2e-2", im::dto::MediaVariant::Origin, 0, &err));

        // 服务端重启（会话在 DB，不在内存）
        server.stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        imsrv::Server server2(TEST_PORT, 2, 2, TEST_DB, TEST_UPLOADS, IM_SERVER_TEST_CERT,
                              IM_SERVER_TEST_KEY, static_cast<std::uint16_t>(TEST_PORT + 1),
                              IM_SERVER_TEST_APP_IDENTITY_KEY, 1);
        assert(server2.start());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // 客户端恢复：以服务端为准，只补传缺失分片
        const auto t0 = std::chrono::steady_clock::now();
        const auto b0 = transport.bytesSent();
        assert(media.pumpMessage("e2e-2", &err));
        resumeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        resumeBytes = transport.bytesSent() - b0;
        im::dto::UploadDraftDto d;
        assert(repo.getUploadDraft(myId, "e2e-2", im::dto::MediaVariant::Origin, &d));
        assert(d.state == im::dto::UploadDraftState::Sent && !d.fileId.empty());
        // 已传 1 片：续传只补 2 片
        assert(resumeBytes == (std::int64_t)fileSize - (std::int64_t)created.chunkSize);

        // 服务端会话显示已完成
        im::media::UploadSessionInfo q;
        r = transport.querySession(created.uploadId, q);
        assert(r.ok && q.state == "finalized");

        // ============ 3. 秒传命中（同文件换 msgId 再传） ============
        const auto t1 = std::chrono::steady_clock::now();
        const auto b1 = transport.bytesSent();
        assert(media.enqueue({makeDraft("e2e-3", 12, 2, filePath, fileSize)}));
        assert(media.pumpMessage("e2e-3", &err));
        instantMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t1).count();
        instantBytes = transport.bytesSent() - b1;
        assert(repo.getUploadDraft(myId, "e2e-3", im::dto::MediaVariant::Origin, &d));
        assert(d.state == im::dto::UploadDraftState::Sent && !d.fileId.empty());
        // 秒传会话不应产生新的分片会话（instant 路径无 upload_id）
        assert(d.uploadId.empty());
        // 秒传只传 64KiB 证明片段
        assert(instantBytes == 65536);

        server2.stop();
    }

    std::printf("PERF 首传: %lld ms, %lld bytes (全量)\n", (long long)firstMs, (long long)firstBytes);
    std::printf("PERF 续传(1/3 已传): %lld ms, %lld bytes (只补缺失)\n",
                (long long)resumeMs, (long long)resumeBytes);
    std::printf("PERF 秒传: %lld ms, %lld bytes (64KiB 证明)\n",
                (long long)instantMs, (long long)instantBytes);

    core.disconnect();
    nativeDb.close();
    std::remove(TEST_DB);
    std::remove((std::string(TEST_DB) + "-wal").c_str());
    std::remove((std::string(TEST_DB) + "-shm").c_str());
    std::filesystem::remove_all(TEST_UPLOADS);
    std::filesystem::remove_all(TEST_NATIVE);
    std::printf("test_media_resume_e2e: ALL PASSED\n");
    return 0;
}
