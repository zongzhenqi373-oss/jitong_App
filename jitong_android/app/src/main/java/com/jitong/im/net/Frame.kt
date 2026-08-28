package com.jitong.im.net

import java.io.DataInputStream
import java.io.EOFException
import java.io.IOException

/**
 * 二段式帧编解码，与 C++ 双端（client_core TcpTransport / im_server Session）逐字节一致：
 *   [4B 大端包长 = 4 + payload 长度][4B 小端协议号][pb payload]
 */
data class Frame(val type: Int, val payload: ByteArray) {

    companion object {
        fun encode(type: Int, payload: ByteArray): ByteArray {
            val bodyLen = 4 + payload.size
            val out = ByteArray(4 + bodyLen)
            // 包长：大端（对齐 C++ encodeLen32）
            out[0] = (bodyLen ushr 24).toByte()
            out[1] = (bodyLen ushr 16).toByte()
            out[2] = (bodyLen ushr 8).toByte()
            out[3] = bodyLen.toByte()
            // 协议号：小端（对齐 C++ encodeType32）
            out[4] = type.toByte()
            out[5] = (type ushr 8).toByte()
            out[6] = (type ushr 16).toByte()
            out[7] = (type ushr 24).toByte()
            System.arraycopy(payload, 0, out, 8, payload.size)
            return out
        }

        /** 从流中同步读一帧；对端正常关闭返回 null，协议违规抛 IOException */
        fun readFrom(input: DataInputStream): Frame? {
            val len = try {
                input.readInt() // DataInputStream 按大端读
            } catch (e: EOFException) {
                return null
            }
            if (len < 4 || len > Protocol.MAX_PACK_LEN) {
                throw IOException("非法包长: $len")
            }
            val body = ByteArray(len)
            input.readFully(body)
            // 协议号：小端（对齐 C++ decodeType32）
            val type = (body[0].toInt() and 0xFF) or
                ((body[1].toInt() and 0xFF) shl 8) or
                ((body[2].toInt() and 0xFF) shl 16) or
                ((body[3].toInt() and 0xFF) shl 24)
            return Frame(type, body.copyOfRange(4, body.size))
        }
    }

    override fun equals(other: Any?): Boolean =
        other is Frame && other.type == type && other.payload.contentEquals(payload)

    override fun hashCode(): Int = 31 * type + payload.contentHashCode()
}
