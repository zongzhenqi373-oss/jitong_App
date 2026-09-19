// MediaService 分片上传编排测试（P7 媒体断点续传 Native 侧）。
//
// FakeUploadTransport 模拟服务端会话状态机，支持故障注入：
// 断网（chunkBudget）、finalize 响应丢失（dropFinalizeResponse，服务端实际已完成）、
// 会话丢失（loseSessions，模拟服务端 GC）。
//
// 覆盖验收清单 Native 侧部分：
//   正常分片上传、秒传命中、传到一半断网→恢复只传缺失、强杀后从服务端状态恢复、
//   finalize 完成但客户端未收到响应、文件完成但消息未发（重启补发）、
//   消息已存在不重复发送、取消后不自动恢复、服务端会话丢失重建。

#include "client_core/media/MediaService.h"
#include "client_core/storage/NativeDatabase.h"
#include "client_core/storage/NativeRepository.h"

#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace im;
using namespace im::dto;
using namespace im::media;
using namespace im::storage;

namespace {

int g_failures = 0;
void check(bool cond, const std::string& name)
{
    if (cond) std::printf("[PASSED] %s\n", name.c_str());
    else { std::printf("[FAILED] %s\n", name.c_str()); ++g_failures; }
}

long long queryInt(NativeDatabase& db, const std::string& sql)
{
    long long v = -1;
    db.withRead([&](sqlite3* d) {
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(d, sql.c_str(), -1, &s, nullptr) == SQLITE_OK && s) {
            if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
        }
        if (s) sqlite3_finalize(s);
    });
    return v;
}

// ---------------- Fake 服务端 ----------------

class FakeUploadTransport : public IUploadTransport {
public:
    struct Session {
        CreateSessionRequest meta;
        std::int64_t chunkSize = 1048576;
        int chunkCount = 0;
        std::set<int> received;
        std::string state = "open";
        std::string fileId;
    };

    std::map<std::string, Session> sessions;
    std::map<std::string, std::string> contentByFileId; // fileId -> sha256
    // 断网注入：允许 chunkBudget 个分片成功上传，之后的分片上传返回可恢复网络错误。
    // -1 = 不注入。只作用于分片上传（会话创建/查询是恢复的前提，不参与注入）。
    int chunkBudget = -1;
    bool dropFinalizeResponse = false; // finalize 实际成功但客户端收不到响应
    bool instantMode = false;       // 秒传命中
    int64_t bytesTransferred = 0;   // 实际传输字节数（验收指标）
    int uploadChunkCalls = 0;
    int nextId = 0;

    UploadResult fail()
    {
        UploadResult r; r.status = -1; r.retryable = true; r.error = "network down";
        return r;
    }

    UploadResult createSession(const CreateSessionRequest& req, UploadSessionInfo& out) override
    {
        if (instantMode) {
            out.instant = true;
            out.challengeId = "chal-1";
            out.proofOffset = 0;
            out.proofLength = std::min<std::int64_t>(65536, req.fileSize);
            UploadResult r; r.ok = true;
            return r;
        }
        out.instant = false;
        out.uploadId = "up-" + std::to_string(++nextId);
        out.chunkSize = 1048576;
        out.chunkCount = static_cast<int>((req.fileSize + out.chunkSize - 1) / out.chunkSize);
        out.expiresAt = 9999999999;
        Session s;
        s.meta = req; s.chunkSize = out.chunkSize; s.chunkCount = out.chunkCount;
        sessions[out.uploadId] = std::move(s);
        UploadResult r; r.ok = true;
        return r;
    }

    UploadResult querySession(const std::string& id, UploadSessionInfo& out) override
    {
        auto it = sessions.find(id);
        if (it == sessions.end()) {
            UploadResult r; r.status = 404; r.retryable = false; r.error = "not found";
            return r;
        }
        out.uploadId = id;
        out.chunkSize = it->second.chunkSize;
        out.chunkCount = it->second.chunkCount;
        out.state = it->second.state;
        out.fileId = it->second.fileId;
        out.received.assign(it->second.received.begin(), it->second.received.end());
        UploadResult r; r.ok = true;
        return r;
    }

    UploadResult uploadChunk(const std::string& id, int index, const std::string& sha,
                             const std::string& bytes, const CancelFlag&) override
    {
        if (chunkBudget == 0) return fail();
        auto it = sessions.find(id);
        if (it == sessions.end()) { UploadResult r; r.status = 404; r.retryable = false; return r; }
        if (chunkBudget > 0) --chunkBudget;
        it->second.received.insert(index);
        bytesTransferred += static_cast<int64_t>(bytes.size());
        ++uploadChunkCalls;
        (void)sha;
        UploadResult r; r.ok = true;
        return r;
    }

    UploadResult prove(const std::string&, const std::string& bytes,
                       std::string& fileIdOut) override
    {
        bytesTransferred += static_cast<int64_t>(bytes.size());
        fileIdOut = "file-instant";
        UploadResult r; r.ok = true;
        return r;
    }

    UploadResult finalize(const std::string& id, std::string& fileIdOut) override
    {
        auto it = sessions.find(id);
        if (it == sessions.end()) { UploadResult r; r.status = 404; r.retryable = false; return r; }
        if ((int)it->second.received.size() != it->second.chunkCount) {
            UploadResult r; r.status = 409; r.retryable = false; r.error = "missing chunks";
            return r;
        }
        it->second.state = "finalized";
        it->second.fileId = "file-" + id;
        fileIdOut = it->second.fileId;
        if (dropFinalizeResponse) {
            UploadResult r; r.status = -1; r.retryable = true; r.error = "response lost";
            return r; // 服务端已完成，客户端未收到
        }
        UploadResult r; r.ok = true;
        return r;
    }

    UploadResult cancelSession(const std::string& id) override
    {
        auto it = sessions.find(id);
        if (it != sessions.end()) it->second.state = "cancelled";
        UploadResult r; r.ok = true;
        return r;
    }

    void loseSessions() { sessions.clear(); }
};

UploadDraftDto makeDraft(const std::string& msgId, MediaVariant variant,
                         const std::string& path, int64_t size)
{
    UploadDraftDto d;
    d.msgId = msgId;
    d.variant = variant;
    d.conversationId = 100;
    d.peerId = 200;
    d.localPath = path;
    d.fileName = "video.bin";
    d.fileSize = size;
    d.sha256 = std::string(64, 'b'); // fake 不校验内容摘要
    d.contentType = "application/octet-stream";
    return d;
}

void writeFile(const std::string& path, int64_t size)
{
    std::ofstream ofs(path, std::ios::binary);
    std::string block(4096, 'x');
    for (int64_t i = 0; i < size; i += 4096)
        ofs.write(block.data(), std::min<int64_t>(4096, size - i));
}

} // namespace

int main()
{
    std::system("rm -rf /tmp/test_media_service && mkdir -p /tmp/test_media_service");
    const std::string filePath = "/tmp/test_media_service/video.bin";
    const int64_t fileSize = 2 * 1048576 + 100; // 3 片
    writeFile(filePath, fileSize);

    FakeUploadTransport fake;

    auto openRepo = [&](NativeDatabase& db, NativeRepository*& repo) {
        std::string err;
        check(db.open("/tmp/test_media_service", 1, std::vector<unsigned char>(32, 5), &err),
              "open " + err);
        repo = new NativeRepository(&db);
    };

    // ============ 1. 正常分片上传全流程 ============
    {
        NativeDatabase db; NativeRepository* repo = nullptr; openRepo(db, repo);
        MediaService svc(1, *repo, fake);
        auto imageDraft = makeDraft("m1", MediaVariant::Origin, filePath, fileSize);
        imageDraft.contentType = "image/avif";
        imageDraft.imageWidth = 320;
        imageDraft.imageHeight = 240;
        check(svc.enqueue({imageDraft}), "登记 m1 图片尺寸");
        check(svc.pumpMessage("m1"), "m1 pump 完成");
        UploadDraftDto d;
        check(repo->getUploadDraft(1, "m1", MediaVariant::Origin, &d), "读 m1");
        check(d.state == UploadDraftState::Sent && d.fileId == "file-up-1", "m1 Sent + fileId");
        check(d.imageWidth == 320 && d.imageHeight == 240, "草稿尺寸持久化");
        im::dto::MessageDto imageMessage;
        check(repo->findMessage(1, "m1", &imageMessage), "图片卡片入库");
        check(imageMessage.imgW == 320 && imageMessage.imgH == 240,
              "图片卡片宽高与草稿一致");
        check(queryInt(db, "SELECT count(*) FROM messages WHERE msg_id='m1'") == 1,
              "消息已落 messages（固定 msg_id）");
        check(queryInt(db, "SELECT count(*) FROM outbox WHERE msg_id='m1'") == 1,
              "消息已入 outbox（发送由 Outbox 状态机负责）");
        check(fake.uploadChunkCalls == 3, "传输了 3 片");
        check(fake.bytesTransferred == fileSize, "实际传输字节数 = 文件大小（无重复传输）");
        delete repo; db.close();
    }

    // ============ 2. 秒传命中 ============
    {
        NativeDatabase db; NativeRepository* repo = nullptr; openRepo(db, repo);
        fake.instantMode = true;
        const auto before = fake.bytesTransferred;
        MediaService svc(1, *repo, fake);
        check(svc.enqueue({makeDraft("m2", MediaVariant::Origin, filePath, fileSize)}), "登记 m2");
        check(svc.pumpMessage("m2"), "m2 pump 完成");
        UploadDraftDto d;
        check(repo->getUploadDraft(1, "m2", MediaVariant::Origin, &d), "读 m2");
        check(d.state == UploadDraftState::Sent && d.fileId == "file-instant", "秒传 Sent");
        check(fake.uploadChunkCalls == 3, "秒传不再传分片（仍为上一条的 3 次）");
        check(fake.bytesTransferred - before == 65536, "秒传只传 64KB 证明片段");
        fake.instantMode = false;
        delete repo; db.close();
    }

    // ============ 3. 传到一半断网 → 恢复只传缺失 ============
    {
        NativeDatabase db; NativeRepository* repo = nullptr; openRepo(db, repo);
        MediaService svc(1, *repo, fake);
        check(svc.enqueue({makeDraft("m3", MediaVariant::Origin, filePath, fileSize)}), "登记 m3");
        fake.chunkBudget = 1; // 第 1 片成功后断网
        check(!svc.pumpMessage("m3"), "断网后 pump 失败");
        UploadDraftDto d;
        check(repo->getUploadDraft(1, "m3", MediaVariant::Origin, &d), "读 m3");
        check(d.state == UploadDraftState::Failed && d.chunksDone.size() == 1,
              "m3 Failed 且只完成 1 片");
        fake.chunkBudget = -1; // 网络恢复
        const auto bytesBefore = fake.bytesTransferred;
        const auto callsBefore = fake.uploadChunkCalls;
        // 恢复：服务端状态为准，只传缺失 2 片
        check(svc.pumpMessage("m3"), "恢复 pump 完成");
        check(repo->getUploadDraft(1, "m3", MediaVariant::Origin, &d), "再读 m3");
        check(d.state == UploadDraftState::Sent, "m3 恢复后 Sent");
        check(fake.uploadChunkCalls - callsBefore == 2, "恢复只补传 2 片");
        check(fake.bytesTransferred - bytesBefore == fileSize - 1048576,
              "恢复传输字节 = 缺失部分");
        delete repo; db.close();
    }

    // ============ 4. 强杀恢复（销毁重建，DB 持久草稿 + 服务端状态） ============
    {
        NativeDatabase db; NativeRepository* repo = nullptr; openRepo(db, repo);
        MediaService svc(1, *repo, fake);
        check(svc.enqueue({makeDraft("m4", MediaVariant::Origin, filePath, fileSize)}), "登记 m4");
        fake.chunkBudget = 1; // 第 1 片成功后断网
        check(!svc.pumpMessage("m4"), "m4 中断");
        UploadDraftDto d;
        check(repo->getUploadDraft(1, "m4", MediaVariant::Origin, &d), "读 m4");
        check(d.state == UploadDraftState::Failed, "m4 Failed");
        const std::string sessionId = d.uploadId;
        fake.chunkBudget = -1; // 网络恢复
        delete repo; db.close(); // 强杀

        NativeDatabase db2; NativeRepository* repo2 = nullptr; openRepo(db2, repo2);
        MediaService svc2(1, *repo2, fake);
        check(svc2.resumeAll() == 1, "强杀后 resumeAll 恢复 m4");
        check(repo2->getUploadDraft(1, "m4", MediaVariant::Origin, &d), "恢复后读 m4");
        check(d.state == UploadDraftState::Sent && d.uploadId == sessionId,
              "m4 复用原会话恢复完成（不重建）");
        delete repo2; db2.close();
    }

    // ============ 5. finalize 完成但客户端未收到响应 ============
    {
        NativeDatabase db; NativeRepository* repo = nullptr; openRepo(db, repo);
        MediaService svc(1, *repo, fake);
        check(svc.enqueue({makeDraft("m5", MediaVariant::Origin, filePath, fileSize)}), "登记 m5");
        fake.dropFinalizeResponse = true;
        check(!svc.pumpMessage("m5"), "finalize 响应丢失 → 本次 pump 失败");
        fake.dropFinalizeResponse = false;
        UploadDraftDto d;
        check(repo->getUploadDraft(1, "m5", MediaVariant::Origin, &d), "读 m5");
        check(d.state == UploadDraftState::Failed && d.fileId.empty(),
              "m5 Failed 且本地无 fileId");
        // 恢复：querySession 发现服务端已 finalized → 直接采用 fileId，不重传
        const auto callsBefore = fake.uploadChunkCalls;
        check(svc.pumpMessage("m5"), "m5 恢复完成");
        check(repo->getUploadDraft(1, "m5", MediaVariant::Origin, &d), "再读 m5");
        check(d.state == UploadDraftState::Sent && !d.fileId.empty(), "m5 采用服务端 fileId");
        check(fake.uploadChunkCalls == callsBefore, "恢复不重传任何分片");
        delete repo; db.close();
    }

    // ============ 6. 文件完成但消息未发（Finalized 崩溃 → 重启补发，不重复） ============
    {
        NativeDatabase db; NativeRepository* repo = nullptr; openRepo(db, repo);
        MediaService svc(1, *repo, fake);
        check(svc.enqueue({makeDraft("m6", MediaVariant::Origin, filePath, fileSize)}), "登记 m6");
        check(svc.pumpMessage("m6"), "m6 首次完成");
        // 模拟重试：消息已存在 → 不重复写 messages
        check(svc.pumpMessage("m6"), "m6 重试幂等");
        check(queryInt(db, "SELECT count(*) FROM messages WHERE msg_id='m6'") == 1,
              "重试不重复发消息");
        delete repo; db.close();
    }

    // ============ 7. 取消后不自动恢复 ============
    {
        NativeDatabase db; NativeRepository* repo = nullptr; openRepo(db, repo);
        MediaService svc(1, *repo, fake);
        check(svc.enqueue({makeDraft("m7", MediaVariant::Origin, filePath, fileSize)}), "登记 m7");
        fake.chunkBudget = 1;
        check(!svc.pumpMessage("m7"), "m7 中断");
        fake.chunkBudget = -1;
        check(svc.cancel("m7"), "取消 m7");
        UploadDraftDto d;
        check(repo->getUploadDraft(1, "m7", MediaVariant::Origin, &d), "读 m7");
        check(d.state == UploadDraftState::Cancelled, "m7 Cancelled 终态");
        check(svc.resumeAll() == 0, "resumeAll 不触碰已取消草稿");
        check(repo->getUploadDraft(1, "m7", MediaVariant::Origin, &d), "再读 m7");
        check(d.state == UploadDraftState::Cancelled, "m7 保持 Cancelled");
        delete repo; db.close();
    }

    // ============ 8. 服务端会话丢失（GC）→ 重建会话重传 ============
    {
        NativeDatabase db; NativeRepository* repo = nullptr; openRepo(db, repo);
        MediaService svc(1, *repo, fake);
        check(svc.enqueue({makeDraft("m8", MediaVariant::Origin, filePath, fileSize)}), "登记 m8");
        fake.chunkBudget = 1;
        check(!svc.pumpMessage("m8"), "m8 中断（1 片已传）");
        fake.chunkBudget = -1; // 网络恢复
        fake.loseSessions(); // 服务端 GC 清掉会话
        check(svc.pumpMessage("m8"), "m8 重建会话完成");
        UploadDraftDto d;
        check(repo->getUploadDraft(1, "m8", MediaVariant::Origin, &d), "读 m8");
        check(d.state == UploadDraftState::Sent, "m8 Sent");
        delete repo; db.close();
    }

    // ============ 9. 图片多变体共享 msg_id ============
    {
        NativeDatabase db; NativeRepository* repo = nullptr; openRepo(db, repo);
        MediaService svc(1, *repo, fake);
        check(svc.enqueue({makeDraft("m9", MediaVariant::Origin, filePath, fileSize),
                           makeDraft("m9", MediaVariant::LargeThumbnail, filePath, 512000),
                           makeDraft("m9", MediaVariant::SmallThumbnail, filePath, 64000)}),
              "登记 m9 三变体");
        check(svc.pumpMessage("m9"), "m9 全部变体完成");
        check(queryInt(db, "SELECT count(*) FROM messages WHERE msg_id='m9'") == 1,
              "一条消息卡片");
        check(queryInt(db, "SELECT length(thumbnail_file_id)>0 AND length(large_thumbnail_file_id)>0 "
                           "FROM messages WHERE msg_id='m9'") == 1,
              "大小缩略图 fileId 均已写入消息");
        delete repo; db.close();
    }

    if (g_failures == 0) {
        std::printf("test_media_service: ALL PASSED\n");
        return 0;
    }
    std::printf("test_media_service: %d FAILED\n", g_failures);
    return 1;
}
