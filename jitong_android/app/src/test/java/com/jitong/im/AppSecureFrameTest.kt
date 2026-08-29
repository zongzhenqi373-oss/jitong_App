package com.jitong.im

import com.jitong.im.net.Frame
import com.jitong.im.net.Protocol
import com.jitong.im.net.SecureChannel
import java.io.IOException
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class AppSecureFrameTest {
    private val sessionId = ByteArray(Protocol.APP_SESSION_ID_LEN) { (it + 1).toByte() }
    private val clientKey = ByteArray(32) { (it + 11).toByte() }
    private val serverKey = ByteArray(32) { (it + 51).toByte() }
    private val clientPrefix = byteArrayOf(1, 2, 3, 4)
    private val serverPrefix = byteArrayOf(5, 6, 7, 8)

    private fun client() = SecureChannel.forProtocolTest(
        sessionId, clientKey, serverKey, clientPrefix, serverPrefix,
    )

    // 用方向互换的通道模拟服务端收发。
    private fun server() = SecureChannel.forProtocolTest(
        sessionId, serverKey, clientKey, serverPrefix, clientPrefix,
    )

    @Test
    fun `业务协议双向加密解密且外层不暴露真实协议号`() {
        val client = client()
        val server = server()
        val request = "secret-login-payload".toByteArray()
        val outer = client.encrypt(Protocol.LOGIN_RQ, request)

        assertEquals(Protocol.APP_ENCRYPTED_FRAME, outer.type)
        val decoded = server.decrypt(outer)
        assertEquals(Protocol.LOGIN_RQ, decoded.type)
        assertArrayEquals(request, decoded.payload)

        val response = server.encrypt(Protocol.LOGIN_RS, byteArrayOf(7, 8, 9))
        val decodedResponse = client.decrypt(response)
        assertEquals(Protocol.LOGIN_RS, decodedResponse.type)
        assertArrayEquals(byteArrayOf(7, 8, 9), decodedResponse.payload)
    }

    @Test
    fun `篡改密文认证失败`() {
        val outer = client().encrypt(Protocol.CHAT_INFO_RQ, "hello".toByteArray())
        val corrupted = outer.payload.copyOf().also { it[it.lastIndex - 2] = (it[it.lastIndex - 2].toInt() xor 1).toByte() }
        assertThrows(IOException::class.java) { server().decrypt(Frame(outer.type, corrupted)) }
    }

    @Test
    fun `同一加密帧只能接收一次`() {
        val sender = client()
        val receiver = server()
        val outer = sender.encrypt(Protocol.HEARTBEAT_RQ, ByteArray(0))
        receiver.decrypt(outer)
        assertThrows(IOException::class.java) { receiver.decrypt(outer) }
    }

    @Test
    fun `安全通道拒绝明文业务协议`() {
        assertThrows(IOException::class.java) {
            client().decrypt(Frame(Protocol.LOGIN_RS, byteArrayOf(1)))
        }
    }
}
