package com.jitong.im.core

import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.IOException
import java.net.InetSocketAddress
import java.net.Socket

/**
 * 端到端网络可达性验证（Part 2 的可自动化部分）。
 *
 * 从 App 进程（真实 UID / 网络沙箱）经 `10.0.2.2` 连到宿主机上运行的 im_server
 * TCP 端口，证明「AVD → 宿主机 im_server」这条链路在应用侧是通的。
 *
 * 说明：完整的 native 登录 e2e（TLS→四步握手→设备证明登录→收发消息）依赖尚未实现的
 * P5 JNI connect/login 桥与配置注入，因此此处只覆盖到 TCP 可达性；握手与加密逻辑本身
 * 已由 [SecureChannelHandshakeTest] 在 arm64 进程内完整验证。
 *
 * 前置：宿主机已启动 im_server 监听 24680（见 outputs/kernel-round2-android-arm64-note.md）。
 * 未启动时本用例自动跳过（Assume），不会误报失败。
 *
 * 运行：
 * ```bash
 * ./gradlew :app:connectedDebugAndroidTest \
 *   -Pandroid.testInstrumentationRunnerArguments.class=com.jitong.im.core.HostServerReachabilityTest
 * ```
 */
@RunWith(AndroidJUnit4::class)
class HostServerReachabilityTest {

    private companion object {
        const val TAG = "JitongHostReach"
        const val HOST = "10.0.2.2" // AVD 内即宿主机 127.0.0.1
        const val PORT = 24680
        const val CONNECT_TIMEOUT_MS = 3000
    }

    @Test
    fun appProcess_canReachHostImServer() {
        val reachable = tryConnect()
        // 服务端未启动时跳过而不是失败：这是需要外部进程配合的手动 e2e 步骤
        assumeTrue("宿主机 im_server 未监听 $HOST:$PORT（请先在宿主机启动）", reachable)
        Log.d(TAG, "reachable $HOST:$PORT = true")
        assertTrue(reachable)
    }

    private fun tryConnect(): Boolean = try {
        Socket().use { socket ->
            socket.connect(InetSocketAddress(HOST, PORT), CONNECT_TIMEOUT_MS)
            socket.isConnected
        }
    } catch (e: IOException) {
        Log.d(TAG, "connect failed: ${e.message}")
        false
    }
}
