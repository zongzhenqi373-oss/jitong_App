package com.jitong.im.net

import java.security.KeyFactory
import java.security.KeyPairGenerator
import java.security.MessageDigest
import java.security.NoSuchAlgorithmException
import java.security.SecureRandom
import java.security.Signature
import java.security.spec.PKCS8EncodedKeySpec
import java.security.spec.X509EncodedKeySpec
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec
import org.bouncycastle.crypto.params.Ed25519PublicKeyParameters
import org.bouncycastle.crypto.signers.Ed25519Signer

/** 标准密码学原语封装；阶段2只供测试，尚未接入网络收发。 */
object AppCrypto {
    private const val X25519_KEY_SIZE = 32
    private const val ED25519_KEY_SIZE = 32
    private const val ED25519_SIGNATURE_SIZE = 64
    private const val SHA256_SIZE = 32
    private const val AES256_KEY_SIZE = 32
    private const val GCM_NONCE_SIZE = 12
    private const val GCM_TAG_SIZE = 16

    // RFC 8410 SubjectPublicKeyInfo / PKCS#8前缀，后接32字节原始密钥。
    private val x25519PublicPrefix = hex("302a300506032b656e032100")
    private val x25519PrivatePrefix = hex("302e020100300506032b656e04220420")
    private val ed25519PublicPrefix = hex("302a300506032b6570032100")
    private val ed25519PrivatePrefix = hex("302e020100300506032b657004220420")

    data class X25519KeyPair(val privateKey: ByteArray, val publicKey: ByteArray)
    data class Ed25519KeyPair(val privateKey: ByteArray, val publicKey: ByteArray)
    data class AesGcmResult(val ciphertext: ByteArray, val tag: ByteArray)

    fun randomBytes(size: Int): ByteArray {
        require(size >= 0) { "随机数长度不能为负数" }
        return ByteArray(size).also(SecureRandom()::nextBytes)
    }

    fun generateX25519KeyPair(): X25519KeyPair {
        // 不同 Android/Conscrypt 版本对 RFC 7748 使用不同的 JCA 名称：
        // 有的注册为 X25519，有的只注册通用名称 XDH。XDH 在这里显式指定
        // NamedParameterSpec("X25519")，因此不是切换曲线或安全降级。
        val generator = try {
            KeyPairGenerator.getInstance("X25519")
        } catch (_: NoSuchAlgorithmException) {
            // Android Conscrypt 的 OpenSSLXDHKeyPairGenerator 固定实现 X25519，
            // 且明确拒绝 AlgorithmParameterSpec；直接生成即可。
            KeyPairGenerator.getInstance("XDH")
        }
        val pair = generator.generateKeyPair()
        return X25519KeyPair(
            pair.private.encoded.takeLastExact(X25519_KEY_SIZE, "X25519私钥"),
            pair.public.encoded.takeLastExact(X25519_KEY_SIZE, "X25519公钥"),
        )
    }

    fun x25519PublicFromPrivate(privateKey: ByteArray): ByteArray {
        requireSize(privateKey, X25519_KEY_SIZE, "X25519私钥")
        val basePoint = ByteArray(X25519_KEY_SIZE).also { it[0] = 9 }
        return x25519SharedSecret(privateKey, basePoint)
    }

    fun x25519SharedSecret(privateKey: ByteArray, peerPublicKey: ByteArray): ByteArray {
        requireSize(privateKey, X25519_KEY_SIZE, "X25519私钥")
        requireSize(peerPublicKey, X25519_KEY_SIZE, "X25519公钥")
        val keyFactory = xdhKeyFactory()
        val local = keyFactory.generatePrivate(PKCS8EncodedKeySpec(x25519PrivatePrefix + privateKey))
        val peer = keyFactory.generatePublic(X509EncodedKeySpec(x25519PublicPrefix + peerPublicKey))
        val agreement = xdhKeyAgreement()
        agreement.init(local)
        agreement.doPhase(peer, true)
        return agreement.generateSecret().also { secret ->
            check(secret.any { it.toInt() != 0 }) { "拒绝X25519全零共享秘密" }
        }
    }

    fun generateEd25519KeyPair(): Ed25519KeyPair {
        val pair = ed25519KeyPairGenerator().generateKeyPair()
        return Ed25519KeyPair(
            pair.private.encoded.takeLastExact(ED25519_KEY_SIZE, "Ed25519私钥"),
            pair.public.encoded.takeLastExact(ED25519_KEY_SIZE, "Ed25519公钥"),
        )
    }

    fun ed25519Sign(privateKey: ByteArray, message: ByteArray): ByteArray {
        requireSize(privateKey, ED25519_KEY_SIZE, "Ed25519私钥")
        val key = ed25519KeyFactory()
            .generatePrivate(PKCS8EncodedKeySpec(ed25519PrivatePrefix + privateKey))
        return ed25519Signature().run {
            initSign(key)
            update(message)
            sign().also { requireSize(it, ED25519_SIGNATURE_SIZE, "Ed25519签名") }
        }
    }

    fun ed25519Verify(publicKey: ByteArray, message: ByteArray, signature: ByteArray): Boolean {
        requireSize(publicKey, ED25519_KEY_SIZE, "Ed25519公钥")
        if (signature.size != ED25519_SIGNATURE_SIZE) return false
        // 不依赖 Android 系统是否暴露 Ed25519/EdDSA KeyFactory。轻量级 API
        // 直接使用协议中固定长度的原始公钥，避免修改全局 JCA Provider 顺序。
        return Ed25519Signer().run {
            init(false, Ed25519PublicKeyParameters(publicKey, 0))
            update(message, 0, message.size)
            verifySignature(signature)
        }
    }

    fun hmacSha256(key: ByteArray, data: ByteArray): ByteArray =
        Mac.getInstance("HmacSHA256").run {
            init(SecretKeySpec(key, "HmacSHA256"))
            doFinal(data)
        }

    fun sha256(data: ByteArray): ByteArray = MessageDigest.getInstance("SHA-256").digest(data)

    private fun xdhKeyFactory(): KeyFactory = try {
        KeyFactory.getInstance("X25519")
    } catch (_: NoSuchAlgorithmException) {
        KeyFactory.getInstance("XDH")
    }

    private fun xdhKeyAgreement(): KeyAgreement = try {
        KeyAgreement.getInstance("X25519")
    } catch (_: NoSuchAlgorithmException) {
        KeyAgreement.getInstance("XDH")
    }

    // Android Conscrypt 部分版本使用 EdDSA 注册 RFC 8032 的 Ed25519
    // KeyFactory/KeyPairGenerator；签名服务通常仍叫 Ed25519，但也兼容 EdDSA。
    private fun ed25519KeyFactory(): KeyFactory = try {
        KeyFactory.getInstance("Ed25519")
    } catch (_: NoSuchAlgorithmException) {
        KeyFactory.getInstance("EdDSA")
    }

    private fun ed25519KeyPairGenerator(): KeyPairGenerator = try {
        KeyPairGenerator.getInstance("Ed25519")
    } catch (_: NoSuchAlgorithmException) {
        KeyPairGenerator.getInstance("EdDSA")
    }

    private fun ed25519Signature(): Signature = try {
        Signature.getInstance("Ed25519")
    } catch (_: NoSuchAlgorithmException) {
        Signature.getInstance("EdDSA")
    }

    fun hkdfSha256(
        inputKeyMaterial: ByteArray,
        salt: ByteArray,
        info: ByteArray,
        outputLength: Int,
    ): ByteArray {
        require(outputLength in 0..(255 * SHA256_SIZE)) { "HKDF输出长度非法" }
        val effectiveSalt = if (salt.isEmpty()) ByteArray(SHA256_SIZE) else salt
        val pseudoRandomKey = hmacSha256(effectiveSalt, inputKeyMaterial)
        val output = ByteArray(outputLength)
        var previous = ByteArray(0)
        var offset = 0
        var counter = 1
        try {
            while (offset < outputLength) {
                val block = hmacSha256(pseudoRandomKey, previous + info + byteArrayOf(counter.toByte()))
                previous.fill(0)
                previous = block
                val copySize = minOf(block.size, outputLength - offset)
                block.copyInto(output, offset, 0, copySize)
                offset += copySize
                counter++
            }
            return output
        } finally {
            previous.fill(0)
            pseudoRandomKey.fill(0)
        }
    }

    fun aes256GcmEncrypt(
        key: ByteArray,
        nonce: ByteArray,
        plaintext: ByteArray,
        aad: ByteArray,
    ): AesGcmResult {
        requireSize(key, AES256_KEY_SIZE, "AES-256密钥")
        requireSize(nonce, GCM_NONCE_SIZE, "AES-GCM nonce")
        val combined = Cipher.getInstance("AES/GCM/NoPadding").run {
            init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(GCM_TAG_SIZE * 8, nonce))
            updateAAD(aad)
            doFinal(plaintext)
        }
        check(combined.size >= GCM_TAG_SIZE) { "AES-GCM输出缺少认证标签" }
        return AesGcmResult(
            combined.copyOfRange(0, combined.size - GCM_TAG_SIZE),
            combined.copyOfRange(combined.size - GCM_TAG_SIZE, combined.size),
        )
    }

    fun aes256GcmDecrypt(
        key: ByteArray,
        nonce: ByteArray,
        ciphertext: ByteArray,
        aad: ByteArray,
        tag: ByteArray,
    ): ByteArray? {
        requireSize(key, AES256_KEY_SIZE, "AES-256密钥")
        requireSize(nonce, GCM_NONCE_SIZE, "AES-GCM nonce")
        requireSize(tag, GCM_TAG_SIZE, "AES-GCM标签")
        return runCatching {
            Cipher.getInstance("AES/GCM/NoPadding").run {
                init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(GCM_TAG_SIZE * 8, nonce))
                updateAAD(aad)
                doFinal(ciphertext + tag)
            }
        }.getOrNull()
    }

    fun constantTimeEqual(left: ByteArray, right: ByteArray): Boolean =
        left.size == right.size && MessageDigest.isEqual(left, right)

    fun secureClear(bytes: ByteArray) = bytes.fill(0)

    private fun requireSize(value: ByteArray, expected: Int, label: String) {
        require(value.size == expected) { "$label 长度必须为${expected}字节" }
    }

    private fun ByteArray.takeLastExact(size: Int, label: String): ByteArray {
        check(this.size >= size) { "${label}编码长度异常" }
        return copyOfRange(this.size - size, this.size)
    }

    private fun hex(value: String): ByteArray {
        require(value.length % 2 == 0)
        return ByteArray(value.length / 2) { index ->
            value.substring(index * 2, index * 2 + 2).toInt(16).toByte()
        }
    }
}
