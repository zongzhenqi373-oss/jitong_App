package com.jitong.im.data.crypto

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * "记住密码"这一个字段专用的保护。
 *
 * 这个值必须在用户还没登录、还没输入密码之前就能被读出来（登录页自动回填），
 * 没法像本地库那样用"密码派生的密钥"保护自己——先有鸡还是先有蛋的问题。
 * 所以这是整套本地加密方案里唯一需要用到 Android Keystore 硬件密钥的地方：
 * 密钥本身从不出安全硬件，落盘的只是密文，读到密文的人拿不到明文密码。
 */
object PasswordVault {
    private const val KEYSTORE_PROVIDER = "AndroidKeyStore"
    private const val KEY_ALIAS = "jitong_password_vault"
    private const val TRANSFORMATION = "AES/GCM/NoPadding"
    private const val GCM_IV_LEN_BYTES = 12
    private const val GCM_TAG_LEN_BITS = 128

    /** 加密明文密码，返回 iv(12B)+ciphertext，交给调用方落盘（原样存字节数组即可） */
    fun encrypt(plainPassword: String): ByteArray {
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.ENCRYPT_MODE, getOrCreateKey())
        val ciphertext = cipher.doFinal(plainPassword.toByteArray(Charsets.UTF_8))
        return cipher.iv + ciphertext
    }

    /** 解密；密钥不存在/失效（如用户重置了设备锁）/数据损坏时返回 null，调用方按"没记住密码"处理 */
    fun decrypt(blob: ByteArray): String? = runCatching {
        val iv = blob.copyOfRange(0, GCM_IV_LEN_BYTES)
        val ciphertext = blob.copyOfRange(GCM_IV_LEN_BYTES, blob.size)
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.DECRYPT_MODE, getOrCreateKey(), GCMParameterSpec(GCM_TAG_LEN_BITS, iv))
        String(cipher.doFinal(ciphertext), Charsets.UTF_8)
    }.getOrNull()

    private fun getOrCreateKey(): SecretKey {
        val keyStore = KeyStore.getInstance(KEYSTORE_PROVIDER).apply { load(null) }
        (keyStore.getKey(KEY_ALIAS, null) as? SecretKey)?.let { return it }

        val spec = KeyGenParameterSpec.Builder(
            KEY_ALIAS,
            KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT,
        )
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setKeySize(256)
            .build()
        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE_PROVIDER)
        generator.init(spec)
        return generator.generateKey()
    }
}
