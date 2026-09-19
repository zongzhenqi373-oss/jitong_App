// 分片上传草稿状态机测试（P7 媒体断点续传 Native 侧）。
//
// 覆盖：登记幂等（保留进度）、会话写入、分片确认幂等、finalize 回填 file_id、
// 状态机保护（终态不可迁移/不可回退）、活跃草稿恢复扫描、owner 隔离。

#include "client_core/storage/NativeDatabase.h"
#include "client_core/storage/NativeRepository.h"
#include "client_core/dto/Dtos.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace im;
using namespace im::dto;
using namespace im::storage;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& name)
{
    if (cond) {
        std::printf("[PASSED] %s\n", name.c_str());
    } else {
        std::printf("[FAILED] %s\n", name.c_str());
        ++g_failures;
    }
}

UploadDraftDto makeDraft(const std::string& msgId, MediaVariant variant = MediaVariant::Origin)
{
    UploadDraftDto d;
    d.msgId = msgId;
    d.variant = variant;
    d.conversationId = 100;
    d.peerId = 200;
    d.localPath = "/tmp/photo.jpg";
    d.fileName = "photo.jpg";
    d.fileSize = 2 * 1024 * 1024 + 100;
    d.sha256 = std::string(64, 'a');
    d.contentType = "image/jpeg";
    return d;
}

} // namespace

int main()
{
    std::system("rm -rf /tmp/test_upload_draft && mkdir -p /tmp/test_upload_draft");
    NativeDatabase db;
    std::string err;
    check(db.open("/tmp/test_upload_draft", 1, std::vector<unsigned char>(32, 9), &err), "open");
    NativeRepository repo(&db);

    // [1] 登记 + 读取
    check(repo.upsertUploadDraft(1, makeDraft("m-up"), nullptr, &err), "登记草稿 " + err);
    UploadDraftDto d;
    check(repo.getUploadDraft(1, "m-up", MediaVariant::Origin, &d, &err), "读取草稿 " + err);
    check(d.state == UploadDraftState::Pending && d.uploadId.empty() && d.chunksDone.empty(),
          "初始 Pending 无会话无进度");

    // [2] 重复登记幂等：不重置进度
    check(repo.setUploadDraftSession(1, "m-up", MediaVariant::Origin, "up-1", 1048576, 3, &err),
          "写入会话 " + err);
    check(repo.markUploadChunkDone(1, "m-up", MediaVariant::Origin, 1, &err), "确认分片1 " + err);
    check(repo.markUploadChunkDone(1, "m-up", MediaVariant::Origin, 1, &err), "重复确认分片1幂等 " + err);
    check(repo.upsertUploadDraft(1, makeDraft("m-up"), nullptr, &err), "重复登记 " + err);
    check(repo.getUploadDraft(1, "m-up", MediaVariant::Origin, &d, &err), "重读 " + err);
    check(d.uploadId == "up-1" && d.chunksDone.size() == 1 && d.chunksDone[0] == 1 &&
          d.state == UploadDraftState::Uploading,
          "重复登记保留会话与分片进度");

    // [3] 分片乱序 + 越界拒绝
    check(repo.markUploadChunkDone(1, "m-up", MediaVariant::Origin, 0, &err), "乱序分片0 " + err);
    check(!repo.markUploadChunkDone(1, "m-up", MediaVariant::Origin, 3, &err), "越界分片拒绝");
    check(repo.getUploadDraft(1, "m-up", MediaVariant::Origin, &d, &err), "再读 " + err);
    check(d.chunksDone.size() == 2 && d.chunksDone[0] == 0 && d.chunksDone[1] == 1,
          "分片记录升序去重");

    // [4] finalize 回填 file_id + 幂等
    check(repo.setUploadDraftFileId(1, "m-up", MediaVariant::Origin, "file-1", &err),
          "回填 file_id " + err);
    check(repo.setUploadDraftFileId(1, "m-up", MediaVariant::Origin, "file-1", &err),
          "同 file_id 重试幂等 " + err);
    check(!repo.setUploadDraftFileId(1, "m-up", MediaVariant::Origin, "file-2", &err),
          "不同 file_id 拒绝（防覆盖）");
    check(repo.getUploadDraft(1, "m-up", MediaVariant::Origin, &d, &err), "读 finalized " + err);
    check(d.fileId == "file-1" && d.state == UploadDraftState::Finalized, "Finalized 状态");

    // [5] 消息已入 Outbox → Sent 终态；终态保护
    check(repo.setUploadDraftState(1, "m-up", MediaVariant::Origin, UploadDraftState::Sent, 0, &err),
          "Sent " + err);
    check(!repo.setUploadDraftState(1, "m-up", MediaVariant::Origin, UploadDraftState::Failed, 1, &err),
          "终态后不可迁回 Failed");
    check(!repo.markUploadChunkDone(1, "m-up", MediaVariant::Origin, 2, &err),
          "终态后分片确认拒绝");

    // [6] 取消是终态，不自动恢复；网络异常 Failed 可恢复
    check(repo.upsertUploadDraft(1, makeDraft("m-cancel"), nullptr, &err), "登记取消草稿 " + err);
    check(repo.setUploadDraftState(1, "m-cancel", MediaVariant::Origin,
                                   UploadDraftState::Cancelled, 0, &err), "取消 " + err);
    check(!repo.setUploadDraftSession(1, "m-cancel", MediaVariant::Origin, "up-2", 1048576, 3, &err),
          "取消后不可重建会话");
    check(repo.upsertUploadDraft(1, makeDraft("m-fail"), nullptr, &err), "登记失败草稿 " + err);
    check(repo.setUploadDraftState(1, "m-fail", MediaVariant::Origin,
                                   UploadDraftState::Failed, 1001, &err), "网络失败 " + err);
    check(repo.setUploadDraftSession(1, "m-fail", MediaVariant::Origin, "up-3", 1048576, 3, &err),
          "Failed 可恢复（重建会话）" + err);

    // [7] 图片多变体共享 msg_id 独立状态
    check(repo.upsertUploadDraft(1, makeDraft("m-img"), nullptr, &err), "登记原图草稿 " + err);
    check(repo.upsertUploadDraft(1, makeDraft("m-img", MediaVariant::LargeThumbnail), nullptr, &err),
          "登记大图草稿 " + err);
    check(repo.upsertUploadDraft(1, makeDraft("m-img", MediaVariant::SmallThumbnail), nullptr, &err),
          "登记小图草稿 " + err);
    check(repo.setUploadDraftState(1, "m-img", MediaVariant::SmallThumbnail,
                                   UploadDraftState::Cancelled, 0, &err), "取消小图 " + err);
    UploadDraftDto large;
    check(repo.getUploadDraft(1, "m-img", MediaVariant::LargeThumbnail, &large, &err), "读大图 " + err);
    check(large.state == UploadDraftState::Pending, "小图取消不影响大图");

    // [8] 活跃恢复扫描：只含 Pending/Uploading/Finalized/Failed
    std::vector<UploadDraftDto> active;
    check(repo.loadActiveUploadDrafts(1, &active, &err), "扫描活跃草稿 " + err);
    // m-up(Sent) 与 m-cancel(Cancelled) 不应出现；m-fail、m-img(Origin/Large) 应在
    check(active.size() == 3, "活跃草稿=3（m-fail + m-img 原图/大图）");

    // [9] owner 隔离
    check(!repo.getUploadDraft(2, "m-fail", MediaVariant::Origin, &d, &err), "owner2 读不到 owner1 草稿");

    db.close();
    if (g_failures == 0) {
        std::printf("test_upload_draft: ALL PASSED\n");
        return 0;
    }
    std::printf("test_upload_draft: %d FAILED\n", g_failures);
    return 1;
}
