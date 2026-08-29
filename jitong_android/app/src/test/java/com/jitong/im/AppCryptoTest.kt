package com.jitong.im

import com.jitong.im.net.AppCrypto
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class AppCryptoTest {
    @Test
    fun `RFC7748 X25519向量与C++一致`() {
        val alicePrivate = hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
        val alicePublic = hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a")
        val bobPrivate = hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb")
        val bobPublic = hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f")
        val shared = hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742")
        assertArrayEquals(alicePublic, AppCrypto.x25519PublicFromPrivate(alicePrivate))
        assertArrayEquals(bobPublic, AppCrypto.x25519PublicFromPrivate(bobPrivate))
        assertArrayEquals(shared, AppCrypto.x25519SharedSecret(alicePrivate, bobPublic))
        assertArrayEquals(shared, AppCrypto.x25519SharedSecret(bobPrivate, alicePublic))
    }

    @Test
    fun `RFC5869 HKDF向量与C++一致`() {
        val result = AppCrypto.hkdfSha256(
            ByteArray(22) { 0x0b },
            hex("000102030405060708090a0b0c"),
            hex("f0f1f2f3f4f5f6f7f8f9"),
            42,
        )
        assertArrayEquals(
            hex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"),
            result,
        )
    }

    @Test
    fun `AES256 GCM向量篡改即失败`() {
        val encrypted = AppCrypto.aes256GcmEncrypt(ByteArray(32), ByteArray(12), ByteArray(16), ByteArray(0))
        assertArrayEquals(hex("cea7403d4d606b6e074ec5d3baf39d18"), encrypted.ciphertext)
        assertArrayEquals(hex("d0d1c8a799996bf0265b98b5d48ab919"), encrypted.tag)
        assertArrayEquals(
            ByteArray(16),
            AppCrypto.aes256GcmDecrypt(ByteArray(32), ByteArray(12), encrypted.ciphertext, ByteArray(0), encrypted.tag),
        )
        val changedTag = encrypted.tag.copyOf().also { it[0] = (it[0].toInt() xor 1).toByte() }
        assertNull(AppCrypto.aes256GcmDecrypt(ByteArray(32), ByteArray(12), encrypted.ciphertext, ByteArray(0), changedTag))
    }

    @Test
    fun `Ed25519签名验证和密钥清理`() {
        val pair = AppCrypto.generateEd25519KeyPair()
        val message = "jitong".toByteArray()
        val signature = AppCrypto.ed25519Sign(pair.privateKey, message)
        assertTrue(AppCrypto.ed25519Verify(pair.publicKey, message, signature))
        assertFalse(AppCrypto.ed25519Verify(pair.publicKey, "changed".toByteArray(), signature))
        assertTrue(AppCrypto.constantTimeEqual(message, message.copyOf()))
        val secret = AppCrypto.randomBytes(32)
        assertNotNull(secret)
        AppCrypto.secureClear(secret)
        assertTrue(secret.all { it.toInt() == 0 })
    }

    private fun hex(value: String): ByteArray = ByteArray(value.length / 2) { index ->
        value.substring(index * 2, index * 2 + 2).toInt(16).toByte()
    }
}
