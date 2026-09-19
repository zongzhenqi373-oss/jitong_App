#pragma once
// Native SDK 句柄管理。
//
// 关键约束（来自执行规划 P2-T01）：
//   1. jlong 只是一个**不透明 id**，绝不编码裸指针；
//      ——Java 侧可能 double-destroy 或 use-after-destroy，裸指针会直接 UAF。
//   2. 每次 API 调用在锁内取得 shared_ptr，使用期间对象不会被销毁；
//   3. destroying 置位后拒绝新调用；
//   4. nativeDestroy 必须幂等（重复/并发调用都安全）。
//
// 线程模型：JNI API 可在任意线程被调用；销毁可能与其他线程并发。

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "client_core/ClientCore.h"
#include "client_core/AccountSession.h"
#include "client_core/storage/NativeDatabase.h"
#include "client_core/storage/NativeRepository.h"
#include "client_core/ClientCoreAuthTransport.h"
#include "client_core/DeviceProofService.h"
#include "client_core/runtime/ClientRuntime.h"
#include "client_core/media/MediaService.h"
#include "client_core/media/UploadTransport.h"

#include "jni_observer.h"
#include "jni_auth_platform.h"
#include "jni_db_key_bridge.h"

namespace jt {

struct NativeSdkHandle {
    // 保护 core / observer 的替换；短临界区，绝不在持锁时执行回调或析构
    std::mutex mutex;

    // 内核实例。用 shared_ptr：异步任务/回调可安全持有副本，
    // 销毁时只是把手柄里的副本移出释放，已在飞的任务仍持有有效对象。
    std::shared_ptr<im::ClientCore> core;

    // JNI 事件桥（持 jobject 全局引用）。销毁时先清它，确保之后不再回调 Java。
    // 用 shared_ptr：回调线程可取一份副本，销毁时只是移出手柄里的引用，
    // 已在飞的回调仍持有有效对象，不会出现 UAF。
    std::shared_ptr<JniObserver> observer;

    // 销毁中标记：置位后 acquire() 一律返回 nullptr，拒绝新 API 调用
    std::atomic<bool> destroying{false};

    // 每次媒体任务独立的协作式取消令牌；JNI 调用持 shared_ptr，移除表项后仍安全。
    std::uint64_t nextMediaOperation = 1;
    std::unordered_map<std::uint64_t, std::shared_ptr<std::atomic<bool>>> mediaOperations;

    // ---------------- P5-T06：认证会话（可选，setupAccount 后建立） ----------------
    // 声明顺序即依赖顺序；析构按逆序：session 先于 transport 先于 signer/store/clock。
    std::shared_ptr<im::account::SystemClock> clock;
    std::shared_ptr<im::account::ITokenStore> tokenStore;
    std::shared_ptr<im::account::IP256Signer> signer;
    std::shared_ptr<jt::JniAuthPlatform> authPlatform;
    std::shared_ptr<im::account::ClientCoreAuthTransport> authTransport;
    std::shared_ptr<im::account::AccountSession> account;
    // 账号事件桥（AccountEvent → Kotlin）。shared_ptr 便于析构时先清引用。
    std::shared_ptr<void> accountObserver;
    // nativeAccountSetup 时记录的服务端地址；一次性注册需要在账号会话之外复用同一连接目标。
    std::string accountServerIp;
    std::uint16_t accountServerPort = 0;

    // P7-G3：账号级运行时（唯一拥有业务服务、数据库与 completion executor）。
    // 析构顺序见 destroyHandleLocked：runtime 先于 db（runtime 依赖 db）。
    std::shared_ptr<im::runtime::ClientRuntime> runtime;

    // P6：按账号打开的 Native 数据底座（影子库）。析构顺序见 destroyHandleLocked：
    // 停队列 → 关读池 → 关写连接 → 清 key，早于 core.reset()。
    std::shared_ptr<im::storage::NativeDatabase> db;
    // 当前 db 对应的账号（0 表示未打开）；迁移/自检接口据此知道操作哪个账号。
    std::int64_t dbOwnerId = 0;

    // 分片上传编排（断点续传）：绑定同一 db + core，core/db 就绪后懒创建。
    // 析构顺序：mediaService 先于 mediaRepo/uploadTransport（service 引用二者）。
    std::shared_ptr<im::media::IUploadTransport> uploadTransport;
    std::shared_ptr<im::storage::NativeRepository> mediaRepo;
    std::shared_ptr<im::media::MediaService> mediaService;
    // 库密钥平台桥（仅 open 时经它取 key；保留 GlobalRef 供显式 deleteKey 使用）。
    std::shared_ptr<jt::JniDbKeyBridge> dbKeyBridge;

    /**
     * 在锁内取得内核实例。
     * @return 有效实例；若已开始销毁或从未创建，返回 nullptr。
     */
    std::shared_ptr<im::ClientCore> acquire()
    {
        std::lock_guard<std::mutex> lk(mutex);
        if (destroying.load(std::memory_order_acquire)) return nullptr;
        return core; // 拷贝 shared_ptr，延长生命周期到调用方作用域结束
    }

    bool isDestroying() const { return destroying.load(std::memory_order_acquire); }
};

/**
 * 创建句柄并登记到进程内句柄表。
 *
 * @param serverName   TLS SNI/证书校验目标（可为空，连接前再设置）
 * @param identityKeys base64 的 Ed25519 身份公钥（key_id → 公钥）。
 *                     **必须提供**：服务端在 TLS 之上强制应用层握手，
 *                     客户端用它验签；不注入则首次连接必然 BadKeyId 失败（fail-close）。
 * @return 不透明句柄 id；失败返回 0
 */
jlong createHandle(const std::string& serverName,
                   const std::map<std::uint32_t, std::string>& identityKeys);

/**
 * 按 id 查找句柄（线程安全）。句柄不存在或已释放时返回 nullptr。
 * 返回的 shared_ptr 保证本次调用期间句柄对象存活。
 */
std::shared_ptr<NativeSdkHandle> lookupHandle(jlong id);

/**
 * 释放句柄（幂等、并发安全）：
 *   拒绝新 API → 清空 observer（不再回调 Java）→ 停止内核 → 从表移除
 *
 * @return 是否由本次调用实际完成释放（false 表示 id 无效或已被释放）
 */
bool releaseHandle(jlong id);

/** 当前存活句柄数，供压力测试断言无泄漏。 */
std::size_t liveHandleCount();

/** 标准 base64 解码；测试配置注入与应用身份公钥加载共用。 */
bool base64Decode(const std::string& in, std::vector<unsigned char>& out);

} // namespace jt
