package com.jitong.im.core

import android.util.Base64
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/** P4 F05：arm64 App 进程到真实 im_server 的加密 Heartbeat 端到端门禁。 */
@RunWith(AndroidJUnit4::class)
class NativeSocketHeartbeatTest {
    private companion object {
        const val TAG = "JitongRound2E2E"
    }

    @Before
    fun assumeNativeAvailable() {
        assumeTrue("Native .so 不可用", NativeBindings.isLoaded)
    }

    @Test
    fun nativeClient_realSocketEncryptedHeartbeatRoundTrip() {
        val args = InstrumentationRegistry.getArguments()
        val host = args.getString("jitongHost") ?: "10.0.2.2"
        val port = args.getString("jitongPort")?.toIntOrNull() ?: 24680
        val serverName = requireNotNull(args.getString("jitongServerName"))
        val caBase64 = requireNotNull(args.getString("jitongCaBase64"))
        val identityPublicKey = requireNotNull(args.getString("jitongIdentityPublicKey"))
        val spkiPin = requireNotNull(args.getString("jitongSpkiPin"))
        val marker = requireNotNull(args.getString("jitongMarker"))

        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val caFile = File(context.cacheDir, "round2-test-ca.pem")
        caFile.writeBytes(Base64.decode(caBase64, Base64.DEFAULT))

        val report = try {
            NativeBindings.nativeSocketHeartbeatTest(
                host, port, serverName, caFile.absolutePath, identityPublicKey, spkiPin, marker,
            ).lineSequence().mapNotNull { line ->
                val index = line.indexOf('=')
                if (index > 0) line.substring(0, index) to line.substring(index + 1) else null
            }.toMap()
        } finally {
            caFile.delete()
        }
        Log.i(TAG, "native socket heartbeat report=$report")

        Log.d("JitongSocketE2E", "report=$report")
        assertTrue("必须在 arm64-v8a 运行，actual=${report["abi"]}", report["abi"] == "arm64-v8a")
        assertEquals("ok", report["config"])
        assertEquals("ok", report["tls_and_app_handshake"])
        assertEquals("ok", report["marker_probe_enqueued"])
        assertEquals("ok", report["encrypted_heartbeat_roundtrip"])
        assertEquals("ok", report["result"])
    }
}
