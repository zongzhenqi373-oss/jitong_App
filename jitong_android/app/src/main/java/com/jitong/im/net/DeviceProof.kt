package com.jitong.im.net

import java.io.ByteArrayOutputStream

/** 与服务端 DeviceProof.cpp 一致的长度前缀规范编码。 */
object DeviceProof {
    fun message(
        operation: String,
        appSessionId: ByteArray,
        deviceId: String,
        credentialBinding: ByteArray,
        publicKey: ByteArray = ByteArray(0),
    ): ByteArray {
        val output = ByteArrayOutputStream()
        output.write("jitong-device-proof-v1".toByteArray(Charsets.UTF_8))
        listOf(
            operation.toByteArray(Charsets.UTF_8), appSessionId,
            deviceId.toByteArray(Charsets.UTF_8), credentialBinding, publicKey,
        ).forEach { field ->
            output.write(byteArrayOf(
                (field.size ushr 24).toByte(), (field.size ushr 16).toByte(),
                (field.size ushr 8).toByte(), field.size.toByte(),
            ))
            output.write(field)
        }
        return output.toByteArray()
    }
}
