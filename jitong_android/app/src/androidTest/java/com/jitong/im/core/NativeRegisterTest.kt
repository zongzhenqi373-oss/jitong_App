package com.jitong.im.core

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.jitong.im.core.platform.NativeAuthPlatform
import kotlinx.coroutines.flow.filterIsInstance
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * 一次性注册（nativeAccountRegister）真机链路：
 * - 校验本地拒绝路径（未 setup / 参数非法）。
 * - 无服务端可达时，连接失败必须经 NativeEventSink 上抛 RegisterResult(0)，
 *   证明「JNI 后台线程 → connect 失败 → 事件上抛」全链路可用。
 * 真实注册往返（result=1/2/3）依赖可达服务端，属于真实服务端 E2E 范围。
 */
@RunWith(AndroidJUnit4::class)
class NativeRegisterTest {
    private var handle: Long = 0L
    private lateinit var sink: NativeEventSink

    @Before
    fun setUp() {
        assumeTrue("Native .so 不可用", NativeBindings.isLoaded)
        sink = NativeEventSink()
        handle = NativeBindings.nativeCreate("im.example.com")
        assertTrue("创建内核句柄失败", handle != 0L)
        assertTrue(NativeBindings.nativeSetEventSink(handle, sink))
    }

    @After
    fun tearDown() {
        if (handle != 0L) {
            NativeBindings.nativeDestroy(handle)
            handle = 0L
        }
    }

    @Test
    fun registerRejectsInvalidInput() {
        assertFalse("空参数必须本地拒绝",
            NativeBindings.nativeAccountRegister(handle, "", "", ""))
    }

    @Test
    fun registerReportsLocalFailureWhenServerUnreachable() = runBlocking {
        val accountSink = object : NativeAccountSink {
            override fun onAccountEvent(
                type: Int, operationId: Long, accountState: Int, connectionState: Int,
                error: Int, userId: Int, message: String?,
            ) = Unit
        }
        assertTrue("accountSetup 失败", NativeBindings.nativeAccountSetup(
            handle, "10.0.2.2", 29999, accountSink, NativeAuthPlatform()))
        assertTrue("注册应被受理（异步执行）",
            NativeBindings.nativeAccountRegister(handle, "regtest", "13977778888", "pass1234"))
        // 29999 端口无服务端：连接失败 → result=0（本地失败，非协议码）
        val result = withTimeout(30_000) {
            sink.events.filterIsInstance<CoreEvent.RegisterResult>().first()
        }
        assertEquals(0, result.result)
    }
}
