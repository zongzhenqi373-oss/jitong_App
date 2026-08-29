package com.jitong.im.net

import im.proto.Im
import java.io.DataInputStream
import java.io.IOException
import android.util.Log
import javax.net.ssl.SSLSocket

/**
 * TLS内部的应用层认证握手与业务帧保护。握手成功后所有业务协议必须封装为
 * AppEncryptedFrame，不允许回退为明文业务帧。
 * 任何验证失败都抛异常，由ImClient关闭连接，不允许回退到旧握手。
 */
class SecureChannel private constructor(
    val sessionId: ByteArray,
    private val clientToServerKey: ByteArray,
    private val serverToClientKey: ByteArray,
    private val clientNoncePrefix: ByteArray,
    private val serverNoncePrefix: ByteArray,
) {
    private var sendSequence = 0L
    private var receiveSequence = 0L

    @Synchronized
    fun encrypt(type: Int, payload: ByteArray): Frame {
        if (type in Protocol.APP_CLIENT_HELLO..Protocol.APP_ENCRYPTED_FRAME) {
            throw IOException("安全控制协议不能作为业务帧加密: $type")
        }
        if (sendSequence == Long.MAX_VALUE) throw IOException("发送序列号耗尽")
        val sequence = ++sendSequence
        val plaintext = le32(type) + payload
        val nonce = clientNoncePrefix + u64(sequence)
        val aad = frameAad(sessionId, sequence)
        return try {
            val result = AppCrypto.aes256GcmEncrypt(
                clientToServerKey, nonce, plaintext, aad,
            )
            val encrypted = Im.AppEncryptedFrame.newBuilder()
                .setVersion(Protocol.APP_SECURITY_VERSION)
                .setSessionId(bytes(sessionId))
                .setSequence(sequence)
                .setCiphertext(bytes(result.ciphertext))
                .setTag(bytes(result.tag))
                .build()
            Frame(Protocol.APP_ENCRYPTED_FRAME, encrypted.toByteArray())
        } finally {
            plaintext.fill(0)
            nonce.fill(0)
            aad.fill(0)
        }
    }

    @Synchronized
    fun decrypt(frame: Frame): Frame {
        if (frame.type != Protocol.APP_ENCRYPTED_FRAME) {
            throw IOException("安全通道建立后收到明文业务帧 type=${frame.type}")
        }
        val encrypted = Im.AppEncryptedFrame.parseFrom(frame.payload)
        val sequence = encrypted.sequence
        if (encrypted.version != Protocol.APP_SECURITY_VERSION ||
            !AppCrypto.constantTimeEqual(encrypted.sessionId.toByteArray(), sessionId) ||
            encrypted.tag.size() != Protocol.APP_GCM_TAG_LEN ||
            encrypted.ciphertext.size() < 4 || sequence <= 0 ||
            sequence != receiveSequence + 1
        ) throw IOException("加密帧字段、会话或序列号非法 seq=$sequence expected=${receiveSequence + 1}")

        val nonce = serverNoncePrefix + u64(sequence)
        val aad = frameAad(sessionId, sequence)
        val plaintext = try {
            AppCrypto.aes256GcmDecrypt(
                serverToClientKey,
                nonce,
                encrypted.ciphertext.toByteArray(),
                aad,
                encrypted.tag.toByteArray(),
            ) ?: throw IOException("AES-GCM认证失败")
        } finally {
            nonce.fill(0)
            aad.fill(0)
        }
        try {
            if (plaintext.size < 4) throw IOException("加密业务明文长度非法")
            val innerType = readLe32(plaintext)
            if (innerType in Protocol.APP_CLIENT_HELLO..Protocol.APP_ENCRYPTED_FRAME) {
                throw IOException("加密帧内嵌安全控制协议非法: $innerType")
            }
            receiveSequence = sequence
            return Frame(innerType, plaintext.copyOfRange(4, plaintext.size))
        } finally {
            plaintext.fill(0)
        }
    }

    fun destroy() {
        AppCrypto.secureClear(sessionId)
        AppCrypto.secureClear(clientToServerKey)
        AppCrypto.secureClear(serverToClientKey)
        AppCrypto.secureClear(clientNoncePrefix)
        AppCrypto.secureClear(serverNoncePrefix)
        sendSequence = 0
        receiveSequence = 0
    }

    companion object {
        private const val HANDSHAKE_TIMEOUT_MS = 15_000
        private const val TAG = "IM_APP_SEC"

        /** 仅供同模块协议测试构造方向对称的通道，不进入生产连接流程。 */
        internal fun forProtocolTest(
            sessionId: ByteArray,
            clientToServerKey: ByteArray,
            serverToClientKey: ByteArray,
            clientNoncePrefix: ByteArray,
            serverNoncePrefix: ByteArray,
        ): SecureChannel = SecureChannel(
            sessionId.copyOf(), clientToServerKey.copyOf(), serverToClientKey.copyOf(),
            clientNoncePrefix.copyOf(), serverNoncePrefix.copyOf(),
        )

        fun establish(socket: SSLSocket): SecureChannel {
            val startedAt = System.currentTimeMillis()
            Log.i(TAG, "开始应用层握手：初始化X25519临时密钥")
            val ephemeral = AppCrypto.generateX25519KeyPair()
            val clientNonce = AppCrypto.randomBytes(Protocol.APP_NONCE_LEN)
            val clientRandomId = AppCrypto.randomBytes(Protocol.APP_RANDOM_ID_LEN)
            var sharedSecret = ByteArray(0)
            var material = ByteArray(0)
            var clientFinishedKey = ByteArray(0)
            var serverFinishedKey = ByteArray(0)
            try {
                val clientHello = Im.AppClientHello.newBuilder()
                    .setVersion(Protocol.APP_SECURITY_VERSION)
                    .setClientEphemeralPublicKey(bytes(ephemeral.publicKey))
                    .setClientNonce(bytes(clientNonce))
                    .setClientRandomId(bytes(clientRandomId))
                    .setCipherSuite(Im.AppCipherSuite.APP_CIPHER_X25519_ED25519_HKDF_SHA256_AES_256_GCM)
                    .build()
                val clientPayload = clientHello.toByteArray()

                socket.soTimeout = HANDSHAKE_TIMEOUT_MS
                socket.getOutputStream().apply {
                    write(Frame.encode(Protocol.APP_CLIENT_HELLO, clientPayload))
                    flush()
                }
                Log.i(
                    TAG,
                    "ClientHello已发送 type=${Protocol.APP_CLIENT_HELLO} " +
                        "payloadBytes=${clientPayload.size}",
                )

                val serverFrame = Frame.readFrom(DataInputStream(socket.getInputStream()))
                    ?: throw IOException("服务端在ServerHello前关闭连接")
                Log.i(
                    TAG,
                    "收到握手帧 type=${serverFrame.type} payloadBytes=${serverFrame.payload.size}",
                )
                if (serverFrame.type != Protocol.APP_SERVER_HELLO ||
                    serverFrame.payload.size > Protocol.APP_MAX_HANDSHAKE_PAYLOAD) {
                    throw IOException("期望ServerHello，实际协议=${serverFrame.type}")
                }
                val serverHello = Im.AppServerHello.parseFrom(serverFrame.payload)
                validateServerHello(serverHello)
                val identityPublicKey = AppIdentityPins.publicKey(serverHello.keyId)
                    ?: throw IOException("未知应用身份key_id=${serverHello.keyId}")
                val serverPublic = serverHello.serverEphemeralPublicKey.toByteArray()
                val serverNonce = serverHello.serverNonce.toByteArray()
                val sessionId = serverHello.sessionId.toByteArray()
                val signatureInput = signingTranscript(
                    clientPayload,
                    serverHello.version,
                    serverPublic,
                    serverNonce,
                    sessionId,
                    serverHello.keyId,
                    serverHello.cipherSuiteValue,
                )
                if (!AppCrypto.ed25519Verify(
                        identityPublicKey,
                        signatureInput,
                        serverHello.signature.toByteArray(),
                    )
                ) throw IOException("服务端应用身份签名验证失败")

                val transcriptHash = finishedTranscriptHash(clientPayload, serverFrame.payload)
                sharedSecret = AppCrypto.x25519SharedSecret(ephemeral.privateKey, serverPublic)
                val salt = AppCrypto.sha256(clientNonce + serverNonce)
                val info = "jitong-app-channel-v1".toByteArray(Charsets.UTF_8) + sessionId + transcriptHash
                material = AppCrypto.hkdfSha256(sharedSecret, salt, info, 136)
                val c2s = material.copyOfRange(0, 32)
                val s2c = material.copyOfRange(32, 64)
                val cNonce = material.copyOfRange(64, 68)
                val sNonce = material.copyOfRange(68, 72)
                clientFinishedKey = material.copyOfRange(72, 104)
                serverFinishedKey = material.copyOfRange(104, 136)

                val clientVerify = AppCrypto.hmacSha256(clientFinishedKey, transcriptHash)
                val clientFinished = Im.AppFinished.newBuilder().setVerifyData(bytes(clientVerify)).build()
                socket.getOutputStream().apply {
                    write(Frame.encode(Protocol.APP_CLIENT_FINISHED, clientFinished.toByteArray()))
                    flush()
                }
                Log.i(TAG, "ClientFinished已发送 type=${Protocol.APP_CLIENT_FINISHED}")

                val finishedFrame = Frame.readFrom(DataInputStream(socket.getInputStream()))
                    ?: throw IOException("服务端在ServerFinished前关闭连接")
                if (finishedFrame.type != Protocol.APP_SERVER_FINISHED ||
                    finishedFrame.payload.size > Protocol.APP_MAX_HANDSHAKE_PAYLOAD) {
                    throw IOException("期望ServerFinished，实际协议=${finishedFrame.type}")
                }
                val serverFinished = Im.AppFinished.parseFrom(finishedFrame.payload)
                if (serverFinished.verifyData.size() != Protocol.APP_FINISHED_LEN) {
                    throw IOException("ServerFinished长度非法")
                }
                val expectedServerVerify = AppCrypto.hmacSha256(
                    serverFinishedKey,
                    transcriptHash + clientVerify,
                )
                if (!AppCrypto.constantTimeEqual(
                        expectedServerVerify,
                        serverFinished.verifyData.toByteArray(),
                    )
                ) throw IOException("ServerFinished校验失败")

                socket.soTimeout = 0
                Log.i(TAG, "应用层握手验证完成 latencyMs=${System.currentTimeMillis() - startedAt}")
                return SecureChannel(sessionId, c2s, s2c, cNonce, sNonce)
            } finally {
                AppCrypto.secureClear(ephemeral.privateKey)
                AppCrypto.secureClear(sharedSecret)
                AppCrypto.secureClear(material)
                AppCrypto.secureClear(clientFinishedKey)
                AppCrypto.secureClear(serverFinishedKey)
                AppCrypto.secureClear(clientNonce)
                AppCrypto.secureClear(clientRandomId)
            }
        }

        private fun validateServerHello(hello: Im.AppServerHello) {
            if (hello.version != Protocol.APP_SECURITY_VERSION ||
                hello.cipherSuite != Im.AppCipherSuite.APP_CIPHER_X25519_ED25519_HKDF_SHA256_AES_256_GCM ||
                hello.serverEphemeralPublicKey.size() != Protocol.APP_X25519_KEY_LEN ||
                hello.serverNonce.size() != Protocol.APP_NONCE_LEN ||
                hello.sessionId.size() != Protocol.APP_SESSION_ID_LEN ||
                hello.signature.size() != Protocol.APP_ED25519_SIGNATURE_LEN ||
                hello.keyId == 0
            ) throw IOException("ServerHello字段非法")
        }

        private fun signingTranscript(
            clientPayload: ByteArray,
            version: Int,
            serverPublic: ByteArray,
            serverNonce: ByteArray,
            sessionId: ByteArray,
            keyId: Int,
            cipherSuite: Int,
        ): ByteArray =
            "jitong-app-handshake-v1".toByteArray(Charsets.UTF_8) +
                u32(clientPayload.size) + clientPayload + u32(version) + serverPublic +
                serverNonce + sessionId + u32(keyId) + u32(cipherSuite)

        private fun finishedTranscriptHash(clientPayload: ByteArray, serverPayload: ByteArray): ByteArray =
            AppCrypto.sha256(u32(clientPayload.size) + clientPayload + u32(serverPayload.size) + serverPayload)

        private fun u32(value: Int): ByteArray = byteArrayOf(
            (value ushr 24).toByte(),
            (value ushr 16).toByte(),
            (value ushr 8).toByte(),
            value.toByte(),
        )

        private fun u64(value: Long): ByteArray = ByteArray(8) { index ->
            (value ushr (56 - index * 8)).toByte()
        }

        private fun le32(value: Int): ByteArray = byteArrayOf(
            value.toByte(),
            (value ushr 8).toByte(),
            (value ushr 16).toByte(),
            (value ushr 24).toByte(),
        )

        private fun readLe32(value: ByteArray): Int =
            (value[0].toInt() and 0xff) or
                ((value[1].toInt() and 0xff) shl 8) or
                ((value[2].toInt() and 0xff) shl 16) or
                ((value[3].toInt() and 0xff) shl 24)

        private fun frameAad(sessionId: ByteArray, sequence: Long): ByteArray =
            "jitong-app-frame-v1".toByteArray(Charsets.UTF_8) +
                u32(Protocol.APP_SECURITY_VERSION) + sessionId + u64(sequence)

        private fun bytes(value: ByteArray) = com.google.protobuf.ByteString.copyFrom(value)
    }
}
