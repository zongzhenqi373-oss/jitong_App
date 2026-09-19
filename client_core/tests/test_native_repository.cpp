// P7-G2：NativeRepository 业务级事务测试。
//
// 验收（v2 §3 P7-G2）：
//   - 消息与 FTS/会话/未读/水位在**同一事务**内提交；
//   - 重复 msg_id 幂等：不增行、不改会话摘要（F06 不变式）；
//   - 发送草稿 → Ack 链路：outbox 生命周期正确；
//   - 已读原子更新 unread 与水位；
//   - 媒体变体与引用同事务写入。

#include <sqlite3.h>

#include <cstdio>
#include <iostream>
#include <string>

#include "client_core/dto/Dtos.h"
#include "client_core/storage/NativeDatabase.h"
#include "client_core/storage/NativeRepository.h"
#include "client_core/storage/MessageMediaColumns.h"

using namespace im::storage;
using im::dto::MessageDto;
using im::dto::MediaTaskDto;
using im::dto::TaskState;
using im::dto::MediaVariant;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& name)
{
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << std::endl;
    if (!cond) ++g_failures;
}

const char* kDir = "/tmp/test_native_repo";
const std::int64_t kOwner = 1;

std::vector<unsigned char> testKey() { return std::vector<unsigned char>(32, 0x55); }

MessageDto makeIncoming(const std::string& msgId, std::int64_t seq, const std::string& content)
{
    MessageDto m;
    m.ownerId = kOwner;
    m.msgId = msgId;
    m.conversationId = 555;
    m.peerId = 2;
    m.seq = seq;
    m.ts = 1700000000 + seq;
    m.localOrder = seq;
    m.fromMe = false;
    m.type = 0;
    m.content = content;
    m.status = 3; // Delivered
    m.pinyin = "nihao";
    m.initials = "nh";
    return m;
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

std::string queryText(NativeDatabase& db, const std::string& sql)
{
    std::string v;
    db.withRead([&](sqlite3* d) {
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(d, sql.c_str(), -1, &s, nullptr) == SQLITE_OK && s) {
            if (sqlite3_step(s) == SQLITE_ROW) {
                const unsigned char* t = sqlite3_column_text(s, 0);
                if (t) v = reinterpret_cast<const char*>(t);
            }
        }
        if (s) sqlite3_finalize(s);
    });
    return v;
}
} // namespace

int main()
{
    std::cout << "=== test_native_repository ===" << std::endl;

    ::system(("rm -rf " + std::string(kDir)).c_str());
    ::system(("mkdir -p " + std::string(kDir)).c_str());

    NativeDatabase db;
    std::string err;
    check(db.open(kDir, kOwner, testKey(), &err), "打开库 " + err);
    check(db.status() == DbStatus::Ready, "状态 Ready");

    NativeRepository repo(&db);

    // [1] 接收消息：消息 + FTS + 会话 + 未读 同事务
    {
        bool ins = false;
        check(repo.commitIncomingMessage(makeIncoming("m1", 10, "hello"), &ins, &err),
              "接收 m1 " + err);
        check(ins, "首次接收 inserted=true");

        const long long n = queryInt(db, "SELECT count(*) FROM messages WHERE owner_id=1");
        check(n == 1, "messages 有 1 行");

        const long long fts = queryInt(db, "SELECT count(*) FROM message_fts_identity "
                                           "WHERE owner_id=1");
        check(fts == 1, "FTS identity 同步写入 1 行（同事务）");

        const long long unread = queryInt(db, "SELECT unread FROM conversations "
                                              "WHERE conversation_id=555");
        check(unread == 1, "未读 +1（非本人新消息）");
    }

    // [2] F06 不变式：重复 msg_id 幂等，不增行、不改摘要
    {
        bool ins2 = true;
        check(repo.commitIncomingMessage(makeIncoming("m1", 10, "hello"), &ins2, &err),
              "重复接收 m1 " + err);
        check(!ins2, "重复接收 inserted=false");

        const long long n = queryInt(db, "SELECT count(*) FROM messages WHERE owner_id=1");
        check(n == 1, "重复接收不增行");

        const long long unread = queryInt(db, "SELECT unread FROM conversations "
                                              "WHERE conversation_id=555");
        check(unread == 1, "重复接收未读不重复累加（仍为 1）");

        const long long fts = queryInt(db, "SELECT count(*) FROM message_fts_identity "
                                           "WHERE owner_id=1");
        check(fts == 1, "重复接收 FTS 不翻倍");
    }

    // [3] 不同 msg_id 正常累加
    {
        bool ins3 = false;
        check(repo.commitIncomingMessage(makeIncoming("m2", 11, "world"), &ins3, &err),
              "接收 m2 " + err);
        check(ins3, "m2 inserted=true");
        const long long n = queryInt(db, "SELECT count(*) FROM messages WHERE owner_id=1");
        check(n == 2, "messages 有 2 行");

        const long long unread = queryInt(db, "SELECT unread FROM conversations "
                                              "WHERE conversation_id=555");
        check(unread == 2, "未读累加为 2");
    }

    // [4] 发送草稿 → outbox
    {
        MessageDto draft = makeIncoming("out1", 0, "pending text");
        draft.fromMe = true;
        draft.status = 0; // Sending
        draft.localOrder = 900;
        check(repo.commitOutgoingDraft(draft, &err), "提交发送草稿 " + err);

        const long long outbox = queryInt(db, "SELECT count(*) FROM outbox WHERE owner_id=1");
        check(outbox == 1, "outbox 有 1 条待发送");

        const long long n = queryInt(db, "SELECT count(*) FROM messages WHERE owner_id=1");
        check(n == 3, "messages 有 3 行（含草稿）");
    }

    // 会话页只读快照：由 Repository 定义排序和账号隔离，UI 不拼 SQL。
    {
        std::vector<im::dto::ConversationDto> rows;
        check(repo.loadConversations(kOwner,&rows,&err),"读取会话快照 "+err);
        check(!rows.empty()&&rows.front().ownerId==kOwner&&rows.front().conversationId==555&&
                  rows.front().peerId==2&&rows.front().lastMsg=="pending text",
              "会话快照包含最新摘要并保持账号隔离");
        std::vector<im::dto::ConversationDto> other;
        check(repo.loadConversations(999,&other,&err)&&other.empty(),"其他账号会话快照为空");
    }

    // [5] Ack：更新 server_time/seq/status 并删除 outbox
    {
        bool updated = false;
        check(repo.commitAck(kOwner, "out1", 1700000999, 12, 3, &updated, &err),
              "提交 Ack " + err);
        check(updated, "Ack 更新了消息");

        const long long outbox = queryInt(db, "SELECT count(*) FROM outbox WHERE owner_id=1");
        check(outbox == 0, "Ack 后 outbox 清空");

        const long long seq = queryInt(db, "SELECT conversation_seq FROM messages "
                                           "WHERE owner_id=1 AND msg_id='out1'");
        check(seq == 12, "Ack 后 conversation_seq=12");

        const long long st = queryInt(db, "SELECT status FROM messages "
                                          "WHERE owner_id=1 AND msg_id='out1'");
        check(st == 3, "Ack 后 status=3（已送达）");
    }

    // [6] 已读：unread 归零 + 水位写入
    {
        check(repo.markConversationRead(kOwner, 555, 11, &err), "标记已读 " + err);
        const long long unread = queryInt(db, "SELECT unread FROM conversations "
                                              "WHERE conversation_id=555");
        check(unread == 0, "已读后 unread=0");

        const long long wm = queryInt(db, "SELECT count(*) FROM sync_watermarks "
                                          "WHERE owner_id=1 AND domain='read'");
        check(wm == 1, "已读水位写入 1 条");
    }

    // [7] 媒体变体与引用：同事务写入
    {
        MediaTaskDto t;
        t.taskId = "t1";
        t.msgId = "m1";
        t.variant = MediaVariant::SmallThumbnail;
        t.state = TaskState::Success;
        t.bytesTotal = 4096;
        t.fileHash = "hash-small-1";
        t.filePath = "/tmp/thumb.jpg";

        check(repo.upsertMediaAndMessage({t}, makeIncoming("m1", 10, "hello"), &err),
              "写入媒体变体与引用 " + err);

        const long long v = queryInt(db, "SELECT count(*) FROM media_variants");
        check(v == 1, "media_variants 有 1 行");

        const long long r = queryInt(db, "SELECT count(*) FROM media_refs WHERE owner_id=1");
        check(r == 1, "media_refs 有 1 行（同事务）");
    }

    // [8] 语义：更旧的历史消息**不得**覆盖更新的会话摘要（用户指出的覆盖缺陷）
    {
        const long long beforeTs = queryInt(db, "SELECT last_message_time FROM conversations "
                                                 "WHERE conversation_id=555");
        MessageDto older = makeIncoming("m-old", 5, "older");
        older.ts = 1700000001; // 比现有最后消息更早
        bool ins7 = false;
        check(repo.commitIncomingMessage(older, &ins7, &err), "插入更旧消息 " + err);
        const long long afterTs = queryInt(db, "SELECT last_message_time FROM conversations "
                                                "WHERE conversation_id=555");
        check(afterTs == beforeTs, "更旧消息未把 last_message_time 回退");
        check(afterTs >= 1700000011, "会话摘要保留更新的本地发送展示时间");
    }

    // [9] 语义：maxseen 水位单调递增（先收 100、再收 90 不得回退到 90）
    {
        MessageDto a = makeIncoming("m-100", 100, "newest");
        a.ts = 1700001000;
        bool i1 = false;
        check(repo.commitIncomingMessage(a, &i1, &err), "收 seq=100 " + err);
        const long long w1 = queryInt(db, "SELECT synced_seq FROM sync_watermarks "
                                           "WHERE owner_id=1 AND domain='maxseen'");
        check(w1 == 100, "水位推进到 100");

        MessageDto b = makeIncoming("m-90", 90, "older");
        b.ts = 1700000900;
        bool i2 = false;
        check(repo.commitIncomingMessage(b, &i2, &err), "再收 seq=90 " + err);
        const long long w2 = queryInt(db, "SELECT synced_seq FROM sync_watermarks "
                                           "WHERE owner_id=1 AND domain='maxseen'");
        check(w2 == 100, "收到更旧的 90 后水位不回退（仍为 100）");
    }

    // [10] 语义：历史漫游导入不计未读
    {
        IncomingContext roamCtx;
        roamCtx.isRoaming = true;
        MessageDto r = makeIncoming("m-roam", 200, "roamed");
        r.conversationId = 777;
        r.peerId = 700; // 独立 peer：避免与既有会话在 (owner,peer,seq) 唯一索引上冲突
        bool i3 = false;
        check(repo.commitIncomingMessage(r, &i3, &err, roamCtx), "漫游消息入库 " + err);
        const long long unread = queryInt(db, "SELECT unread FROM conversations "
                                              "WHERE conversation_id=777");
        check(unread == 0, "漫游导入不计未读（unread=0）");
    }

    // [11] 语义：当前前台会话收到的消息不计未读
    {
        IncomingContext activeCtx;
        activeCtx.activeConversationId = 888;
        MessageDto m = makeIncoming("m-active", 300, "active");
        m.conversationId = 888;
        m.peerId = 800;
        bool i4 = false;
        check(repo.commitIncomingMessage(m, &i4, &err, activeCtx), "前台会话消息入库 " + err);
        const long long unread = queryInt(db, "SELECT unread FROM conversations "
                                              "WHERE conversation_id=888");
        check(unread == 0, "当前前台会话不计未读（unread=0）");
    }

    // [12] 语义：重复消息补齐既有记录缺失的 seq / status（而非直接返回）
    {
        MessageDto draft = makeIncoming("m-backsync", 0, "pending");
        draft.status = 0;
        bool i5 = false;
        check(repo.commitIncomingMessage(draft, &i5, &err), "先入库未确认消息(seq=0) " + err);
        check(queryInt(db, "SELECT conversation_seq FROM messages WHERE msg_id='m-backsync'") == 0,
              "初始 seq=0");

        MessageDto confirmed = makeIncoming("m-backsync", 50, "pending");
        confirmed.status = 3;
        bool i6 = true;
        check(repo.commitIncomingMessage(confirmed, &i6, &err), "同 msg_id 再收(seq=50) " + err);
        check(!i6, "重复消息 inserted=false");
        check(queryInt(db, "SELECT conversation_seq FROM messages WHERE msg_id='m-backsync'") == 50,
              "重复消息补齐了 seq（0 → 50）");
        check(queryInt(db, "SELECT status FROM messages WHERE msg_id='m-backsync'") == 3,
              "重复消息补齐了 status（0 → 3）");
    }

    // [13] 语义：已读只清除 readSeq 之前的未读（不该无条件清零）
    {
        for (int s = 10; s <= 12; ++s) {
            MessageDto m = makeIncoming("m-r" + std::to_string(s), s, "r");
            m.conversationId = 999;
            m.peerId = 900;
            bool ix = false;
            check(repo.commitIncomingMessage(m, &ix, &err), "会话999 收 seq=" + std::to_string(s) + " " + err);
        }
        check(queryInt(db, "SELECT unread FROM conversations WHERE conversation_id=999") == 3,
              "3 条未读");

        // 已读到 11：10、11 已读；12 仍应未读
        check(repo.markConversationRead(kOwner, 999, 11, &err), "标记已读到 11 " + err);
        check(queryInt(db, "SELECT unread FROM conversations WHERE conversation_id=999") == 1,
              "只清除 readSeq 之前的未读（剩 1 条 seq=12）");
        check(queryInt(db, "SELECT read_seq FROM conversations WHERE conversation_id=999") == 11,
              "read_seq 记录为 11");
    }

    // [14] 语义：旧已读请求不得让 read_seq / unread 回退（有效水位 = MAX(旧, 新)）
    {
        for (int s = 101; s <= 105; ++s) {
            MessageDto m = makeIncoming("m-rw-" + std::to_string(s), s, "rw");
            m.conversationId = 1000;
            m.peerId = 1000;
            bool ix = false;
            check(repo.commitIncomingMessage(m, &ix, &err),
                  "会话1000 收 seq=" + std::to_string(s) + " " + err);
        }
        check(queryInt(db, "SELECT unread FROM conversations WHERE conversation_id=1000") == 5,
              "5 条未读");

        check(repo.markConversationRead(kOwner, 1000, 105, &err), "已读到 105 " + err);
        check(queryInt(db, "SELECT read_seq FROM conversations WHERE conversation_id=1000") == 105,
              "read_seq=105");
        check(queryInt(db, "SELECT unread FROM conversations WHERE conversation_id=1000") == 0,
              "已读到 105 后 unread=0");

        // 迟到的旧已读请求（102 < 105）：有效水位保持 105，不回到 102，未读不回升
        check(repo.markConversationRead(kOwner, 1000, 102, &err), "旧已读请求 102 " + err);
        check(queryInt(db, "SELECT read_seq FROM conversations WHERE conversation_id=1000") == 105,
              "旧已读请求不回退 read_seq（仍 105）");
        check(queryInt(db, "SELECT unread FROM conversations WHERE conversation_id=1000") == 0,
              "旧已读请求不让未读回升（仍 0）");
    }

    // [15] Ack 状态机：迟到旧回执不覆盖终态（只在 Sending→终态 前进）
    {
        MessageDto draft = makeIncoming("out-status", 0, "status machine");
        draft.fromMe = true;
        draft.status = 0;
        draft.localOrder = 950;
        check(repo.commitOutgoingDraft(draft, &err), "草稿 out-status " + err);

        bool updated = false;
        check(repo.commitAck(kOwner, "out-status", 1700000999, 20, 3, &updated, &err),
              "Ack status=3（离线转存）" + err);
        check(queryInt(db, "SELECT status FROM messages WHERE msg_id='out-status'") == 3,
              "status=3");
        check(queryInt(db, "SELECT count(*) FROM outbox WHERE msg_id='out-status'") == 0,
              "终态后 outbox 删除");

        // 迟到旧回执 status=1（已送达）：不覆盖终态 3，也不回退 seq
        bool updated2 = false;
        check(repo.commitAck(kOwner, "out-status", 1700000999, 20, 1, &updated2, &err),
              "迟到旧回执 status=1 " + err);
        check(queryInt(db, "SELECT status FROM messages WHERE msg_id='out-status'") == 3,
              "迟到旧回执不回退 status（仍 3）");
        check(queryInt(db, "SELECT conversation_seq FROM messages WHERE msg_id='out-status'") == 20,
              "迟到旧回执不回退 seq（仍 20）");
    }

    // [16] Ack 状态机：可重试失败（status=0）保留 outbox 并递增 retry_count
    {
        MessageDto draft = makeIncoming("out-fail", 0, "retry");
        draft.fromMe = true;
        draft.status = 0;
        draft.localOrder = 960;
        check(repo.commitOutgoingDraft(draft, &err), "草稿 out-fail " + err);

        bool updated = false;
        check(repo.commitAck(kOwner, "out-fail", 0, 0, 0, &updated, &err),
              "可重试失败 status=0 " + err);
        check(queryInt(db, "SELECT status FROM messages WHERE msg_id='out-fail'") == 0,
              "失败后 status 仍 Sending(0)");
        check(queryInt(db, "SELECT count(*) FROM outbox WHERE msg_id='out-fail'") == 1,
              "失败保留 outbox（可重试）");
        check(repo.commitAck(kOwner, "out-fail", 0, 0, 0, &updated, &err), "重复失败回执");
        check(queryInt(db, "SELECT retry_count FROM outbox WHERE msg_id='out-fail'") == 0,
              "回执不计发送次数（含重复回执）");
    }

    // [17] 媒体事务：同事务写消息本体 + 真实 real_mime/width/height
    {
        MediaTaskDto t;
        t.taskId = "t-media";
        t.msgId = "m-media";
        t.variant = MediaVariant::Origin;
        t.state = TaskState::Success;
        t.bytesTotal = 20480;
        t.fileHash = "hash-origin-2";
        t.filePath = "/tmp/photo.avif";
        t.realMime = "image/avif"; // 真实编码格式，禁止按后缀猜
        t.width = 1920;
        t.height = 1080;

        MessageDto m = makeIncoming("m-media", 400, "media msg");
        m.conversationId = 1111;
        m.peerId = 1100;

        check(repo.upsertMediaAndMessage({t}, m, &err), "媒体+消息同事务 " + err);
        check(queryInt(db, "SELECT count(*) FROM messages WHERE msg_id='m-media'") == 1,
              "媒体事务写入消息本体（此前只写媒体不写消息）");
        check(queryInt(db, "SELECT count(*) FROM media_variants WHERE media_id='hash-origin-2'") == 1,
              "媒体变体写入");
        check(queryText(db, "SELECT real_mime FROM media_variants WHERE media_id='hash-origin-2'") ==
                  "image/avif",
              "real_mime 为真实格式 image/avif");
        check(queryInt(db, "SELECT width FROM media_variants WHERE media_id='hash-origin-2'") == 1920,
              "width=1920");
        check(queryInt(db, "SELECT height FROM media_variants WHERE media_id='hash-origin-2'") == 1080,
              "height=1080");
    }

    // [18] 重复草稿幂等：不重复写 FTS / outbox / 会话摘要
    {
        MessageDto draft = makeIncoming("out-dup", 0, "dup draft");
        draft.fromMe = true;
        draft.status = 0;
        draft.localOrder = 970;
        check(repo.commitOutgoingDraft(draft, &err), "首次草稿 out-dup " + err);
        check(queryInt(db, "SELECT count(*) FROM messages WHERE msg_id='out-dup'") == 1,
              "messages 1 行");
        check(queryInt(db, "SELECT count(*) FROM outbox WHERE msg_id='out-dup'") == 1,
              "outbox 1 条");
        check(queryInt(db, "SELECT count(*) FROM message_fts_identity "
                           "WHERE owner_id=1 AND msg_id='out-dup'") == 1,
              "FTS identity 1 条");

        check(repo.commitOutgoingDraft(draft, &err), "重复草稿 out-dup " + err);
        check(queryInt(db, "SELECT count(*) FROM messages WHERE msg_id='out-dup'") == 1,
              "重复草稿不增行");
        check(queryInt(db, "SELECT count(*) FROM outbox WHERE msg_id='out-dup'") == 1,
              "重复草稿 outbox 不翻倍");
        check(queryInt(db, "SELECT count(*) FROM message_fts_identity "
                           "WHERE owner_id=1 AND msg_id='out-dup'") == 1,
              "重复草稿 FTS identity 不翻倍");
    }

    {
        MessageDto m=makeIncoming("media-roundtrip",901,"media all fields");
        m.mediaPath="origin"; m.imgW=100; m.imgH=200; m.fileId="origin-id";
        m.fileName="photo.png"; m.fileSize=900; m.contentType="image/png"; m.sha256="origin-hash";
        m.thumbnailFileId="small-id"; m.thumbnailPath="small"; m.thumbnailSize=12;
        m.thumbnailSha256="small-hash"; m.thumbnailW=10; m.thumbnailH=20;
        m.largeThumbnailFileId="large-id"; m.largeThumbnailPath="large"; m.largeThumbnailSize=50;
        m.largeThumbnailSha256="large-hash"; m.largeThumbnailW=50; m.largeThumbnailH=100;
        m.localPath="local"; m.transferred=900;
        bool inserted=false;
        check(repo.commitIncomingMessage(m,&inserted,&err),"完整媒体写入 " + err);
        db.close();
        check(db.open(kDir,kOwner,testKey(),&err),"重开媒体库 " + err);
        MessageDto actual=makeIncoming("media-roundtrip",901,"media all fields");
        bool found=false;
        db.withRead([&](sqlite3* d){
            sqlite3_stmt* s=nullptr;
            const auto sql="SELECT "+mediaSelectColumns()+" FROM messages WHERE msg_id='media-roundtrip'";
            if(sqlite3_prepare_v2(d,sql.c_str(),-1,&s,nullptr)==SQLITE_OK && sqlite3_step(s)==SQLITE_ROW){
                found=true; readMediaColumns(s,0,actual);
            }
            sqlite3_finalize(s);
        });
        check(found && actual.fields()==m.fields(),"重启后媒体 DTO 逐字段一致（三档）");
        auto conflict=m; conflict.peerId=12345;
        inserted=true;
        check(!repo.commitIncomingMessage(conflict,&inserted,&err) && !inserted,
              "相同 msgId 不允许跨 peer 改身份");
        bool updated=true;
        check(!repo.commitAck(kOwner,m.msgId,0,902,3,&updated,&err) && !updated,
              "冲突 seq 回执被拒且 updated=false");
    }
    {
        MessageDto foreground=makeIncoming("foreground-regression",950,"visible");
        foreground.conversationId=7777; foreground.peerId=7777;
        IncomingContext ctx; ctx.activeConversationId=7777;
        check(repo.commitIncomingMessage(foreground,nullptr,&err,ctx),"前台收到消息");
        check(repo.markConversationRead(kOwner,7777,0,&err),"迟到已读重算");
        check(queryInt(db,"SELECT unread FROM conversations WHERE conversation_id=7777")==0,
              "前台已看消息不会被重算为未读");
        check(queryInt(db,"SELECT read_seq FROM conversations WHERE conversation_id=7777")==950,
              "前台阅读推进已读水位");
    }
    // [19] 历史消息 keyset 分页：同时间戳不漏不重，翻页期间插入新消息不漂移。
    {
        constexpr std::int64_t conv = 8888;
        for (int i = 1; i <= 5; ++i) {
            auto m = makeIncoming("page-" + std::to_string(i), i, "page");
            m.conversationId = conv; m.peerId = conv; m.ts = 1700000000; m.localOrder = i;
            check(repo.commitIncomingMessage(m, nullptr, &err), "分页夹具写入 " + std::to_string(i));
        }
        MessagePage first;
        check(repo.listConversation(kOwner, conv, nullptr, 2, &first, &err), "读取首页 " + err);
        check(first.hasMore && first.messages.size() == 2 && first.messages[0].msgId == "page-5" &&
                  first.messages[1].msgId == "page-4", "首页按完整排序键稳定降序");

        auto newer = makeIncoming("page-6", 6, "new while paging");
        newer.conversationId = conv; newer.peerId = conv; newer.ts = 1700000001; newer.localOrder = 6;
        check(repo.commitIncomingMessage(newer, nullptr, &err), "翻页期间插入新消息");
        MessagePage second;
        check(repo.listConversation(kOwner, conv, &first.next, 2, &second, &err), "读取第二页 " + err);
        check(second.hasMore && second.messages.size() == 2 && second.messages[0].msgId == "page-3" &&
                  second.messages[1].msgId == "page-2", "第二页不重复且不受新插入影响");
        MessagePage third;
        check(repo.listConversation(kOwner, conv, &second.next, 2, &third, &err), "读取末页 " + err);
        check(!third.hasMore && third.messages.size() == 1 && third.messages[0].msgId == "page-1",
              "末页 hasMore=false 且无漏读");
    }
    // [20] 下载任务持久化：同账号消息授权、可恢复失败、取消终态与 generation 防迟到。
    {
        auto media=makeIncoming("download-media", 1001, "");
        media.type=1;media.fileId="media-file-1";
        media.sha256=std::string(64,'a');media.fileSize=4096;
        check(repo.commitIncomingMessage(media,nullptr,&err),"媒体消息夹具入库 " + err);
        check(!repo.beginDownloadTask(kOwner,"bad-task","download-media","foreign-id",
                                      "/tmp/media.part",1,&err),"未被消息引用的 file_id 不入队");
        check(repo.beginDownloadTask(kOwner,"download-1","download-media","media-file-1",
                                     "/tmp/media.part",1,&err),"下载任务进入 Writer 队列 " + err);
        std::vector<DownloadTaskRow> pending;
        check(repo.listRecoverableDownloads(kOwner,&pending,&err)&&pending.size()==1&&
              pending[0].expectedSha256==media.sha256,"重启快照带消息权威摘要");
        check(repo.finishDownloadTask(kOwner,"download-1",1,2,1024,&err),"失败保留可恢复任务");
        db.close();
        check(db.open(kDir,kOwner,testKey(),&err),"重新打开加密库 " + err);
        check(repo.listRecoverableDownloads(kOwner,&pending,&err)&&pending.size()==1&&
              pending[0].transferred==1024,"重启后失败断点可读取");
        check(repo.beginDownloadTask(kOwner,"download-2","download-media","media-file-1",
                                     "/tmp/media.part",2,&err),"新 generation 接管旧任务");
        check(!repo.finishDownloadTask(kOwner,"download-1",1,3,4096,&err),"旧任务迟到完成不能复活");
        check(repo.finishDownloadTask(kOwner,"download-2",2,4,1024,&err),"取消成为终态");
        check(repo.beginDownloadTask(kOwner,"download-3","download-media","media-file-1",
                                     "/tmp/media.part",3,&err),"可创建新的显式请求");
        check(repo.finishDownloadTask(kOwner,"download-3",3,5,0,&err),"无效 part 可标记 abandoned");
        check(repo.listRecoverableDownloads(kOwner,&pending,&err)&&pending.empty(),"取消后不再恢复");
    }
    db.close();
    check(db.status() == DbStatus::Closed, "关闭后 Closed");

    ::system(("rm -rf " + std::string(kDir)).c_str());

    if (g_failures == 0) {
        std::cout << "test_native_repository PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_native_repository FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
