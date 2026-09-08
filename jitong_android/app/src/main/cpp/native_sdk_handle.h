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
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "client_core/ClientCore.h"

#include "jni_observer.h"

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
 * @param serverName TLS SNI/证书校验目标（可为空，连接前再设置）
 * @return 不透明句柄 id；失败返回 0
 */
jlong createHandle(const std::string& serverName);

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

} // namespace jt
