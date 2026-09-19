// P7-G4：MessageService（Outbox/Inbox 合并入口）编排测试。
//
// 覆盖 v2 §3 P7-G4 文本部分：
//   - C++ 发号（确定性 msg_id + local_order）；
//   - 发送草稿：messages(SENDING) + outbox + FTS + 会话摘要 单事务；
//   - Ack 合并：写 server_time/conversation_seq/status 并删除 outbox；
//   - Inbox 统一入口：幂等（msg_id）与未读/水位（Repository 语义已单独测过）。

#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include "im.pb.h"
#include "client_core/Protocol.h"

#include <sqlite3.h>

#include "client_core/dto/Dtos.h"
#include "client_core/message/MessageService.h"
#include "client_core/storage/NativeDatabase.h"
#include "client_core/storage/NativeRepository.h"

using namespace im::message;
using namespace im::storage;
using im::dto::MessageDto;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& name)
{
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << std::endl;
    if (!cond) ++g_failures;
}

const char* kDir = "/tmp/test_message_service";
const std::int64_t kOwner = 1;

std::vector<unsigned char> testKey() { return std::vector<unsigned char>(32, 0x55); }

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

MessageDto makeIntent(const std::string& content)
{
    MessageDto m;
    m.ownerId = kOwner;
    m.conversationId = 555;
    m.peerId = 2;
    m.type = 0;
    m.content = content;
    m.pinyin = "nihao";
    m.initials = "nh";
    return m;
}
} // namespace

int main()
{
    std::cout << "=== test_message_service ===" << std::endl;

    ::system(("rm -rf " + std::string(kDir)).c_str());
    ::system(("mkdir -p " + std::string(kDir)).c_str());

    NativeDatabase db;
    std::string err;
    check(db.open(kDir, kOwner, testKey(), &err), "打开库 " + err);
    NativeRepository repo(&db);
    MessageService svc(&repo);
    // 注入确定性 ID 生成器（生产用重启后唯一 ID，测试用固定序列保持可复现）
    int idCounter = 0;
    svc.setIdGenerator([&idCounter]() { return "test-" + std::to_string(idCounter++); });

    // [1] prepareOutgoing：C++ 发号 + 单事务写 messages/outbox/FTS/会话摘要
    {
        const SendResult r1 = svc.prepareOutgoing(makeIntent("hello world"));
        check(r1.ok, "prepareOutgoing 成功 " + r1.error);
        check(!r1.msgId.empty(), "生成了 msg_id");
        check(r1.msgId == "test-0", "msg_id 由注入生成器生成（test-0）");
        check(r1.localOrder == 1, "localOrder=1");

        check(queryInt(db, "SELECT count(*) FROM messages WHERE owner_id=1") == 1,
              "messages 有 1 条草稿");
        check(queryInt(db, "SELECT count(*) FROM outbox WHERE owner_id=1") == 1,
              "outbox 有 1 条待发送");
        check(queryInt(db, "SELECT count(*) FROM message_fts_identity WHERE owner_id=1") == 1,
              "FTS identity 写入草稿（发送中也可搜）");
        check(queryInt(db, "SELECT count(*) FROM conversations WHERE conversation_id=555") == 1,
              "会话摘要已建立");
        bool recoverable=false;
        db.withRead([&](sqlite3* d){
            sqlite3_stmt* s=nullptr;
            if(sqlite3_prepare_v2(d,"SELECT packet_type,payload,payload_version FROM outbox WHERE msg_id='test-0'",-1,&s,nullptr)==SQLITE_OK && sqlite3_step(s)==SQLITE_ROW){
                im::proto::ChatInfoRq rq;
                recoverable=sqlite3_column_int(s,0)==im::proto::DEF_PROT_CHAT_INFO_RQ &&
                    sqlite3_column_int(s,2)==1 && rq.ParseFromArray(sqlite3_column_blob(s,1),sqlite3_column_bytes(s,1)) &&
                    rq.myid()==1 && rq.friid()==2 && rq.msg_id()==r1.msgId && rq.msg()=="hello world" && rq.seq()==0;
            }
            sqlite3_finalize(s);
        });
        check(recoverable,"Outbox 保存版本化完整业务 protobuf，可重新加密发送");
    }

    // [2] 第二条草稿：localOrder 递增
    {
        const SendResult r2 = svc.prepareOutgoing(makeIntent("second"));
        check(r2.localOrder == 2, "第二条草稿 localOrder=2");
        check(r2.msgId == "test-1", "msg_id 递增（test-1）");
    }

    // [3] Ack 合并：写 server_time/seq/status 并删除 outbox
    {
        bool updated = false;
        check(svc.onSendAck(kOwner, "test-0", 1700000100, 50, 3, &updated, &err),
              "Ack 成功 " + err);
        check(updated, "Ack 更新了消息");
        check(queryInt(db, "SELECT conversation_seq FROM messages WHERE msg_id='test-0'") == 50,
              "Ack 后 conversation_seq=50");
        check(queryInt(db, "SELECT count(*) FROM outbox WHERE msg_id='test-0'") == 0,
              "Ack 后该消息 outbox 删除");
        check(queryInt(db, "SELECT count(*) FROM outbox WHERE owner_id=1") == 1,
              "仍有 1 条未 Ack 的 outbox（test-1）");
    }

    // [4] Inbox 统一入口：幂等（同一 msg_id 只 inserted 一次）
    {
        IncomingResult in1 = svc.onIncoming(makeIntent("incoming"));
        // makeIntent 没有 msgId，需手动设置
        MessageDto inc = makeIntent("incoming msg");
        inc.msgId = "remote-1";
        inc.seq = 10;
        inc.ts = 1700000200;
        inc.fromMe = false;
        inc.status = 3;
        in1 = svc.onIncoming(inc);
        check(in1.ok && in1.inserted, "首次接收 inserted=true " + in1.error);

        IncomingResult in2 = svc.onIncoming(inc);
        check(in2.ok && !in2.inserted, "重复接收 inserted=false（幂等）");

        check(queryInt(db, "SELECT count(*) FROM messages WHERE msg_id='remote-1'") == 1,
              "重复接收不增行");
    }

    // [5] 收到更旧消息后水位不回退（复用已修的 Repository 语义）
    {
        MessageDto newest = makeIntent("n");
        newest.msgId = "remote-100";
        newest.seq = 100;
        newest.ts = 1700001000;
        newest.fromMe = false;
        svc.onIncoming(newest);

        MessageDto older = makeIntent("o");
        older.msgId = "remote-90";
        older.seq = 90;
        older.ts = 1700000900;
        older.fromMe = false;
        svc.onIncoming(older);

        check(queryInt(db, "SELECT synced_seq FROM sync_watermarks "
                           "WHERE owner_id=1 AND domain='maxseen'") == 100,
              "水位保持 100（先 100 后 90 不回退）");
    }

    // Regression: retained outbox + reconstructed service/database must never reuse an order.
    const auto previousMax = queryInt(db, "SELECT MAX(local_order) FROM messages");
    db.close();
    check(db.open(kDir, kOwner, testKey(), &err), "重开数据库 " + err);
    MessageService restarted(&repo);
    const auto afterRestart = restarted.prepareOutgoing(makeIntent("after restart"));
    check(afterRestart.ok && afterRestart.localOrder > previousMax,
          "重启后发号大于已持久化最大值 " + afterRestart.error);
    check(afterRestart.msgId.size() == 32, "生产 ID 为 128 位随机数十六进制编码");
    std::atomic<int> successes{0};
    std::vector<std::thread> senders;
    for (int i = 0; i < 8; ++i) senders.emplace_back([&] {
        MessageService independent(&repo);
        if (independent.prepareOutgoing(makeIntent("concurrent")).ok) ++successes;
    });
    for (auto& sender : senders) sender.join();
    check(successes == 8, "8 个 Service 并发发号均提交成功");
    check(queryInt(db, "SELECT COUNT(*)-COUNT(DISTINCT local_order) FROM outbox") == 0,
          "Outbox 无重复 local_order");
    check(queryInt(db, "PRAGMA user_version") == 8, "库已升级到 v8");
    {
        std::vector<OutboxAttempt> leased,other;
        check(repo.claimOutbox(kOwner,100,30,64,&leased,&err) && !leased.empty(),"领取持久化 Outbox " + err);
        check(repo.claimOutbox(kOwner,100,30,64,&other,&err) && other.empty(),"租约内不会被重复领取");
        if(!leased.empty()){
            const auto a=leased.front();
            check(a.attempt==1,"第一次领取才计 attempt=1");
            im::dto::MessageDto stored;
            check(repo.findMessage(kOwner,a.msgId,&stored) && stored.msgId==a.msgId,"按 msgId 查询提交结果");
            check(repo.finishOutboxAttempt(kOwner,a.msgId,a.attempt,false,105,"temporary",&err),"失败安排重试");
            check(repo.finishOutboxAttempt(kOwner,a.msgId,a.attempt,false,999,"duplicate",&err),"重复失败幂等");
            check(repo.claimOutbox(kOwner,104,30,64,&other,&err) && other.empty(),"到期前不重试");
            check(repo.claimOutbox(kOwner,105,30,64,&other,&err) && other.size()==1 && other[0].attempt==2,
                  "重复回调不推迟重试，第二次领取 attempt=2");
            check(repo.finishOutboxAttempt(kOwner,a.msgId,1,true,0,"stale",&err),"旧 attempt 迟到响应无影响");
            check(restarted.onProtocolAck(kOwner,a.msgId,im::proto::CHAT_RESULT_SUCC,501,2,106,&err),
                  "协议 result=0 正确映射已送达（不是发送中）");
            check(repo.findMessage(kOwner,a.msgId,&stored) && stored.status==1 && stored.seq==501,
                  "回执更新状态与权威 seq");
        }
        if(leased.size()>1){
            const auto a=leased[1];
            check(repo.cancelOutbox(kOwner,a.msgId,&err),"取消保留消息但停止自动重试");
            check(repo.claimOutbox(kOwner,1000,30,64,&other,&err),"重启式到期领取");
            bool cancelledAbsent=true;
            for(const auto& entry:other) if(entry.msgId==a.msgId) cancelledAbsent=false;
            check(cancelledAbsent,"已取消任务不会恢复发送");
        }
    }
    db.close();
    ::system(("rm -rf " + std::string(kDir)).c_str());

    if (g_failures == 0) {
        std::cout << "test_message_service PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_message_service FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
