// 账号级运行时（P7-G3）。
//
// 当前提供账号生命周期、数据库共享所有权、generation 与有界命令结果。
// Runtime-owned executor 和完整业务服务装配仍由后续生产接线实现。
//
// 契约（v2 §3 P7-G3）：
//   - `NativeSdkHandle` 唯一拥有账号级 `ClientRuntime`；同账号只有一个 active Runtime、
//     一个 Socket、一个 Writer；
//   - destroy 顺序固定：拒绝新任务 → 终结命令 → close/drain → 释放 JNI global ref；
//     **禁止在自身线程 join**（否则 self-join 死锁）；
//   - 事件携带 runtimeGeneration、ownerId、dbVersion/operationId；旧 generation 事件丢弃；
//   - start/stop/logout/destroy 可重入、可并发，无 double completion、self-join、UAF
//     或悬挂请求；
//   - 所有命令返回 operationId，底层异步完成（本层只负责登记终态，不执行业务）。

#ifndef CLIENT_CORE_RUNTIME_CLIENT_RUNTIME_H
#define CLIENT_CORE_RUNTIME_CLIENT_RUNTIME_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <thread>
#include <vector>

#include "client_core/runtime/CompletionRegistry.h"
#include "client_core/runtime/InvalidationBus.h"
#include "client_core/storage/NativeDatabase.h"
#include "client_core/message/MessageService.h"
#include "client_core/friend/FriendService.h"
#include "client_core/sync/SyncService.h"
#include "client_core/ClientCore.h"
#include "client_core/testing/DeterministicClock.h"

namespace im {
namespace runtime {

class IRuntimeEventSink {
public:
    virtual ~IRuntimeEventSink() = default;
    virtual void onInvalidated(InvalidationBus::Domain domain, std::int64_t dbVersion,
                               std::int64_t generation, std::int64_t ownerId) = 0;
    virtual void onOperationCompleted(const CompletionRegistry::Record& record,
                                      std::int64_t generation, std::int64_t ownerId) = 0;
};

class ClientRuntime : public std::enable_shared_from_this<ClientRuntime> {
public:
    struct Config {
        std::int64_t ownerId = 0;
        std::string filesDir;
        std::size_t completionCapacity = 256;
        /** 漫游请求无响应的 deadline；生产默认5秒，测试可缩短但必须为正数。 */
        std::int64_t gapRequestTimeoutMs = 5000;
    };

    enum class State {
        Idle = 0,
        Starting = 1,
        Running = 2,
        Stopping = 3,
        Stopped = 4,
        Destroyed = 5,
        Destroying = 6,
    };

    explicit ClientRuntime(const Config& cfg, im::testing::IClock* clock = nullptr);
    ~ClientRuntime();

    ClientRuntime(const ClientRuntime&) = delete;
    ClientRuntime& operator=(const ClientRuntime&) = delete;

    // ---- 生命周期（可重入、可并发） ----
    /** 启动；重复调用幂等（已 Running 返回 true）。 */
    bool start(std::string* err = nullptr);
    /** 停止服务但保留 Runtime 与已打开数据库（可再次 start）。 */
    void stop();
    /** 登出：停止、关闭数据库并递增 generation，使在途旧 generation 事件失效。 */
    void logout();
    /** 销毁：释放资源；可重复调用；禁止自身线程 join。 */
    void destroy();

    // ---- 状态 ----
    State state() const;
    std::int64_t generation() const { return m_generation.load(); }
    std::int64_t ownerId() const { return m_ownerId; }
    bool isActive() const { return state() == State::Running; }

    const char* stateName() const;
    static const char* stateName(State s);

    // ---- 事件契约 ----
    /** 事件是否属于当前 generation（旧 generation 必须丢弃）。 */
    bool isCurrentGeneration(std::int64_t gen) const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return gen == m_generation.load() && m_state == State::Running;
    }

    /**
     * 发布一次失效通知（dbVersion 必须单调）。
     * 网络 IO 与 DB Writer 永不等待 Kotlin collector：这里只做记账。
     */
    bool publishInvalidation(InvalidationBus::Domain d, std::int64_t dbVersion,
                             std::int64_t sourceGeneration);
    std::int64_t consumeInvalidation(InvalidationBus::Domain d)
    {
        return m_invalidations.consume(d);
    }

    // ---- 操作完成登记（有界、exactly-once） ----
    using Result = CompletionRegistry::Result;
    using OperationId = std::string;

    /**
     * 预留一个 operationId（实例 + generation + 单调序号）；未启动或容量满返回空。
     * 调用方必须先取得非空 ID 再产生业务副作用。
     */
    OperationId nextOperationId(const char* domain);

    /** 登记终态；重复终结被拒（exactly-once）。 */
    bool completeOperation(const OperationId& id, Result r, const std::string& error = {});
    /** 绑定平台事件桥；回调只作通知，业务快照仍由 SDK 重新查询。 */
    void setRuntimeEventSink(std::shared_ptr<IRuntimeEventSink> sink);
    bool consumeOperation(const OperationId& id, CompletionRegistry::Record* out = nullptr)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_completions.consume(id, out)) return false;
        m_admitted.erase(id);
        return true;
    }
    std::size_t pendingOperations() const { return m_completions.size(); }

    // ---- 数据库 ----
    // Runtime 与 NativeSdkHandle 共享所有权（NativeDatabase::close 幂等，
    // 因此 runtime.destroy() 与 handle 的 db.reset() 谁先谁后都安全）。
    bool setDatabase(std::shared_ptr<im::storage::NativeDatabase> db);
    std::shared_ptr<im::storage::NativeDatabase> database() const;
    bool databaseReady() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_state == State::Running && m_db && m_db->status() == im::storage::DbStatus::Ready;
    }

    struct MessageCommand {
        OperationId operationId;
        std::string msgId;
        std::int64_t localOrder=0;
        bool accepted=false;
        std::string error;
    };
    /** 绑定 NativeSdkHandle 中既有 ClientCore，不创建第二条连接。 */
    bool attachClientCore(const std::shared_ptr<im::ClientCore>& core);
    MessageCommand sendText(std::int64_t conversationId, std::int64_t peerId,
                            const std::string& text, const std::string& pinyin = {},
                            const std::string& initials = {});
    /** 媒体字节已上传后提交卡片；与文本共用 Native Outbox/ACK 幂等链路。 */
    MessageCommand sendMedia(const im::dto::MessageDto& intent);
    OperationId markRead(std::int64_t conversationId,std::int64_t readSeq);
    /** 领取持久 Outbox 并经既有安全连接发送；返回实际入队条数。 */
    int flushOutbox(std::int64_t nowSeconds, int limit = 32);
    /** 认证编排层告知账号是否具备发送业务消息的资格；false 会立即关闭发送门禁。 */
    void setAccountAuthenticated(bool authenticated);
    // 仅供弱引用协议桥调用；仍会校验 state/generation/owner。
    void handleIncomingChat(const im::ChatProtocolMessage& message);
    void handleChatAck(const im::ChatProtocolAck& ack);
    void handleRoamConversations(const std::vector<im::ChatProtocolMessage>& messages);
    void handleRoamMessages(std::int64_t peerId,
                            const std::vector<im::ChatProtocolMessage>& messages,
                            bool hasMore,std::int64_t minSeq);
    bool requestRoamConversations();
    bool requestRoamMessages(std::int64_t peerId,std::int64_t beforeSeq,int limit);
    bool loadFriends(std::vector<im::dto::FriendDto>* out,std::string* err=nullptr) const;
    bool loadFriendRequests(std::vector<im::dto::FriendRequestDto>* out,
                            std::string* err=nullptr) const;
    bool requestFriendRequests();
    bool sendAddFriendRequest(const std::string& nick);
    bool answerFriendRequest(std::int64_t requesterId,const std::string& requesterNick,bool agree);
    bool deleteFriend(std::int64_t friendId);
    void handleFriendInfo(const im::FriendProtocolInfo& info);
    void handleFriendRequest(const im::FriendProtocolRequest& request);
    void handleFriendRequestList(const std::vector<im::FriendProtocolRequest>& requests);
    void handleFriendOffline(std::int64_t friendId);
    void handleDeleteFriendResult(int result,std::int64_t friendId);
#ifdef CLIENT_CORE_TEST_HOOKS
    /** 仅测试：替代真实 Socket 漫游发送，模拟接受请求但永不返回。 */
    void setRoamRequestHookForTest(
        std::function<bool(std::int64_t,std::int64_t,int)> hook)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_roamRequestHookForTest=std::move(hook);
    }
#endif
private:
    void stopControlled();
    void startOutboxPump();
    void stopOutboxPump();
    void wakeOutboxPump();
    void scheduleOutboxPump(std::int64_t epochSeconds);
    void runOutboxPump();
    bool storeProtocolMessage(const im::ChatProtocolMessage& message,bool roaming);
    void maybeRequestGap(std::int64_t conversationId);
    void startSyncTimer();
    void stopSyncTimer();
    void armGapTimer(std::int64_t conversationId,std::int64_t atMs,bool requestTimeout);
    void cancelGapTimer(std::int64_t conversationId);
    void runSyncTimer();

    Config m_cfg;
    std::int64_t m_ownerId = 0;
    im::testing::IClock* m_clock;

    mutable std::mutex m_mutex;
    // Serializes lifecycle operations, never held by DB workers or observers.
    mutable std::mutex m_control;
    State m_state = State::Idle;
    std::atomic<std::int64_t> m_generation{0};
    std::atomic<std::uint64_t> m_opSeq{0};
    const std::uint64_t m_instanceId;
    std::unordered_set<OperationId> m_admitted;

    std::shared_ptr<im::storage::NativeDatabase> m_db;
    std::shared_ptr<im::storage::NativeRepository> m_repository;
    std::shared_ptr<im::message::MessageService> m_messageService;
    std::shared_ptr<im::friend_service::FriendService> m_friendService;
    std::shared_ptr<im::sync::SyncService> m_syncService;
    std::shared_ptr<im::ClientCore> m_core;
    std::shared_ptr<IRuntimeEventSink> m_runtimeEventSink;
    std::function<bool(std::int64_t,std::int64_t,int)> m_roamRequestHookForTest;
    std::shared_ptr<im::IMessageProtocolSink> m_messageBridge;
    std::shared_ptr<im::IFriendProtocolSink> m_friendBridge;
    struct InflightMessage { std::int64_t attempt=0; OperationId operationId; };
    std::unordered_map<std::string,InflightMessage> m_inflightMessages;
    std::unordered_set<std::int64_t> m_gapPending;
    std::unordered_set<std::int64_t> m_gapInFlight;
    std::unordered_map<std::int64_t,int> m_gapPages;
    struct GapTimer { std::int64_t atMs=0; bool requestTimeout=false; };
    std::mutex m_syncTimerMutex;
    std::condition_variable m_syncTimerCv;
    std::thread m_syncTimerThread;
    std::unordered_map<std::int64_t,GapTimer> m_gapTimers;
    bool m_syncTimerStop=true;
    std::atomic<std::int64_t> m_dbVersion{0};
    std::mutex m_outboxMutex;
    std::condition_variable m_outboxCv;
    std::thread m_outboxThread;
    bool m_outboxStop = true;
    bool m_outboxWake = false;
    std::int64_t m_outboxWakeAt = 0;
    std::atomic<bool> m_accountAuthenticated{false};
    CompletionRegistry m_completions;
    InvalidationBus m_invalidations;
};

} // namespace runtime
} // namespace im

#endif // CLIENT_CORE_RUNTIME_CLIENT_RUNTIME_H
