#include "client_core/runtime/ClientRuntime.h"
#include "im.pb.h"

#include <algorithm>
#include <chrono>

namespace im {
namespace runtime {
namespace { std::atomic<std::uint64_t> runtimeInstance{0}; }

namespace {
std::int64_t systemNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
}

namespace {
class RuntimeMessageBridge final : public im::IMessageProtocolSink {
public:
    explicit RuntimeMessageBridge(std::weak_ptr<ClientRuntime> runtime):m_runtime(std::move(runtime)){}
    void onIncomingChat(const im::ChatProtocolMessage& m) override
    { if(auto runtime=m_runtime.lock())runtime->handleIncomingChat(m); }
    void onChatAck(const im::ChatProtocolAck& a) override
    { if(auto runtime=m_runtime.lock())runtime->handleChatAck(a); }
    void onRoamConversations(const std::vector<im::ChatProtocolMessage>& m) override
    { if(auto runtime=m_runtime.lock())runtime->handleRoamConversations(m); }
    void onRoamMessages(std::int64_t peerId,const std::vector<im::ChatProtocolMessage>& m,
                        bool hasMore,std::int64_t minSeq) override
    { if(auto runtime=m_runtime.lock())runtime->handleRoamMessages(peerId,m,hasMore,minSeq); }
private:
    std::weak_ptr<ClientRuntime> m_runtime;
};
class RuntimeFriendBridge final : public im::IFriendProtocolSink {
public:
    explicit RuntimeFriendBridge(std::weak_ptr<ClientRuntime> runtime):m_runtime(std::move(runtime)){}
    void onFriendInfo(const im::FriendProtocolInfo& v) override
    {if(auto r=m_runtime.lock())r->handleFriendInfo(v);}
    void onFriendRequest(const im::FriendProtocolRequest& v) override
    {if(auto r=m_runtime.lock())r->handleFriendRequest(v);}
    void onFriendRequestList(const std::vector<im::FriendProtocolRequest>& v) override
    {if(auto r=m_runtime.lock())r->handleFriendRequestList(v);}
    void onFriendOffline(std::int64_t id) override
    {if(auto r=m_runtime.lock())r->handleFriendOffline(id);}
    void onDeleteFriendResult(int result,std::int64_t id) override
    {if(auto r=m_runtime.lock())r->handleDeleteFriendResult(result,id);}
private:
    std::weak_ptr<ClientRuntime> m_runtime;
};
}

const char* ClientRuntime::stateName(State s)
{
    switch (s) {
        case State::Idle:      return "Idle";
        case State::Starting:  return "Starting";
        case State::Running:   return "Running";
        case State::Stopping:  return "Stopping";
        case State::Stopped:   return "Stopped";
        case State::Destroyed: return "Destroyed";
        case State::Destroying: return "Destroying";
    }
    return "?";
}

const char* ClientRuntime::stateName() const { return stateName(state()); }

ClientRuntime::ClientRuntime(const Config& cfg, im::testing::IClock* clock)
    : m_cfg(cfg),
      m_ownerId(cfg.ownerId),
      m_clock(clock),
      m_instanceId(++runtimeInstance),
      m_completions(cfg.completionCapacity, clock)
{
    // generation 从 1 开始：0 保留给"尚未启动/未知"，便于识别陈旧事件
    m_generation.store(0);
}

ClientRuntime::~ClientRuntime() { destroy(); }

void ClientRuntime::setRuntimeEventSink(std::shared_ptr<IRuntimeEventSink> sink)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if(m_state==State::Destroyed||m_state==State::Destroying)return;
    m_runtimeEventSink=std::move(sink);
}

bool ClientRuntime::publishInvalidation(InvalidationBus::Domain domain,std::int64_t dbVersion,
                                        std::int64_t sourceGeneration)
{
    std::shared_ptr<IRuntimeEventSink> sink;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if(m_state!=State::Running||sourceGeneration!=m_generation.load())return false;
        if(!m_invalidations.publish(domain,dbVersion))return false;
        sink=m_runtimeEventSink;
    }
    if(sink)sink->onInvalidated(domain,dbVersion,sourceGeneration,m_ownerId);
    return true;
}

bool ClientRuntime::completeOperation(const OperationId& id,Result result,const std::string& error)
{
    std::shared_ptr<IRuntimeEventSink> sink;CompletionRegistry::Record record;
    const auto generation=m_generation.load();
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if(m_state!=State::Running||!m_admitted.count(id))return false;
        if(!m_completions.complete(id,result,error)||!m_completions.peek(id,&record))return false;
        sink=m_runtimeEventSink;
    }
    if(sink)sink->onOperationCompleted(record,generation,m_ownerId);
    return true;
}

bool ClientRuntime::setDatabase(std::shared_ptr<im::storage::NativeDatabase> db)
{
    // Declared before the guards so its destructor (possibly close/join) runs after unlocking.
    std::shared_ptr<im::storage::NativeDatabase> previous;
    std::shared_ptr<im::storage::NativeRepository> previousRepository;
    std::shared_ptr<im::message::MessageService> previousService;
    std::shared_ptr<im::friend_service::FriendService> previousFriendService;
    std::shared_ptr<im::sync::SyncService> previousSync;
    std::shared_ptr<im::storage::NativeRepository> repository;
    std::shared_ptr<im::message::MessageService> service;
    std::shared_ptr<im::friend_service::FriendService> friendService;
    std::shared_ptr<im::sync::SyncService> sync;
    if(db){repository=std::make_shared<im::storage::NativeRepository>(db);
           service=std::make_shared<im::message::MessageService>(repository.get());
           friendService=std::make_shared<im::friend_service::FriendService>(
               m_ownerId,repository.get());
           sync=std::make_shared<im::sync::SyncService>(m_ownerId,repository.get(),m_clock);}
    std::lock_guard<std::mutex> control(m_control);
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_state != State::Idle && m_state != State::Stopped) return false;
    if (db && db->ownerId()!=0 && db->ownerId()!=m_ownerId) return false;
    previous = std::move(m_db);
    previousRepository=std::move(m_repository); previousService=std::move(m_messageService);
    previousFriendService=std::move(m_friendService);
    previousSync=std::move(m_syncService);
    m_db = std::move(db);
    m_repository=std::move(repository); m_messageService=std::move(service);
    m_friendService=std::move(friendService);m_syncService=std::move(sync);
    return true;
}

bool ClientRuntime::loadFriends(std::vector<im::dto::FriendDto>* out,std::string* err) const
{
    std::shared_ptr<im::friend_service::FriendService> service;
    {std::lock_guard<std::mutex> lk(m_mutex);service=m_friendService;}
    if(!service){if(err)*err="friend service not ready";return false;}
    return service->loadFriends(out,err);
}

bool ClientRuntime::loadFriendRequests(std::vector<im::dto::FriendRequestDto>* out,
                                       std::string* err) const
{
    std::shared_ptr<im::friend_service::FriendService> service;
    {std::lock_guard<std::mutex> lk(m_mutex);service=m_friendService;}
    if(!service){if(err)*err="friend service not ready";return false;}
    return service->loadFriendRequests(out,err);
}

bool ClientRuntime::requestFriendRequests()
{
    std::shared_ptr<im::ClientCore> core;
    {std::lock_guard<std::mutex> lk(m_mutex);core=m_core;}
    if(!m_accountAuthenticated.load()||!core||!core->isConnected())return false;
    core->requestFriendRequests();return true;
}

bool ClientRuntime::sendAddFriendRequest(const std::string& nick)
{
    std::shared_ptr<im::ClientCore> core;
    {std::lock_guard<std::mutex> lk(m_mutex);core=m_core;}
    if(!m_accountAuthenticated.load()||!core||!core->isConnected()||nick.empty())return false;
    core->sendAddFriendRequest(nick);return true;
}

bool ClientRuntime::answerFriendRequest(std::int64_t id,const std::string& nick,bool agree)
{
    std::shared_ptr<im::ClientCore> core;
    {std::lock_guard<std::mutex> lk(m_mutex);core=m_core;}
    if(!m_accountAuthenticated.load()||!core||!core->isConnected()||id<=0)return false;
    core->answerAddFriend(static_cast<int>(id),nick,agree);return true;
}

bool ClientRuntime::deleteFriend(std::int64_t id)
{
    std::shared_ptr<im::ClientCore> core;
    {std::lock_guard<std::mutex> lk(m_mutex);core=m_core;}
    if(!m_accountAuthenticated.load()||!core||!core->isConnected()||id<=0)return false;
    core->deleteFriend(static_cast<int>(id));return true;
}

void ClientRuntime::handleFriendInfo(const im::FriendProtocolInfo& value)
{
    std::shared_ptr<im::friend_service::FriendService> service;
    {std::lock_guard<std::mutex> lk(m_mutex);service=m_friendService;}
    if(!service||value.friendId<=0)return;
    im::dto::FriendDto dto;dto.friendId=value.friendId;dto.nick=value.nick;
    dto.avatar=std::to_string(value.iconId);dto.signature=value.signature;
    dto.online=value.status==im::proto::STATUS_ONLINE;std::string error;
    if(service->onFriendInfo(dto,&error)){
        service->onPresence(dto.friendId,dto.online);
        publishInvalidation(InvalidationBus::Domain::Friends,++m_dbVersion,m_generation.load());
    }
}

void ClientRuntime::handleFriendRequest(const im::FriendProtocolRequest& value)
{ handleFriendRequestList({value}); }

void ClientRuntime::handleFriendRequestList(const std::vector<im::FriendProtocolRequest>& values)
{
    std::shared_ptr<im::friend_service::FriendService> service;
    {std::lock_guard<std::mutex> lk(m_mutex);service=m_friendService;}
    if(!service)return;bool changed=false;
    for(const auto& value:values){
        if(value.requesterId<=0||value.targetId<=0)continue;
        im::dto::FriendRequestDto dto;dto.fromUserId=value.requesterId;dto.toUserId=value.targetId;
        dto.createdAt=value.createdAt;dto.message=value.requesterNick;dto.direction=value.targetId==m_ownerId?
            im::dto::RequestDirection::Incoming:im::dto::RequestDirection::Outgoing;
        dto.requestId=std::to_string(value.requesterId)+":"+std::to_string(value.targetId)+":"+
            std::to_string(value.createdAt);bool inserted=false;std::string error;
        if(service->onFriendRequest(dto,&inserted,&error))changed=true;
    }
    if(changed)publishInvalidation(InvalidationBus::Domain::Friends,++m_dbVersion,m_generation.load());
}

void ClientRuntime::handleFriendOffline(std::int64_t id)
{
    std::shared_ptr<im::friend_service::FriendService> service;
    {std::lock_guard<std::mutex> lk(m_mutex);service=m_friendService;}
    if(!service)return;service->onPresence(id,false);
    publishInvalidation(InvalidationBus::Domain::Friends,++m_dbVersion,m_generation.load());
}

void ClientRuntime::handleDeleteFriendResult(int result,std::int64_t id)
{
    if(result!=im::proto::DELETE_FRIEND_SUCCESS)return;
    std::shared_ptr<im::friend_service::FriendService> service;
    {std::lock_guard<std::mutex> lk(m_mutex);service=m_friendService;}
    bool deleted=false;std::string error;
    if(service&&service->deleteFriend(id,&deleted,&error))
        publishInvalidation(InvalidationBus::Domain::Friends,++m_dbVersion,m_generation.load());
}

bool ClientRuntime::attachClientCore(const std::shared_ptr<im::ClientCore>& core)
{
    if(!core)return false;
    const auto weak=weak_from_this(); if(weak.expired())return false;
    auto bridge=std::make_shared<RuntimeMessageBridge>(weak);
    auto friendBridge=std::make_shared<RuntimeFriendBridge>(weak);
    std::shared_ptr<im::ClientCore> previous;
    {
        std::lock_guard<std::mutex> control(m_control); std::lock_guard<std::mutex> lk(m_mutex);
        if(m_state!=State::Idle&&m_state!=State::Stopped)return false;
        previous=std::move(m_core);m_core=core;m_messageBridge=bridge;m_friendBridge=friendBridge;
    }
    if(previous&&previous!=core)previous->setMessageProtocolSink({});
    core->setMessageProtocolSink(bridge);core->setFriendProtocolSink(friendBridge);return true;
}

ClientRuntime::MessageCommand ClientRuntime::sendText(std::int64_t conversationId,
    std::int64_t peerId,const std::string& text,const std::string& pinyin,const std::string& initials)
{
    MessageCommand out; out.operationId=nextOperationId("send_text");
    if(out.operationId.empty()){out.error="runtime not running or command capacity full";return out;}
    std::shared_ptr<im::message::MessageService> service;
    {std::lock_guard<std::mutex> lk(m_mutex);service=m_messageService;}
    if(!service||text.empty()||text.size()>=im::proto::CHAT_MSG_LEN){
        out.error=!service?"database/message service not ready":
            (text.empty()?"empty text":"text exceeds protocol byte limit");
        completeOperation(out.operationId,Result::Failed,out.error); return out;
    }
    im::dto::MessageDto intent; intent.ownerId=m_ownerId; intent.conversationId=conversationId;
    intent.peerId=peerId; intent.type=0; intent.content=text; intent.pinyin=pinyin; intent.initials=initials;
    const auto prepared=service->prepareOutgoing(intent); out.msgId=prepared.msgId;
    out.localOrder=prepared.localOrder; out.accepted=prepared.ok; out.error=prepared.error;
    if(!prepared.ok){completeOperation(out.operationId,Result::Failed,prepared.error);return out;}
    {std::lock_guard<std::mutex> lk(m_mutex);m_inflightMessages[prepared.msgId]={0,out.operationId};}
    publishInvalidation(InvalidationBus::Domain::Messages,++m_dbVersion,m_generation.load());
    publishInvalidation(InvalidationBus::Domain::Conversations,++m_dbVersion,m_generation.load());
    wakeOutboxPump(); return out;
}

ClientRuntime::MessageCommand ClientRuntime::sendMedia(const im::dto::MessageDto& input)
{
    MessageCommand out; out.operationId=nextOperationId("send_media");
    if(out.operationId.empty()){out.error="runtime not running or command capacity full";return out;}
    std::shared_ptr<im::message::MessageService> service;
    {std::lock_guard<std::mutex> lk(m_mutex);service=m_messageService;}
    const bool image=input.type==static_cast<int>(im::proto::IMAGE);
    const bool file=input.type==static_cast<int>(im::proto::FILE);
    if(!service||(!image&&!file)||input.conversationId<=0||input.peerId<=0||
       input.fileId.empty()||input.fileName.empty()||input.fileSize<=0||
       input.fileSize>im::proto::FILE_MAX_SIZE||input.contentType.empty()||input.sha256.size()!=64||
       (image&&(input.imgW<=0||input.imgH<=0))){
        out.error=!service?"database/message service not ready":"invalid media intent";
        completeOperation(out.operationId,Result::Failed,out.error);return out;
    }
    auto intent=input;intent.ownerId=m_ownerId;intent.content=image?"[图片]":"[文件]";
    const auto prepared=service->prepareOutgoing(intent);out.msgId=prepared.msgId;
    out.localOrder=prepared.localOrder;out.accepted=prepared.ok;out.error=prepared.error;
    if(!prepared.ok){completeOperation(out.operationId,Result::Failed,prepared.error);return out;}
    {std::lock_guard<std::mutex> lk(m_mutex);m_inflightMessages[prepared.msgId]={0,out.operationId};}
    publishInvalidation(InvalidationBus::Domain::Messages,++m_dbVersion,m_generation.load());
    publishInvalidation(InvalidationBus::Domain::Conversations,++m_dbVersion,m_generation.load());
    wakeOutboxPump();return out;
}

ClientRuntime::OperationId ClientRuntime::markRead(std::int64_t conversationId,std::int64_t readSeq)
{
    const auto operation=nextOperationId("mark_read");if(operation.empty())return {};
    std::shared_ptr<im::message::MessageService> service;
    {std::lock_guard<std::mutex> lk(m_mutex);service=m_messageService;}
    std::string error;
    if(!service||conversationId<=0||readSeq<0||!service->markRead(m_ownerId,conversationId,readSeq,&error)){
        completeOperation(operation,Result::Failed,error.empty()?"invalid mark-read intent":error);
        return operation;
    }
    publishInvalidation(InvalidationBus::Domain::Conversations,++m_dbVersion,m_generation.load());
    completeOperation(operation,Result::Ok);return operation;
}

int ClientRuntime::flushOutbox(std::int64_t nowSeconds,int limit)
{
    std::shared_ptr<im::storage::NativeRepository> repository; std::shared_ptr<im::ClientCore> core;
    {std::lock_guard<std::mutex> lk(m_mutex);if(m_state!=State::Running)return 0;
     repository=m_repository;core=m_core;}
    if(!m_accountAuthenticated.load()||!repository||!core||!core->isConnected())return 0;
    std::vector<im::storage::OutboxAttempt> attempts; std::string error;
    if(!repository->claimOutbox(m_ownerId,nowSeconds,30,limit,&attempts,&error))return 0;
    int sent=0; std::int64_t nextWake=0;
    for(const auto& attempt:attempts){
        if(core->sendChatPayload(attempt.payload)){
            std::lock_guard<std::mutex> lk(m_mutex); auto& state=m_inflightMessages[attempt.msgId];
            state.attempt=attempt.attempt; ++sent; nextWake=nowSeconds+30;
        } else {
            repository->finishOutboxAttempt(m_ownerId,attempt.msgId,attempt.attempt,false,
                                             nowSeconds+1,"not_connected",nullptr);
            if(nextWake==0||nowSeconds+1<nextWake)nextWake=nowSeconds+1;
        }
    }
    if(nextWake>0)scheduleOutboxPump(nextWake);
    return sent;
}

void ClientRuntime::setAccountAuthenticated(bool authenticated)
{
    m_accountAuthenticated.store(authenticated);
    if(authenticated){
        wakeOutboxPump();
        std::vector<std::int64_t> pending;
        {std::lock_guard<std::mutex> lk(m_mutex);pending.assign(m_gapPending.begin(),m_gapPending.end());}
        for(const auto conversationId:pending)maybeRequestGap(conversationId);
    }
}

void ClientRuntime::wakeOutboxPump()
{
    {std::lock_guard<std::mutex> lk(m_outboxMutex);m_outboxWake=true;}
    m_outboxCv.notify_one();
}

void ClientRuntime::scheduleOutboxPump(std::int64_t epochSeconds)
{
    if(epochSeconds<=0)return;
    {
        std::lock_guard<std::mutex> lk(m_outboxMutex);
        if(m_outboxWakeAt==0||epochSeconds<m_outboxWakeAt)m_outboxWakeAt=epochSeconds;
    }
    m_outboxCv.notify_one();
}

void ClientRuntime::startOutboxPump()
{
    std::lock_guard<std::mutex> lk(m_outboxMutex);
    if(m_outboxThread.joinable())return;
    m_outboxStop=false;m_outboxWake=true;
    m_outboxThread=std::thread([this]{runOutboxPump();});
}

void ClientRuntime::stopOutboxPump()
{
    {
        std::lock_guard<std::mutex> lk(m_outboxMutex);
        m_outboxStop=true;m_outboxWake=true;
    }
    m_outboxCv.notify_all();
    if(m_outboxThread.joinable())m_outboxThread.join();
}

void ClientRuntime::runOutboxPump()
{
    std::unique_lock<std::mutex> lk(m_outboxMutex);
    while(!m_outboxStop){
        if(m_accountAuthenticated.load()&&m_outboxWakeAt>0){
            const auto deadline=std::chrono::system_clock::time_point(
                std::chrono::seconds(m_outboxWakeAt));
            m_outboxCv.wait_until(lk,deadline);
        } else m_outboxCv.wait(lk);
        if(m_outboxStop)break;
        const auto now=std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const bool immediate=m_outboxWake;
        m_outboxWake=false;
        if(!m_accountAuthenticated.load())continue;
        if(!immediate&&m_outboxWakeAt>now)continue; // 定时被提前更新或虚假唤醒
        if(m_outboxWakeAt<=now)m_outboxWakeAt=0;
        lk.unlock();
        flushOutbox(now,32);
        lk.lock();
    }
}

void ClientRuntime::startSyncTimer()
{
    std::lock_guard<std::mutex> lk(m_syncTimerMutex);
    if(m_syncTimerThread.joinable())return;
    m_syncTimerStop=false;
    m_syncTimerThread=std::thread([this]{runSyncTimer();});
}

void ClientRuntime::stopSyncTimer()
{
    {
        std::lock_guard<std::mutex> lk(m_syncTimerMutex);
        m_syncTimerStop=true;m_gapTimers.clear();
    }
    m_syncTimerCv.notify_all();
    if(m_syncTimerThread.joinable())m_syncTimerThread.join();
}

void ClientRuntime::armGapTimer(std::int64_t conversationId,std::int64_t atMs,bool requestTimeout)
{
    if(conversationId<=0||atMs<=0)return;
    {
        std::lock_guard<std::mutex> lk(m_syncTimerMutex);
        m_gapTimers[conversationId]={atMs,requestTimeout};
    }
    m_syncTimerCv.notify_one();
}

void ClientRuntime::cancelGapTimer(std::int64_t conversationId)
{
    {std::lock_guard<std::mutex> lk(m_syncTimerMutex);m_gapTimers.erase(conversationId);}
    m_syncTimerCv.notify_one();
}

void ClientRuntime::runSyncTimer()
{
    std::unique_lock<std::mutex> lk(m_syncTimerMutex);
    while(!m_syncTimerStop){
        if(m_gapTimers.empty()){m_syncTimerCv.wait(lk);continue;}
        auto next=std::min_element(m_gapTimers.begin(),m_gapTimers.end(),
            [](const auto& a,const auto& b){return a.second.atMs<b.second.atMs;});
        const auto conversationId=next->first;const auto expected=next->second;
        const auto deadline=std::chrono::system_clock::time_point(std::chrono::milliseconds(expected.atMs));
        if(m_syncTimerCv.wait_until(lk,deadline)!=std::cv_status::timeout)continue;
        auto current=m_gapTimers.find(conversationId);
        if(current==m_gapTimers.end()||current->second.atMs!=expected.atMs||
           current->second.requestTimeout!=expected.requestTimeout)continue;
        const auto timer=current->second;m_gapTimers.erase(current);lk.unlock();
        if(timer.requestTimeout){
            bool wasInFlight=false;std::shared_ptr<im::sync::SyncService> sync;
            {
                std::lock_guard<std::mutex> state(m_mutex);
                wasInFlight=m_gapInFlight.erase(conversationId)>0;
                m_gapPages.erase(conversationId);m_gapPending.insert(conversationId);sync=m_syncService;
            }
            if(wasInFlight&&sync){
                sync->markGapFailed(conversationId);
                const auto retryAt=sync->nextRetryAtMs(conversationId);
                if(retryAt>0)armGapTimer(conversationId,retryAt,false);
            }
        } else maybeRequestGap(conversationId);
        lk.lock();
    }
}

bool ClientRuntime::storeProtocolMessage(const im::ChatProtocolMessage& p,bool roaming)
{
    std::shared_ptr<im::message::MessageService> service;std::shared_ptr<im::sync::SyncService> sync;
    {std::lock_guard<std::mutex> lk(m_mutex);if(m_state!=State::Running)return false;
     service=m_messageService;sync=m_syncService;}
    const bool fromMe=p.fromId==m_ownerId;
    const auto peerId=fromMe?p.toId:p.fromId;
    if(!service||peerId<=0||(fromMe?p.fromId:p.toId)!=m_ownerId||p.msgId.empty()||
       p.conversationSeq<=0)return false;
    im::dto::MessageDto m; m.ownerId=m_ownerId;m.msgId=p.msgId;m.conversationId=peerId;m.peerId=peerId;
    m.fromMe=fromMe;m.seq=p.conversationSeq;m.ts=p.serverTime;m.type=p.type;m.content=p.content;
    m.status=fromMe?1:2;
    m.fileId=p.fileId;m.fileName=p.fileName;m.fileSize=p.fileSize;m.contentType=p.contentType;m.sha256=p.sha256;
    m.imgW=p.imageWidth;m.imgH=p.imageHeight;m.thumbnailFileId=p.thumbnailFileId;
    m.thumbnailSize=p.thumbnailSize;m.thumbnailSha256=p.thumbnailSha256;m.thumbnailW=p.thumbnailWidth;
    m.thumbnailH=p.thumbnailHeight;m.largeThumbnailFileId=p.largeThumbnailFileId;
    m.largeThumbnailSize=p.largeThumbnailSize;m.largeThumbnailSha256=p.largeThumbnailSha256;
    m.largeThumbnailW=p.largeThumbnailWidth;m.largeThumbnailH=p.largeThumbnailHeight;
    im::storage::IncomingContext ctx;ctx.isRoaming=roaming;
    const bool ok=service->onIncoming(m,ctx).ok;
    if(ok&&sync){
        sync->observeSeq(peerId,p.conversationSeq);
        if(sync->hasGaps(peerId)){std::lock_guard<std::mutex> lk(m_mutex);m_gapPending.insert(peerId);}
    }
    return ok;
}

void ClientRuntime::handleIncomingChat(const im::ChatProtocolMessage& p)
{
    const auto generation=m_generation.load();
    if(storeProtocolMessage(p,false)){
        publishInvalidation(InvalidationBus::Domain::Messages,++m_dbVersion,generation);
        publishInvalidation(InvalidationBus::Domain::Conversations,++m_dbVersion,generation);
    }
    maybeRequestGap(p.fromId==m_ownerId?p.toId:p.fromId);
}

void ClientRuntime::handleRoamConversations(const std::vector<im::ChatProtocolMessage>& messages)
{
    const auto generation=m_generation.load(); bool changed=false;
    for(const auto& message:messages)changed=storeProtocolMessage(message,true)||changed;
    if(changed){
        publishInvalidation(InvalidationBus::Domain::Messages,++m_dbVersion,generation);
        publishInvalidation(InvalidationBus::Domain::Conversations,++m_dbVersion,generation);
    }
}

void ClientRuntime::handleRoamMessages(std::int64_t peerId,
    const std::vector<im::ChatProtocolMessage>& messages,bool hasMore,std::int64_t minSeq)
{
    (void)minSeq; const auto generation=m_generation.load(); bool changed=false;
    cancelGapTimer(peerId);
    {std::lock_guard<std::mutex> lk(m_mutex);m_gapInFlight.erase(peerId);}
    for(const auto& message:messages){
        const auto actualPeer=message.fromId==m_ownerId?message.toId:message.fromId;
        if(actualPeer==peerId)changed=storeProtocolMessage(message,true)||changed;
    }
    if(changed){
        publishInvalidation(InvalidationBus::Domain::Messages,++m_dbVersion,generation);
        publishInvalidation(InvalidationBus::Domain::Conversations,++m_dbVersion,generation);
    }
    std::shared_ptr<im::sync::SyncService> sync;
    {std::lock_guard<std::mutex> lk(m_mutex);sync=m_syncService;}
    if(!changed||!hasMore){
        if(sync&&sync->hasGaps(peerId)){
            sync->markGapFailed(peerId);
            const auto retryAt=sync->nextRetryAtMs(peerId);
            if(retryAt>0)armGapTimer(peerId,retryAt,false);
        }
        std::lock_guard<std::mutex> lk(m_mutex);m_gapPages.erase(peerId);
    }
    maybeRequestGap(peerId);
}

bool ClientRuntime::requestRoamConversations()
{
    std::shared_ptr<im::ClientCore> core;
    {std::lock_guard<std::mutex> lk(m_mutex);if(m_state!=State::Running)return false;core=m_core;}
    if(!m_accountAuthenticated.load()||!core||!core->isConnected())return false;
    core->sendRoamConvRq();return true;
}

bool ClientRuntime::requestRoamMessages(std::int64_t peerId,std::int64_t beforeSeq,int limit)
{
    std::shared_ptr<im::ClientCore> core;
    std::function<bool(std::int64_t,std::int64_t,int)> testHook;
    {std::lock_guard<std::mutex> lk(m_mutex);if(m_state!=State::Running)return false;
     core=m_core;testHook=m_roamRequestHookForTest;}
    if(!m_accountAuthenticated.load()||peerId<=0||beforeSeq<=0||limit<=0||limit>100)return false;
    if(testHook)return testHook(peerId,beforeSeq,limit);
    if(!core||!core->isConnected())return false;
    core->sendRoamMsgRq(static_cast<int>(peerId),beforeSeq,limit);return true;
}

void ClientRuntime::maybeRequestGap(std::int64_t conversationId)
{
    if(conversationId<=0)return;
    std::shared_ptr<im::sync::SyncService> sync;
    std::shared_ptr<im::ClientCore> core;
    bool hasTestTransport=false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if(m_state!=State::Running)return;
        sync=m_syncService;core=m_core;hasTestTransport=static_cast<bool>(m_roamRequestHookForTest);
    }
    if(!sync)return;
    if(!sync->hasGaps(conversationId)){
        std::lock_guard<std::mutex> lk(m_mutex);
        m_gapPending.erase(conversationId);m_gapInFlight.erase(conversationId);
        m_gapPages.erase(conversationId);return;
    }
    const auto gaps=sync->pendingGaps(conversationId);
    if(gaps.empty()){
        // 退避状态保存在数据库中；即使进程/Runtime 重启，也必须重新挂上唤醒器，
        // 不能只依赖下一条消息或下一次网络状态变化碰巧触发。
        const auto retryAt=sync->nextRetryAtMs(conversationId);
        if(retryAt>0)armGapTimer(conversationId,retryAt,false);
        return;
    }
    // 离线或尚未完成认证时只保留 pending。连接恢复/认证成功会再次驱动，
    // 避免把“当前不可发送”误记为补洞失败并无意义地放大退避。
    if(!m_accountAuthenticated.load()||(!hasTestTransport&&(!core||!core->isConnected()))){
        std::lock_guard<std::mutex> lk(m_mutex);
        m_gapPending.insert(conversationId);
        return;
    }
    bool exhausted=false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_gapPending.insert(conversationId);
        if(m_gapInFlight.count(conversationId))return; // 每会话 single-flight
        auto& pages=m_gapPages[conversationId];
        if(pages>=5){exhausted=true;m_gapPages.erase(conversationId);}
        else {++pages;m_gapInFlight.insert(conversationId);}
    }
    if(exhausted){
        sync->markGapFailed(conversationId);
        const auto retryAt=sync->nextRetryAtMs(conversationId);
        if(retryAt>0)armGapTimer(conversationId,retryAt,false);
        return;
    }
    const auto& gap=gaps.front();
    const auto width=gap.to-gap.from+1;
    const int limit=static_cast<int>(std::min<std::int64_t>(100,width));
    if(!requestRoamMessages(conversationId,gap.to+1,limit)){
        {std::lock_guard<std::mutex> lk(m_mutex);m_gapInFlight.erase(conversationId);}
        sync->markGapFailed(conversationId);
        const auto retryAt=sync->nextRetryAtMs(conversationId);
        if(retryAt>0)armGapTimer(conversationId,retryAt,false);
    } else {
        const auto timeout=std::max<std::int64_t>(1,m_cfg.gapRequestTimeoutMs);
        armGapTimer(conversationId,systemNowMs()+timeout,true);
    }
}

void ClientRuntime::handleChatAck(const im::ChatProtocolAck& ack)
{
    std::shared_ptr<im::message::MessageService> service;
    std::shared_ptr<im::storage::NativeRepository> repository; InflightMessage inflight;
    {std::lock_guard<std::mutex> lk(m_mutex);if(m_state!=State::Running)return;service=m_messageService;
     repository=m_repository;
     auto it=m_inflightMessages.find(ack.msgId);if(it!=m_inflightMessages.end())inflight=it->second;}
    if(!service||ack.msgId.empty()||inflight.attempt<=0)return;
    im::dto::MessageDto sent;
    if(!repository||!repository->findMessage(m_ownerId,ack.msgId,&sent)||!sent.fromMe||
       sent.peerId!=ack.peerId)return;
    const auto now=std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count(); std::string error;
    std::int64_t retryAt=0;
    if(!service->onProtocolAck(m_ownerId,ack.msgId,ack.result,ack.conversationSeq,inflight.attempt,
                               now,&error,&retryAt))return;
    if(ack.result!=im::proto::CHAT_RESULT_SERVER_ERROR){
        {std::lock_guard<std::mutex> lk(m_mutex);m_inflightMessages.erase(ack.msgId);}
        completeOperation(inflight.operationId,
            (ack.result==im::proto::CHAT_RESULT_SUCC||ack.result==im::proto::CHAT_RESULT_FAIL)?Result::Ok:Result::Failed,
            error);
    }
    else scheduleOutboxPump(retryAt);
    if(ack.conversationSeq>0){
        std::shared_ptr<im::sync::SyncService> sync;
        {std::lock_guard<std::mutex> lk(m_mutex);sync=m_syncService;}
        if(sync&&sync->observeSeq(ack.peerId,ack.conversationSeq)&&sync->hasGaps(ack.peerId)){
            std::lock_guard<std::mutex> lk(m_mutex);m_gapPending.insert(ack.peerId);
        }
        maybeRequestGap(ack.peerId);
    }
    publishInvalidation(InvalidationBus::Domain::Messages,++m_dbVersion,m_generation.load());
    publishInvalidation(InvalidationBus::Domain::Conversations,++m_dbVersion,m_generation.load());
}

std::shared_ptr<im::storage::NativeDatabase> ClientRuntime::database() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_db;
}

ClientRuntime::State ClientRuntime::state() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_state;
}

bool ClientRuntime::start(std::string* err)
{
    std::unique_lock<std::mutex> control(m_control, std::try_to_lock);
    if (!control.owns_lock()) {
        if (err) *err = "Runtime lifecycle operation in progress";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_state == State::Destroyed) {
            if (err) *err = "Runtime 已销毁，不能启动";
            return false;
        }
        if (m_state == State::Running) return true; // 幂等
        if (m_ownerId<=0 || (m_db && m_db->ownerId()!=0 && m_db->ownerId()!=m_ownerId)) {
            if (err) *err="invalid runtime/database owner";
            return false;
        }
        if (m_state == State::Starting) return true; // 重入：视为已在启动
        m_state = State::Starting;
        // 锁内创建数据库：m_db 是 shared_ptr 成员，多线程无锁读写是数据竞争，
        // 必须在锁内做创建/拷贝/move（第 2 点缺陷）。
        if (!m_db) m_db = std::make_shared<im::storage::NativeDatabase>();
    }

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_state != State::Starting) return false; // 期间被 stop/destroy：放弃
        m_state = State::Running;
        m_generation.fetch_add(1); // 每次启动进入新的 generation
    }
    startOutboxPump();
    startSyncTimer();
    return true;
}

void ClientRuntime::stop()
{
    std::lock_guard<std::mutex> control(m_control);
    stopControlled();
}

void ClientRuntime::stopControlled()
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_state == State::Destroyed || m_state == State::Stopped) return;
        if (m_state != State::Running && m_state != State::Starting) return;
        m_state = State::Stopping;
        m_accountAuthenticated.store(false);
        for (const auto& id : m_admitted) m_completions.complete(id, Result::Cancelled, "runtime stopping");
    }
    stopOutboxPump();
    stopSyncTimer();
    // stop 只停止账号服务，不关闭账号数据库。否则 start 无法在没有密钥桥的情况下
    // 重开同一个 SQLCipher 实例，形成 Running + Closed DB 的假运行态。
    m_invalidations.reset();

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_state == State::Stopping) m_state = State::Stopped;
    }
}

void ClientRuntime::logout()
{
    std::lock_guard<std::mutex> control(m_control);
    stopControlled();
    std::shared_ptr<im::storage::NativeDatabase> db;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        db = std::move(m_db);
        m_messageService.reset();m_friendService.reset();m_syncService.reset();m_repository.reset();
        m_inflightMessages.clear();
        m_gapPending.clear();m_gapInFlight.clear();m_gapPages.clear();
    }
    // logout 清除内存数据库句柄/密钥状态；下次登录必须经平台密钥桥重新打开并注入。
    if (db) db->close();
    // 递增 generation：使在途旧 generation 的完成事件失效（换账号/重登防串号）
    m_generation.fetch_add(1);
    m_invalidations.reset();
}

void ClientRuntime::destroy()
{
    // Concurrent destroy waits until the first destruction has actually drained.
    std::lock_guard<std::mutex> control(m_control);
    std::shared_ptr<im::storage::NativeDatabase> db;
    std::shared_ptr<im::ClientCore> core;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_state == State::Destroyed) return; // 幂等
        m_state = State::Destroying;
        m_accountAuthenticated.store(false);
        for (const auto& id : m_admitted) m_completions.complete(id, Result::Cancelled, "runtime destroying");
        db = std::move(m_db); // 锁内 move 出强引用，此后 m_db 为空
        m_messageService.reset();m_friendService.reset();m_syncService.reset();m_repository.reset();
        m_inflightMessages.clear();
        m_gapPending.clear();m_gapInFlight.clear();m_gapPages.clear();
        core=std::move(m_core);m_messageBridge.reset();m_friendBridge.reset();
    }

    stopOutboxPump();
    stopSyncTimer();

    if(core){core->setMessageProtocolSink({});core->setFriendProtocolSink({});}

    // 阶段一：关闭数据库（关闭顺序由 NativeDatabase 保证：停队列 → 关读池 → 关写连接）
    if (db) db->close();

    // 阶段二：拒绝新任务、清空在途登记（不 join 外部线程，避免 self-join）
    m_invalidations.reset();

    // 阶段三：置终态（m_db 已在锁内 move 出，无需再 reset）
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_state = State::Destroyed;
    }
}

ClientRuntime::OperationId ClientRuntime::nextOperationId(const char* domain)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!domain || !*domain || m_state != State::Running ||
        m_admitted.size() >= m_cfg.completionCapacity) return {};
    const std::uint64_t seq = m_opSeq.fetch_add(1) + 1;
    auto id = std::string(domain) + ":" + std::to_string(m_ownerId) + ":" +
           std::to_string(m_instanceId) + ":" + std::to_string(m_generation.load()) + ":" + std::to_string(seq);
    m_admitted.insert(id);
    return id;
}

} // namespace runtime
} // namespace im
