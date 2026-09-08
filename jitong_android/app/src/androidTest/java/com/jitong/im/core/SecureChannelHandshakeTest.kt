package com.jitong.im.core

import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * P4 应用层安全通道在 Android（arm64-v8a / x86_64）上的进程内握手验证。
 *
 * 覆盖执行规划 §22.3.5「arm64 进程内 TLS→握手→加密业务」中不依赖 socket 的核心：
 * 四步握手（X25519 + Ed25519 验签 + HKDF-SHA256）与加密业务帧（AES-256-GCM）
 * 双向往返，全程在 native 侧用与真实链路相同的加密原语完成，与服务端实现零漂移。
 *
 * 端到端连真实服务端（AVD 连宿主机 10.0.2.2 的 im_server）由另外的手动步骤覆盖，
 * 见 outputs/kernel-round2-android-arm64-note.md。
 *
 * 运行：
 * ```bash
 * ./gradlew :app:connectedAndroidTest \
 *   -Pandroid.testInstrumentationRunnerArguments.class=com.jitong.im.core.SecureChannelHandshakeTest
 * ```
 */
@RunWith(AndroidJUnit4::class)
class SecureChannelHandshakeTest {

    private companion object {
        const val TAG = "JitongSecureChannel"
    }

    @Before
    fun assumeNativeAvailable() {
        assumeTrue("Native .so 不可用", NativeBindings.isLoaded)
    }

    @Test
    fun secureChannelHandshake_completesOnDevice() {
        val report = NativeBindings.handshakeTestMap()
        Log.d(TAG, "handshake = $report")

        // ABI 必须是我们构建的两种之一；arm64-v8a 才真正满足门禁要求
        assertTrue("abi=${report["abi"]}", report["abi"] in listOf("arm64-v8a", "x86_64"))

        // 关键分步：验签、握手完成、双向加密往返
        assertEquals("client_hello ok", "ok", report["client_hello"])
        assertEquals("client_handle_server_hello ok", "ok", report["client_handle_server_hello"])
        assertEquals("verify_client_finished ok", "ok", report["verify_client_finished"])
        assertEquals("client_handle_server_finished ok", "ok", report["client_handle_server_finished"])
        assertEquals("client_established ok", "ok", report["client_established"])
        assertEquals("client_encrypt ok", "ok", report["client_encrypt"])
        assertEquals("server_decrypt ok", "ok", report["server_decrypt"])
        assertEquals("server_encrypt ok", "ok", report["server_encrypt"])
        assertEquals("client_decrypt ok", "ok", report["client_decrypt"])
        assertEquals("result ok", "ok", report["result"])
    }

    @Test
    fun deviceProof_p256_signAndVerifyOnDevice() {
        val report = NativeBindings.nativeDeviceProofSelfTest()
            .lineSequence()
            .mapNotNull { line ->
                val idx = line.indexOf('=')
                if (idx > 0) line.substring(0, idx) to line.substring(idx + 1) else null
            }
            .toMap()
        Log.d(TAG, "deviceProof = $report")

        assertEquals("generate ok", "ok", report["generate"])
        assertEquals("public_key ok", "ok", report["public_key"])
        assertEquals("sign ok", "ok", report["sign"])
        assertEquals("verify ok", "ok", report["verify"])
        assertEquals("result ok", "ok", report["result"])
    }
}
