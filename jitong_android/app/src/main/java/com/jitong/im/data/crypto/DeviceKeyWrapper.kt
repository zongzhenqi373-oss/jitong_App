package com.jitong.im.data.crypto

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import java.io.File
import java.io.FileOutputStream
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * 数据库 realKey 的设备侧包装（ADR-01 双包装中的 `deviceWrapper`）。
 *
 * 用途：Token 冷启动时无需用户输入密码即可解出 realKey，实现"无感解锁"。
 *
 * 安全边界（**必须由 Keystore 参数证明，不能仅凭"用了 Keystore"就声称成立**）：
 *   - Keystore alias 含 ownerId，**不跨账号复用**；
 *   - [setUserAuthenticationRequired] 为 **false**（默认）：与 [TokenVault] 对 Token 的保护
 *     同级，但不保证锁屏时不可用；后台访问由平台/设备策略决定；
 *   - 若产品决定要求锁屏/生物认证，改为 `setUserAuthenticationRequired(true)` 即可，
 *     副作用是锁屏状态下无感解锁会失败并回退到 NEEDS_PASSWORD_UNLOCK。
 *
 * v1: authenticated header(owner/type/version) ‖ iv(12) ‖ ciphertext(+GCM tag)。
 * 兼容旧 iv(12) ‖ ciphertext(48)；成功读取旧格式后由账号锁内的写路径升级。
 */
object DeviceKeyWrapper {
    private const val ANDROID_KEYSTORE = "AndroidKeyStore"
    private const val IV_LEN = 12
    private const val TAG_BITS = 128
    private fun header(ownerId: Int) = "JTDB:1:device:$ownerId:".toByteArray(Charsets.UTF_8)

    private fun alias(ownerId: Int) = "jitong_db_device_key_$ownerId"
    private fun blobFile(context: Context, ownerId: Int) =
        File(context.filesDir, "db_device_key_$ownerId.blob")

    fun exists(context: Context, ownerId: Int): Boolean = blobFile(context, ownerId).exists()
    fun isCurrentFormat(context: Context, ownerId: Int): Boolean =
        blobFile(context, ownerId).length() == (header(ownerId).size + IV_LEN + 32 + TAG_BITS / 8).toLong()

    /** 生成（或取现有）Keystore 密钥。 */
    private fun getOrCreateKey(ownerId: Int): SecretKey {
        val ks = KeyStore.getInstance(ANDROID_KEYSTORE).apply { load(null) }
        (ks.getKey(alias(ownerId), null) as? SecretKey)?.let { return it }

        val gen = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, ANDROID_KEYSTORE)
        gen.init(
            KeyGenParameterSpec.Builder(alias(ownerId), KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                // 见类注释：与 TokenVault 同级保护，不绑锁屏/生物认证
                .setUserAuthenticationRequired(false)
                .build()
        )
        return gen.generateKey()
    }

    /**
     * 用 Keystore key 加密 realKey 并落盘 deviceWrapper。
     * 只在**已成功获得 realKey**（密码登录解开或新建）后调用；失败不影响主流程。
     */
    fun create(context: Context, ownerId: Int, realKey: ByteArray): Boolean {
        return try {
            require(realKey.size == 32)
            val key = getOrCreateKey(ownerId)
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.ENCRYPT_MODE, key)
            cipher.updateAAD(header(ownerId))
            val ciphertext = cipher.doFinal(realKey)

            val tmp = File.createTempFile("db_device_key_$ownerId-", ".tmp", context.filesDir)
            try {
                FileOutputStream(tmp).use { fos ->
                    fos.write(header(ownerId))
                    fos.write(cipher.iv)
                    fos.write(ciphertext)
                    fos.fd.sync()
                }
                if (!tmp.renameTo(blobFile(context, ownerId))) {
                    tmp.delete()
                    return false
                }
            } catch (e: Exception) {
                tmp.delete()
                throw e
            }
            blobFile(context, ownerId).setReadable(true, true)
            blobFile(context, ownerId).setWritable(true, true)
            true
        } catch (e: Exception) {
            false // Keystore 不可用：不阻断主流程，回退密码解锁
        }
    }

    /**
     * 用 Keystore key 解出 realKey。
     * @return null 表示失败（Keystore key 失效 / blob 损坏 / 被篡改），调用方回退密码解锁
     */
    fun unwrap(context: Context, ownerId: Int): ByteArray? {
        val file = blobFile(context, ownerId)
        if (!file.exists()) return null
        return try {
            val bytes = file.readBytes()
            val header = header(ownerId)
            val legacy = bytes.size == IV_LEN + 32 + TAG_BITS / 8
            val offset = if (legacy) 0 else header.size
            if (!legacy && (bytes.size != header.size + IV_LEN + 32 + TAG_BITS / 8 ||
                        !bytes.copyOfRange(0, header.size).contentEquals(header))) return null
            val iv = bytes.copyOfRange(offset, offset + IV_LEN)
            val ciphertext = bytes.copyOfRange(offset + IV_LEN, bytes.size)
            // Reads must never create/replace an alias (lost key remains a recoverable failure).
            val ks = KeyStore.getInstance(ANDROID_KEYSTORE).apply { load(null) }
            val key = ks.getKey(alias(ownerId), null) as? SecretKey ?: return null
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, key, GCMParameterSpec(TAG_BITS, iv))
            if (!legacy) cipher.updateAAD(header)
            cipher.doFinal(ciphertext).takeIf { it.size == 32 }
        } catch (e: Exception) {
            null
        }
    }

    /** 删除 deviceWrapper（登出清理时可选调用，见 ADR-01 待定项）。 */
    fun delete(context: Context, ownerId: Int) {
        blobFile(context, ownerId).delete()
        runCatching {
            val ks = KeyStore.getInstance(ANDROID_KEYSTORE).apply { load(null) }
            ks.deleteEntry(alias(ownerId))
        }
    }
}
