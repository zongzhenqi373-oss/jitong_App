package com.jitong.im.core

import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * Native 模式下的认证协调器（P5-T06）。
 *
 * "瘦 UI / 厚内核"落地：当 [KernelBackendSelector.effective] == [KernelBackend.CPP_NATIVE] 时，
 * UI 通过本类表达意图（登录/自动登录/登出/取消），**不**在 Kotlin 侧做任何 Token 有效期判断、
 * 刷新调度、断线重连或被踢处理——这些全部由 C++ 的 AccountSession 编排，UI 只订阅
 * [accountState] / [events] 做展示映射。
 *
 * 与 Legacy 路径互斥：进程内只应存在一个活动认证核心（见 [KernelBackend] 约束）。
 * 若 Native 不可用，本类的 [setup] 返回 false，调用方应回退到 Legacy MainViewModel 流程。
 */
class NativeAuthController(private val sdk: JitongSdk) {

    /** 当前账号状态（只读映射）。 */
    val accountState: StateFlow<AccountState> get() = sdk.accountState

    /** 账号事件流（登录成功/失败、刷新、被踢、登出）。 */
    val events: SharedFlow<JitongSdk.AccountEvent> get() = sdk.accountEvents

    @Volatile
    private var ready = false

    /**
     * 建立 Native 认证会话。必须在使用其它方法前成功调用。
     * @return Native 不可用或建立失败时返回 false（调用方应回退到 Legacy）。
     */
    @Synchronized
    fun setup(serverName: String, serverIp: String, port: Int): Boolean {
        if (KernelBackendSelector.effective != KernelBackend.CPP_NATIVE) return false
        if (!sdk.start(serverName)) return false
        ready = sdk.setupAccount(serverIp, port)
        if (!ready) sdk.stop()
        return ready
    }

    /**
     * 密码登录。UI 只管调用，连点由内核 Single-Flight 去重（同账号复用 operationId）。
     * @return operationId（0 表示未就绪/被拒）。
     */
    fun loginWithPassword(account: String, password: String): Long =
        if (ready) sdk.loginWithPassword(account, password) else 0L

    /** 冷启动自动登录：有未过期凭据则内核走 Token 登录，否则返回 0（UI 应展示密码登录页）。 */
    fun startWithSavedToken(account: String): Long =
        if (ready) sdk.startWithSavedToken(account) else 0L

    /** 登出。 */
    fun logout(allDevices: Boolean = false) {
        if (ready) sdk.logout(allDevices)
    }

    /** 取消进行中的认证（放弃/连点保护）。 */
    fun cancel() {
        if (ready) sdk.cancelAuthentication()
    }

    /** 释放会话（进程退出/切账号）。 */
    @Synchronized
    fun shutdown() {
        ready = false
        sdk.stop()
    }

    /** 便捷映射：当前是否已认证。 */
    val isAuthenticated: Boolean get() = accountState.value == AccountState.Authenticated
}
