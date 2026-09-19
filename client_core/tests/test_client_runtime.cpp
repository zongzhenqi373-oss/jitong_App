// P7-G3：ClientRuntime 生命周期与隔离契约测试。
//
// 验收（v2 §3 P7-G3）：
//   - start/stop/logout/destroy 可重入、可并发；
//   - 旧 generation 事件必须失效（换账号/重登防串号）；
//   - destroy 风暴无 UAF、self-join、死锁；
//   - operationId 确定性（便于 Golden 复现）且完成登记 exactly-once。

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <unistd.h>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <future>
#include <filesystem>

#include "client_core/runtime/ClientRuntime.h"
#include "client_core/testing/DeterministicClock.h"

using namespace im::runtime;
using im::testing::DeterministicClock;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& name)
{
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << std::endl;
    if (!cond) ++g_failures;
}

ClientRuntime::Config makeCfg(std::int64_t ownerId)
{
    ClientRuntime::Config c;
    c.ownerId = ownerId;
    c.filesDir = "/tmp/test_runtime_" + std::to_string(ownerId);
    c.completionCapacity = 64;
    return c;
}
class RecordingRuntimeSink final : public IRuntimeEventSink {
public:
    void onInvalidated(InvalidationBus::Domain domain,std::int64_t version,
                       std::int64_t generation,std::int64_t owner) override {
        lastDomain=domain;lastVersion=version;lastGeneration=generation;lastOwner=owner;++invalidations;
    }
    void onOperationCompleted(const CompletionRegistry::Record& record,std::int64_t generation,
                              std::int64_t owner) override {
        lastOperation=record.operationId;lastResult=record.result;lastGeneration=generation;
        lastOwner=owner;++completions;
    }
    int invalidations=0,completions=0;std::int64_t lastVersion=0,lastGeneration=0,lastOwner=0;
    InvalidationBus::Domain lastDomain=InvalidationBus::Domain::Messages;
    std::string lastOperation;CompletionRegistry::Result lastResult=CompletionRegistry::Result::Ok;
};
} // namespace

int main()
{
    std::cout << "=== test_client_runtime ===" << std::endl;

    // [1] 状态机与幂等
    {
        DeterministicClock clock;
        ClientRuntime rt(makeCfg(1), &clock);
        check(rt.state() == ClientRuntime::State::Idle, "初始 Idle");

        std::string err;
        check(rt.start(&err), "start 成功 " + err);
        check(rt.state() == ClientRuntime::State::Running, "start 后 Running");
        check(rt.generation() == 1, "首次 start generation=1");

        check(rt.start(&err), "重复 start 幂等返回 true");
        check(rt.generation() == 1, "重复 start 不递增 generation");
        check(rt.isActive(), "isActive 为真");

        rt.stop();
        check(rt.state() == ClientRuntime::State::Stopped, "stop 后 Stopped");
        check(!rt.isActive(), "stop 后非 active");

        check(rt.start(&err), "stop 后可再次 start");
        check(rt.generation() == 2, "再次 start generation=2（新会话）");
    }

    // [2] destroy 幂等与终态
    {
        DeterministicClock clock;
        ClientRuntime rt(makeCfg(1), &clock);
        std::string err;
        rt.start(&err);
        rt.destroy();
        check(rt.state() == ClientRuntime::State::Destroyed, "destroy 后 Destroyed");
        rt.destroy();
        rt.destroy();
        check(rt.state() == ClientRuntime::State::Destroyed, "重复 destroy 幂等");

        check(!rt.start(&err), "销毁后不能 start");
        check(err.find("已销毁") != std::string::npos, "错误信息说明已销毁");
    }

    // [3] logout 递增 generation 并使旧事件失效
    {
        DeterministicClock clock;
        ClientRuntime rt(makeCfg(7), &clock);
        std::string err;
        rt.start(&err);
        const auto g1 = rt.generation();
        const auto op1 = rt.nextOperationId("send");

        rt.logout();
        check(rt.state() == ClientRuntime::State::Stopped, "logout 后 Stopped");
        check(rt.generation() > g1, "logout 递增 generation");
        check(!rt.isCurrentGeneration(g1), "旧 generation 事件失效（防串号）");

        rt.start(&err);
        check(rt.isCurrentGeneration(rt.generation()), "新 generation 事件有效");
        const auto op2 = rt.nextOperationId("send");
        check(op1 != op2, "logout 前后 operationId 不同（含 generation）");
        check(!rt.completeOperation(op1,ClientRuntime::Result::Ok),"旧 generation 迟到回调不能覆盖取消结果");
        CompletionRegistry::Record cancelled;
        check(rt.consumeOperation(op1,&cancelled) && cancelled.result==ClientRuntime::Result::Cancelled,
              "logout 对已接收命令保留可消费的取消终态");
    }

    // [4] operationId 确定性（同参数必同值，便于 Golden）
    {
        DeterministicClock clock;
        ClientRuntime a(makeCfg(1), &clock);
        ClientRuntime b(makeCfg(1), &clock);
        std::string err;
        a.start(&err);
        b.start(&err);
        check(a.nextOperationId("send") != b.nextOperationId("send"),
              "同账号 Runtime 重建不复用 operationId");

        const auto x = a.nextOperationId("send");
        const auto y = a.nextOperationId("send");
        check(x != y, "同一 Runtime 内 operationId 递增不重复");
        check(x.find("send:1:") == 0, "operationId 含 domain 与 ownerId");
    }

    // [5] 完成登记 exactly-once + 有界
    {
        DeterministicClock clock;
        ClientRuntime rt(makeCfg(1), &clock);
        std::string err;
        rt.start(&err);

        const auto op = rt.nextOperationId("sendText");
        check(rt.completeOperation(op, ClientRuntime::Result::Ok), "首次终结成功");
        check(!rt.completeOperation(op, ClientRuntime::Result::Failed),
              "重复终结被拒（exactly-once）");

        CompletionRegistry::Record rec;
        check(rt.consumeOperation(op, &rec), "消费终态");
        check(rec.result == ClientRuntime::Result::Ok, "终态保持首次结果（未被覆盖）");
    }

    // [6] 失效通知：dbVersion 单调 + 消费
    {
        DeterministicClock clock;
        ClientRuntime rt(makeCfg(1), &clock);
        std::string err;
        rt.start(&err);

        check(rt.publishInvalidation(InvalidationBus::Domain::Messages, 5, rt.generation()),
              "publish dbVersion=5");
        check(!rt.publishInvalidation(InvalidationBus::Domain::Messages, 5, rt.generation()),
              "重复 publish 被拒（非单调）");
        check(rt.consumeInvalidation(InvalidationBus::Domain::Messages) == 5,
              "consume 得到 5");
        check(rt.consumeInvalidation(InvalidationBus::Domain::Messages) == 0,
              "再次 consume 无新变更");
    }

    // [6b] Runtime 主动通知：UI 不轮询；旧 generation 仍由 Runtime 拒绝。
    {
        DeterministicClock clock;ClientRuntime rt(makeCfg(42),&clock);std::string err;rt.start(&err);
        auto sink=std::make_shared<RecordingRuntimeSink>();rt.setRuntimeEventSink(sink);
        check(rt.publishInvalidation(InvalidationBus::Domain::Messages,7,rt.generation()),
              "发布 Runtime invalidation");
        check(sink->invalidations==1&&sink->lastVersion==7&&sink->lastOwner==42&&
                  sink->lastGeneration==rt.generation(),"事件桥携带 domain/version/generation/owner");
        const auto op=rt.nextOperationId("event");
        check(rt.completeOperation(op,ClientRuntime::Result::Failed,"expected"),"终结事件操作");
        check(sink->completions==1&&sink->lastOperation==op&&
                  sink->lastResult==ClientRuntime::Result::Failed,"命令终态主动且 exactly-once 上抛");
        check(!rt.completeOperation(op,ClientRuntime::Result::Ok)&&sink->completions==1,
              "重复终结不重复通知平台");
    }

    // [7] 并发 destroy 风暴（无 UAF / self-join / 死锁）
    {
        DeterministicClock clock;
        ClientRuntime rt(makeCfg(1), &clock);
        std::string err;
        rt.start(&err);

        std::atomic<int> done{0};
        std::vector<std::thread> ts;
        for (int i = 0; i < 8; ++i) {
            ts.emplace_back([&]() {
                rt.destroy();
                done.fetch_add(1);
            });
        }
        for (auto& t : ts) t.join();
        check(done.load() == 8, "8 线程并发 destroy 全部返回（无死锁）");
        check(rt.state() == ClientRuntime::State::Destroyed, "终态 Destroyed");
    }

    // [8] start/stop 并发交错（不崩、状态合法）
    {
        DeterministicClock clock;
        ClientRuntime rt(makeCfg(1), &clock);
        std::vector<std::thread> ts;
        for (int i = 0; i < 4; ++i) {
            ts.emplace_back([&]() {
                std::string e;
                rt.start(&e);
            });
        }
        for (int i = 0; i < 4; ++i) {
            ts.emplace_back([&]() { rt.stop(); });
        }
        for (auto& t : ts) t.join();

        const auto s = rt.state();
        check(s == ClientRuntime::State::Stopped || s == ClientRuntime::State::Running ||
                  s == ClientRuntime::State::Stopping || s == ClientRuntime::State::Starting,
              "并发 start/stop 后状态合法（无非法态）");
        std::cout << "      最终状态: " << rt.stateName() << std::endl;
    }

    // [9] 双账号 Runtime 互不干扰
    {
        DeterministicClock clock;
        ClientRuntime a(makeCfg(1001), &clock);
        ClientRuntime b(makeCfg(1002), &clock);
        std::string err;
        a.start(&err);
        b.start(&err);

        check(a.ownerId() == 1001 && b.ownerId() == 1002, "各自 ownerId 正确");
        const auto oa = a.nextOperationId("send");
        const auto ob = b.nextOperationId("send");
        check(oa != ob, "不同账号 operationId 不同（含 ownerId）");
        check(oa.find(":1001:") != std::string::npos, "A 的 operationId 含 1001");
        check(ob.find(":1002:") != std::string::npos, "B 的 operationId 含 1002");
    }

    {
        ClientRuntime rt(makeCfg(1));
        auto database = std::make_shared<im::storage::NativeDatabase>();
        check(rt.setDatabase(database), "未启动时允许注入数据库");
        rt.start();
        auto snapshot = rt.database();
        check(snapshot == database, "数据库访问返回持有所有权的快照");
        check(!rt.setDatabase(nullptr), "运行中拒绝替换数据库");
        rt.destroy();
        check(!rt.database(), "销毁后不暴露数据库");
        check(snapshot != nullptr, "销毁后先前快照不悬空");
        check(!rt.start(), "destroy 后 start 不可复活");
        check(!rt.setDatabase(database), "destroy 后不可重新注入数据库");
    }

    {
        char path[]="/tmp/jitong-runtime-restart-XXXXXX";
        const char* dir=mkdtemp(path); check(dir!=nullptr,"创建 stop/restart 数据库目录");
        if(dir){
            auto db=std::make_shared<im::storage::NativeDatabase>(); std::string err;
            check(db->open(dir,1,std::vector<unsigned char>(32,0x44),&err),"打开 stop/restart 数据库 "+err);
            ClientRuntime rt(makeCfg(1)); check(rt.setDatabase(db),"注入已打开数据库");
            check(rt.start()&&rt.databaseReady(),"start 后数据库 Ready");
            rt.stop(); check(db->status()==im::storage::DbStatus::Ready,"stop 不关闭数据库");
            check(rt.start()&&rt.databaseReady(),"stop 后 restart 仍可读写数据库");
            rt.logout(); check(db->status()==im::storage::DbStatus::Closed,"logout 关闭数据库并清除密钥状态");
            check(!rt.database(),"logout 后必须重新注入数据库");
            rt.destroy(); std::filesystem::remove_all(dir);
        }
    }

    {
        // A blocked Writer pins destroy at drain, making the start/destroy race deterministic.
        char path[]="/tmp/jitong-runtime-race-XXXXXX";
        const char* dir=mkdtemp(path);
        check(dir!=nullptr,"创建独立生命周期测试目录");
        if(dir){
            auto db=std::make_shared<im::storage::NativeDatabase>();
            std::string err;
            const bool opened=db->open(dir,1,std::vector<unsigned char>(32,0x61),&err);
            check(opened,"生命周期测试数据库打开 " + err);
            if(opened){
                ClientRuntime rt(makeCfg(1)); rt.setDatabase(db); rt.start();
                std::promise<void> entered,release;
                auto released=release.get_future().share();
                im::storage::DbCommandQueue::Request req;
                req.fn=[&](sqlite3*){entered.set_value(); released.wait(); return true;};
                const auto submitted=db->submit(req);
                check(submitted==im::storage::CommandResult::Ok,"提交阻塞 Writer");
                if(submitted==im::storage::CommandResult::Ok){
                    entered.get_future().wait();
                    auto destroying=std::async(std::launch::async,[&]{rt.destroy();});
                    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                    while(rt.state()!=ClientRuntime::State::Destroying && std::chrono::steady_clock::now()<deadline)
                        std::this_thread::yield();
                    check(rt.state()==ClientRuntime::State::Destroying,"Writer drain 中处于 Destroying");
                    check(!rt.start(),"drain 尚未完成时禁止 start");
                    release.set_value(); destroying.get();
                    check(rt.state()==ClientRuntime::State::Destroyed && !rt.database(),"销毁终态无数据库复活");
                } else release.set_value();
            }
            db->close();
            std::filesystem::remove_all(dir);
        }
    }
    {
        auto cfg=makeCfg(1); cfg.completionCapacity=2;
        ClientRuntime rt(cfg); rt.start();
        check(!rt.databaseReady(),"Runtime started 不等于 Database ready");
        const auto a=rt.nextOperationId("send"), b=rt.nextOperationId("send");
        check(!a.empty() && !b.empty() && rt.nextOperationId("send").empty(),"容量满在接收新命令前背压");
        check(rt.completeOperation(a,ClientRuntime::Result::Ok) && rt.completeOperation(b,ClientRuntime::Result::Ok),
              "已经接收的命令仍能登记终态");
        check(rt.consumeOperation(a) && !rt.nextOperationId("send").empty(),"消费终态后恢复容量");
        rt.destroy();
        check(!rt.publishInvalidation(InvalidationBus::Domain::Messages,99,rt.generation()),"销毁后拒绝发布失效事件");
    }
    {
        char path[]="/tmp/jitong-runtime-message-XXXXXX"; const char* dir=mkdtemp(path);
        check(dir!=nullptr,"创建 Runtime 消息目录");
        if(dir){
            auto db=std::make_shared<im::storage::NativeDatabase>(); std::string err;
            check(db->open(dir,1,std::vector<unsigned char>(32,0x35),&err),"打开 Runtime 消息库 "+err);
            auto rt=std::make_shared<ClientRuntime>(makeCfg(1)); check(rt->setDatabase(db),"装配消息 Repository");
            check(rt->start(),"启动消息 Runtime");
            const auto sent=rt->sendText(9,9,"你好","nihao","nh");
            check(sent.accepted&&!sent.msgId.empty()&&!sent.operationId.empty(),"断网发送先接受并持久化");
            im::storage::NativeRepository repo(db); im::dto::MessageDto stored;
            check(repo.findMessage(1,sent.msgId,&stored)&&stored.status==0&&stored.content=="你好",
                  "断网消息为 SENDING 且正文可查询");
            im::dto::MessageDto image;image.conversationId=9;image.peerId=9;image.type=1;
            image.fileId="origin-id";image.fileName="photo.jpg";image.fileSize=1024;
            image.contentType="image/jpeg";image.sha256=std::string(64,'a');image.imgW=1200;image.imgH=800;
            image.thumbnailFileId="small-id";image.thumbnailSize=128;image.thumbnailSha256=std::string(64,'b');
            image.thumbnailW=240;image.thumbnailH=180;image.largeThumbnailFileId="large-id";
            image.largeThumbnailSize=512;image.largeThumbnailSha256=std::string(64,'c');
            image.largeThumbnailW=960;image.largeThumbnailH=640;image.localPath="/account/media/photo.jpg";
            const auto media=rt->sendMedia(image);
            check(media.accepted&&!media.msgId.empty(),"媒体卡片先落 Native Outbox");
            check(repo.findMessage(1,media.msgId,&stored)&&stored.type==1&&
                  stored.fileId=="origin-id"&&stored.thumbnailFileId=="small-id"&&
                  stored.largeThumbnailFileId=="large-id"&&stored.imgW==1200&&stored.imgH==800,
                  "媒体原图/大小缩略图元数据完整持久化");
            im::ChatProtocolMessage push; push.fromId=9;push.toId=1;push.msgId="push-1";
            push.content="收到";push.type=0;push.serverTime=100;push.conversationSeq=8;
            rt->handleIncomingChat(push);
            check(repo.findMessage(1,"push-1",&stored)&&!stored.fromMe&&stored.seq==8&&stored.status==2,
                  "结构化 Push 经统一 Inbox 落库");
            im::ChatProtocolMessage roamIn=push;roamIn.msgId="roam-in";roamIn.conversationSeq=6;
            im::ChatProtocolMessage roamOut=push;roamOut.fromId=1;roamOut.toId=9;
            roamOut.msgId="roam-out";roamOut.content="我发的历史";roamOut.conversationSeq=7;
            rt->handleRoamMessages(9,{roamOut,roamIn},false,6);
            check(repo.findMessage(1,"roam-in",&stored)&&!stored.fromMe&&stored.peerId==9,
                  "接收方向漫游消息按 owner 识别对端");
            check(repo.findMessage(1,"roam-out",&stored)&&stored.fromMe&&stored.peerId==9&&stored.status==1,
                  "发送方向漫游消息不被误判为收到消息");
            im::ChatProtocolMessage wrong=roamIn;wrong.msgId="wrong-peer";
            rt->handleRoamMessages(10,{wrong},false,6);
            check(!repo.findMessage(1,"wrong-peer",&stored),"漫游响应 peer 与消息身份不一致时拒绝入库");
            rt->handleRoamMessages(9,{roamOut,roamIn},false,6);
            im::storage::MessagePage page;
            check(repo.listConversation(1,9,nullptr,20,&page,&err)&&page.messages.size()==5,
                  "重复漫游由 msgId 幂等合并，不重复插入");
            const auto readOp=rt->markRead(9,8);
            CompletionRegistry::Record readDone;
            check(!readOp.empty()&&rt->consumeOperation(readOp,&readDone)&&
                      readDone.result==ClientRuntime::Result::Ok,
                  "已读意图原子提交并返回 exactly-once 终态");
            std::vector<im::dto::ConversationDto> conversations;
            check(repo.loadConversations(1,&conversations,&err)&&!conversations.empty()&&
                      conversations.front().unread==0,"已读后会话快照 unread=0");
            im::FriendProtocolInfo friendInfo;friendInfo.friendId=9;friendInfo.nick="张三";
            friendInfo.signature="Native friend";friendInfo.status=im::proto::STATUS_ONLINE;
            rt->handleFriendInfo(friendInfo);
            std::vector<im::dto::FriendDto> friends;
            check(rt->loadFriends(&friends,&err)&&friends.size()==1&&
                      friends.front().friendId==9&&friends.front().nick=="张三"&&friends.front().online,
                  "FriendInfo 经协议桥落库并合并 Presence");
            rt->handleFriendOffline(9);friends.clear();rt->loadFriends(&friends,&err);
            check(friends.size()==1&&!friends.front().online,"Offline 只更新 Presence，不删除好友事实");
            im::FriendProtocolRequest request;request.requesterId=9;request.targetId=1;
            request.requesterNick="张三";request.createdAt=123;
            rt->handleFriendRequestList({request});
            std::vector<im::dto::FriendRequestDto> friendRequests;
            check(rt->loadFriendRequests(&friendRequests,&err)&&friendRequests.size()==1&&
                      friendRequests.front().direction==im::dto::RequestDirection::Incoming,
                  "好友申请列表经统一入口幂等落库");
            rt->handleFriendRequestList({request});friendRequests.clear();
            check(rt->loadFriendRequests(&friendRequests,&err)&&friendRequests.size()==1,
                  "重复好友申请按稳定 requestId 去重");
            rt->handleDeleteFriendResult(im::proto::DELETE_FRIEND_SUCCESS,9);friends.clear();
            check(rt->loadFriends(&friends,&err)&&friends.empty(),"删除成功回执后更新 Native 好友事实");
            check(rt->consumeInvalidation(InvalidationBus::Domain::Friends)>0,
                  "好友变化发布 Friends 失效通知");
            check(rt->consumeInvalidation(InvalidationBus::Domain::Messages)>0,
                  "发送与接收发布可合并消息失效通知");
            rt->destroy(); std::filesystem::remove_all(dir);
        }
    }
    {
        // 可控假传输接受请求但永不返回：验证 deadline 会释放 single-flight、
        // 持久化退避，并在退避到期后自动重试，而不是永久卡死。
        char path[]="/tmp/jitong-runtime-gap-timeout-XXXXXX"; const char* dir=mkdtemp(path);
        check(dir!=nullptr,"创建补洞超时测试目录");
        if(dir){
            auto db=std::make_shared<im::storage::NativeDatabase>(); std::string err;
            check(db->open(dir,1,std::vector<unsigned char>(32,0x5a),&err),
                  "打开补洞超时测试库 "+err);
            auto cfg=makeCfg(1);cfg.gapRequestTimeoutMs=20;
            auto rt=std::make_shared<ClientRuntime>(cfg);
            std::mutex requestsMutex;std::condition_variable requestsCv;int requests=0;
            rt->setRoamRequestHookForTest([&](std::int64_t peer,std::int64_t before,int limit){
                {
                    std::lock_guard<std::mutex> lk(requestsMutex);++requests;
                    check(peer==9&&before==3&&limit==2,"补洞请求边界为缺失区间 [1,2]");
                }
                requestsCv.notify_all();return true; // 故意不回调 handleRoamMessages
            });
            check(rt->setDatabase(db)&&rt->start(),"启动带假漫游传输的 Runtime");
            rt->setAccountAuthenticated(true);
            im::ChatProtocolMessage jump; jump.fromId=9;jump.toId=1;jump.msgId="gap-jump";
            jump.content="seq jump";jump.type=0;jump.serverTime=100;jump.conversationSeq=3;
            rt->handleIncomingChat(jump);
            {
                std::unique_lock<std::mutex> lk(requestsMutex);
                check(requestsCv.wait_for(lk,std::chrono::milliseconds(2200),[&]{return requests>=2;}),
                      "无响应请求超时后按持久退避自动重试");
            }
            im::storage::NativeRepository repo(db);
            std::vector<im::storage::SyncGapRow> gaps;
            check(repo.loadSyncGaps(1,9,&gaps)&&!gaps.empty()&&gaps.front().attempt>=1&&
                      gaps.front().nextRetryAtMs>0,
                  "超时失败次数与 next_retry_at 已持久化");
            rt->destroy();std::filesystem::remove_all(dir);
        }
    }
    if (g_failures == 0) {
        std::cout << "test_client_runtime PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_client_runtime FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
