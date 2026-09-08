package com.jitong.im.core

import kotlinx.coroutines.flow.Flow

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
}
