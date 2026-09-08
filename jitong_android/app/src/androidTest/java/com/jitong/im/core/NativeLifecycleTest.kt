package com.jitong.im.core

import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Native 内核生命周期与自检测试（P2-T04 / P1-T03）。
 *
 * 必须在真实 ART 上运行：JNI、Keystore、MMKV 都无法在普通 JVM 单测中工作。
 *
 * 运行：
 * ```bash
 * ./gradlew :app:connectedAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=\
 *   com.jitong.im.core.NativeLifecycleTest
 * ```
 *
 * 建议同时开启 CheckJNI：
 * ```bash
 * adb shell setprop debug.checkjni 1   # 或 debug.jni.logging / 强 Marshmallow+ 用 wrap.sh
 * ```
 */
@RunWith(AndroidJUnit4::class)
class NativeLifecycleTest {

    private companion object {
        const val TAG = "JitongKernelTest"
    }

    @Before
    fun assumeNativeAvailable() {
        // .so 缺失时跳过而不是失败：Native 是可选后端
        assumeTrue("Native .so 不可用", NativeBindings.isLoaded)
    }

    @Test
    fun nativeVersion_isNotEmpty() {
        assertTrue(NativeBindings.nativeVersion().isNotBlank())
    }

    @Test
    fun selfTest_reportsOk() {
        val report = NativeBindings.selfTestMap()

        // 输出实际自检结果，便于人工核对版本与 ABI
        Log.d(TAG, "selfTest = $report")

        assertEquals("ok", report["frame_codec"])
        assertEquals("ok", report["endianness"])
        assertEquals("ok", report["result"])
        assertNotNull(report["protobuf"])
        assertNotNull(report["openssl"])
        // abi 必须是我们构建的两种之一
        assertTrue(report["abi"] in listOf("arm64-v8a", "x86_64"))
    }

    @Test
    fun sdk_startAndStop_leavesNoHandle() {
        val sdk = JitongSdk()
        assertTrue(sdk.start("im.example.com"))
        sdk.stop()
        assertEquals(0, NativeBindings.nativeHandleCount())
    }

    @Test
    fun doubleDestroy_isIdempotent() {
        val handle = NativeBindings.nativeCreate("im.example.com")
        assertTrue(handle != 0L)
        NativeBindings.nativeDestroy(handle)
        // 重复销毁不得崩溃，也不得重复释放
        NativeBindings.nativeDestroy(handle)
        assertEquals(0, NativeBindings.nativeHandleCount())
    }

    @Test
    fun staleHandle_isRejected() {
        val handle = NativeBindings.nativeCreate("im.example.com")
        NativeBindings.nativeDestroy(handle)
        // 销毁后挂 sink 必须失败而不是崩溃
        assertEquals(false, NativeBindings.nativeSetEventSink(handle, NativeEventSink()))
    }

    @Test
    fun lifecycleStressTest_100Rounds_passes() {
        val sink = NativeEventSink()
        val report = NativeBindings.nativeLifecycleStressTest(100, sink)
            .lineSequence()
            .mapNotNull { line ->
                val idx = line.indexOf('=')
                if (idx > 0) line.substring(0, idx) to line.substring(idx + 1) else null
            }
            .toMap()

        Log.d(TAG, "stress = $report")

        assertEquals("100", report["rounds"])
        // 净增句柄必须为 0（允许调用方自己持有句柄，故断言增量而非绝对值）
        assertEquals("0", report["leaked"])
        assertEquals("0", report["failures"])
        assertEquals("ok", report["result"])
        // 并发回调确实跑起来了（说明 Attach/Detach 路径被覆盖）
        assertTrue((report["callbacks"]?.toIntOrNull() ?: 0) > 0)
    }

    @Test
    fun eventSink_receivesCallbacksFromNativeThreads() {
        val sdk = JitongSdk()
        assertTrue(sdk.start("im.example.com"))
        try {
            // 压力测试内部会从 4 个 native 线程触发 onLoginResult/onConnectionClosed
            val report = NativeBindings.nativeLifecycleStressTest(10, NativeEventSink())
            assertTrue(report.contains("result=ok"))
        } finally {
            sdk.stop()
        }
    }

    @Test
    fun instrumentationContext_isAvailable() {
        assertNotNull(InstrumentationRegistry.getInstrumentation().targetContext)
    }
}
