package com.jitong.im.core

import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * C++ Native 内核的 Kotlin 门面（P2 首阶段：生命周期 + 自检）。
 *
 * 职责边界（V3 计划 §3.2）：
 *   - 只做句柄管理与事件投递；
 *   - **不做** Token 判断、登录重试、消息去重、数据库双写、文件重试、缩略图选择。
 *
 * 首阶段不接管任何业务：UI 仍走 Kotlin Legacy，
 * [KernelBackendSelector] 保持 [KernelBackend.KOTLIN_LEGACY]。
 */
class JitongSdk internal constructor() {

    private val sink = NativeEventSink()
    private val authPlatform = com.jitong.im.core.platform.NativeAuthPlatform()

    /** 内核上抛的事件流；订阅方自行切到 UI 线程。 */
    val events: Flow<CoreEvent> = sink.events

    @Volatile
    private var handle: Long = 0L

    /** 创建内核句柄并挂上事件桥。重复调用会先销毁旧句柄。 */
    @Synchronized
    fun start(serverName: String?): Boolean {
        if (!NativeBindings.isLoaded) return false
        if (handle != 0L) stop()

        val created = NativeBindings.nativeCreate(serverName)
        if (created == 0L) return false

        if (!NativeBindings.nativeSetEventSink(created, sink)) {
            NativeBindings.nativeDestroy(created)
            return false
        }
        handle = created
        return true
    }

    /** 销毁句柄。幂等，可重复调用。 */
    @Synchronized
    fun stop() {
        val current = handle
        handle = 0L
        accountReady = false
        _accountState.value = AccountState.LoggedOut
        if (current != 0L) NativeBindings.nativeDestroy(current)
    }

    /** Native 自检报告；.so 不可用时返回 null。 */
    fun selfTest(): Map<String, String>? =
        if (NativeBindings.isLoaded) NativeBindings.selfTestMap() else null

    /** 生命周期压力测试报告；.so 不可用时返回 null。 */
    fun lifecycleStressTest(iterations: Int = 100): Map<String, String>? {
        if (!NativeBindings.isLoaded) return null
        val text = NativeBindings.nativeLifecycleStressTest(iterations, sink)
        return text.lineSequence().mapNotNull { line ->
            val idx = line.indexOf('=')
            if (idx > 0) line.substring(0, idx) to line.substring(idx + 1) else null
        }.toMap()
    }

    /** Instrumented test 专用，不参与业务协议。 */
    internal fun emitUtf8Test(): Boolean {
        val current = handle
        return current != 0L && NativeBindings.nativeEmitUtf8Test(current)
    }

    /** 内核版本；.so 不可用时返回 null。 */
    fun version(): String? =
        if (NativeBindings.isLoaded) runCatching { NativeBindings.nativeVersion() }.getOrNull() else null

    // ---------------- P5-T06：认证会话（厚内核编排） ----------------

    /** 一条账号事件（由 C++ AccountSession 上抛）。 */
    data class AccountEvent(
        val type: AccountEventType,
        val operationId: Long,
        val accountState: AccountState,
        val connectionState: ConnectionState,
        val error: AuthError,
        val userId: Int,
        val message: String?,
    )

    private val _accountState = MutableStateFlow(AccountState.LoggedOut)
    /** 当前账号状态（UI 只读映射，不自行判断 token）。 */
    val accountState: StateFlow<AccountState> = _accountState

    private val _accountEvents = MutableSharedFlow<AccountEvent>(extraBufferCapacity = 32)
    /** 账号事件流（登录成功/失败、刷新、被踢、登出等）。 */
    val accountEvents: SharedFlow<AccountEvent> = _accountEvents

    private val accountSink = object : NativeAccountSink {
        override fun onAccountEvent(
            type: Int, operationId: Long, accountState: Int, connectionState: Int,
            error: Int, userId: Int, message: String?,
        ) {
            val ev = AccountEvent(
                type = AccountEventType.fromOrdinal(type),
                operationId = operationId,
                accountState = AccountState.fromOrdinal(accountState),
                connectionState = ConnectionState.fromOrdinal(connectionState),
                error = AuthError.fromOrdinal(error),
                userId = userId,
                message = message,
            )
            _accountState.value = ev.accountState
            _accountEvents.tryEmit(ev)
        }
    }

    @Volatile
    private var accountReady = false

    /**
     * 建立认证会话（在 [start] 之后调用）。之后所有连接/握手/设备签名/Token/刷新/重连/
     * 被踢处理都在 C++ 内核编排；UI 只调用登录/登出意图并订阅 [accountState]/[accountEvents]。
     */
    @Synchronized
    fun setupAccount(serverIp: String, port: Int): Boolean {
        val current = handle
        if (current == 0L || !NativeBindings.isLoaded) return false
        accountReady = NativeBindings.nativeAccountSetup(current, serverIp, port, accountSink, authPlatform)
        return accountReady
    }

    /** 密码登录。返回 operationId（0 表示被拒/未就绪）。 */
    fun loginWithPassword(account: String, password: String): Long {
        val current = handle
        if (current == 0L || !accountReady) return 0L
        return NativeBindings.nativeAccountLoginWithPassword(current, account, password)
    }

    /** 冷启动自动登录：有未过期凭据则 Token 登录，否则返回 0（需密码登录）。 */
    fun startWithSavedToken(account: String): Long {
        val current = handle
        if (current == 0L || !accountReady) return 0L
        return NativeBindings.nativeAccountStartWithSavedToken(current, account)
    }

    /**
     * 冷启动便捷入口：只组装 Native 认证会话并尝试恢复加密凭据。
     * 返回 0 表示本地无有效凭据，调用方应停留登录页；这里不会回退到软件密钥或明文存储。
     */
    fun restoreAccount(serverIp: String, port: Int, account: String): Long {
        if (!setupAccount(serverIp, port)) return 0L
        return startWithSavedToken(account)
    }

    /** 登出。 */
    fun logout(allDevices: Boolean = false) {
        val current = handle
        if (current != 0L && accountReady) NativeBindings.nativeAccountLogout(current, allDevices)
    }

    /** 取消进行中的认证（连点/放弃）。 */
    fun cancelAuthentication() {
        val current = handle
        if (current != 0L && accountReady) NativeBindings.nativeAccountCancel(current)
    }
}
