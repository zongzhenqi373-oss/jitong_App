package com.jitong.im.core

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.jitong.im.net.ImClient
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class LegacyCutoverNetworkTest {
    @Test
    fun frozenClientRejectsNewConnections() = runBlocking {
        val client = ImClient()
        assertTrue(client.freezeNetworkForCutover())
        assertFalse(client.connected)
        // 冻结后应在本地拒绝，不应进入真实 TLS 连接尝试。
        assertFalse(client.connect("127.0.0.1", 1))
    }
}
