package com.jitong.im

import com.jitong.im.net.Frame
import com.jitong.im.net.Protocol
import com.jitong.im.net.sha256Hex
import im.proto.Im
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.DataInputStream
import java.io.IOException

/**
 * JVM 单元测试：锁死 Android 端与 C++ 双端的线格式约定。
 * 任何一端改帧格式/哈希算法，这里都会红。
 */
class ProtocolCodecTest {

    @Test
    fun `encode 与 C++ 端字节布局一致`() {
        // 协议号 1005(CHAT_INFO_RQ)=0x3ED，payload="abc"，包长=4+3=7
        val bytes = Frame.encode(Protocol.CHAT_INFO_RQ, "abc".toByteArray())
        assertArrayEquals(
            byteArrayOf(
                0x00, 0x00, 0x00, 0x07, // 包长：大端
                0xED.toByte(), 0x03, 0x00, 0x00, // 协议号：小端
                0x61, 0x62, 0x63, // payload
            ),
            bytes,
        )
    }

    @Test
    fun `encode decode 往返一致`() {
        val payload = ByteArray(256) { it.toByte() } // 含 0 字节，验证二进制安全
        val encoded = Frame.encode(Protocol.CHAT_INFO_RQ, payload)
        val frame = Frame.readFrom(DataInputStream(ByteArrayInputStream(encoded)))
        assertEquals(Protocol.CHAT_INFO_RQ, frame?.type)
        assertArrayEquals(payload, frame?.payload)
    }

    @Test
    fun `空 payload 帧（心跳）往返一致`() {
        val encoded = Frame.encode(Protocol.HEARTBEAT_RQ, ByteArray(0))
        assertEquals(8, encoded.size)
        val frame = Frame.readFrom(DataInputStream(ByteArrayInputStream(encoded)))
        assertEquals(Protocol.HEARTBEAT_RQ, frame?.type)
        assertEquals(0, frame?.payload?.size)
    }

    @Test
    fun `流结束返回 null`() {
        assertNull(Frame.readFrom(DataInputStream(ByteArrayInputStream(ByteArray(0)))))
    }

    @Test(expected = IOException::class)
    fun `非法包长被拒绝`() {
        val bad = byteArrayOf(0x7F, 0xFF.toByte(), 0xFF.toByte(), 0xFF.toByte()) // ~2GB > MAX_PACK_LEN
        Frame.readFrom(DataInputStream(ByteArrayInputStream(bad)))
    }

    @Test
    fun `sha256Hex 与 C++ sha256Hex 输出一致`() {
        // 公开测试向量，C++ 端 protocol/sha256.h 同一实现
        assertEquals(
            "8d969eef6ecad3c29a3a629280e686cf0c3f5d5a86aff3ca12020c923adc6c92",
            sha256Hex("123456"),
        )
    }

    @Test
    fun `protobuf 线格式与 C++ 一致`() {
        // LoginRq{tel:"1", pass:"h"} 的 pb 编码：field1 tag=0x0A, field2 tag=0x12
        val bytes = Im.LoginRq.newBuilder().setTel("1").setPass("h").build().toByteArray()
        assertArrayEquals(
            byteArrayOf(0x0A, 0x01, 0x31, 0x12, 0x01, 0x68),
            bytes,
        )
        val parsed = Im.LoginRq.parseFrom(bytes)
        assertEquals("1", parsed.tel)
        assertEquals("h", parsed.pass)
    }

    @Test
    fun `ChatInfoRq 字段齐全（msg_id 幂等链路）`() {
        val rq = Im.ChatInfoRq.newBuilder()
            .setMyid(1).setFriid(2).setMsg("你好").setType(Im.MsgType.TEXT).setMsgId("uuid-1")
            .build()
        val parsed = Im.ChatInfoRq.parseFrom(rq.toByteArray())
        assertEquals(1, parsed.myid)
        assertEquals(2, parsed.friid)
        assertEquals("你好", parsed.msg)
        assertEquals(Im.MsgType.TEXT, parsed.type)
        assertEquals("uuid-1", parsed.msgId)
        assertTrue(rq.toByteArray().size < 64) // 中文 UTF-8 正常编码
    }

    @Test
    fun `AI 请求响应与取消协议可往返`() {
        val request = Im.AiReplyRq.newBuilder()
            .setRequestId("ai-uuid-1")
            .setPeerId(2)
            .setTone("自然")
            .setMaxSuggestions(3)
            .build()
        val requestFrame = Frame.readFrom(
            DataInputStream(ByteArrayInputStream(Frame.encode(Protocol.AI_REPLY_RQ, request.toByteArray())))
        )
        assertEquals(Protocol.AI_REPLY_RQ, requestFrame?.type)
        assertEquals("ai-uuid-1", Im.AiReplyRq.parseFrom(requestFrame!!.payload).requestId)

        val response = Im.AiReplyRs.newBuilder()
            .setRequestId("ai-uuid-1")
            .setStatus(Im.AiReplyStatus.AI_REPLY_OK)
            .addSuggestions("好的，晚点见")
            .build()
        assertEquals("好的，晚点见", Im.AiReplyRs.parseFrom(response.toByteArray()).suggestionsList.single())

        val cancel = Im.AiCancelRq.newBuilder().setRequestId("ai-uuid-1").build()
        assertEquals("ai-uuid-1", Im.AiCancelRq.parseFrom(cancel.toByteArray()).requestId)
        assertEquals(Protocol.DEF_BASE + 31, Protocol.AI_CANCEL_RQ)
    }

    @Test
    fun `应用层安全通道协议号和字段长度定义一致`() {
        assertEquals(Protocol.DEF_BASE + 36, Protocol.APP_CLIENT_HELLO)
        assertEquals(Protocol.DEF_BASE + 40, Protocol.APP_ENCRYPTED_FRAME)
        val hello = Im.AppClientHello.newBuilder()
            .setVersion(Protocol.APP_SECURITY_VERSION)
            .setClientEphemeralPublicKey(com.google.protobuf.ByteString.copyFrom(ByteArray(32) { 1 }))
            .setClientNonce(com.google.protobuf.ByteString.copyFrom(ByteArray(32) { 2 }))
            .setClientRandomId(com.google.protobuf.ByteString.copyFrom(ByteArray(16) { 3 }))
            .setCipherSuite(Im.AppCipherSuite.APP_CIPHER_X25519_ED25519_HKDF_SHA256_AES_256_GCM)
            .build()
        val parsed = Im.AppClientHello.parseFrom(hello.toByteArray())
        assertEquals(Protocol.APP_X25519_KEY_LEN, parsed.clientEphemeralPublicKey.size())
        assertEquals(Protocol.APP_NONCE_LEN, parsed.clientNonce.size())
        assertEquals(Protocol.APP_RANDOM_ID_LEN, parsed.clientRandomId.size())
    }
}
