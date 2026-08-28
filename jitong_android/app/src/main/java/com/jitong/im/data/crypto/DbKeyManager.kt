package com.jitong.im.data.crypto

import android.content.Context
import java.io.File
import java.security.SecureRandom
import javax.crypto.Cipher
import javax.crypto.SecretKeyFactory
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.PBEKeySpec
import javax.crypto.spec.SecretKeySpec

/**
 * 本地 Room 数据库（SQLCipher）的真实密钥管理。
 *
 * 设计：真实密钥（realKey）随机生成，从不直接落盘；落盘的是用"包装密钥"加密后的密文，
 * 包装密钥 = PBKDF2(登录密码哈希 passHash, 设备盐)。只有登录成功、拿到 passHash 之后才能
 * 解出 realKey，未登录状态下本地库无法被打开。
 *
 * 按 ownerId 隔离：不同账号各自一份 realKey + 各自一个物理库文件（AppDatabase 侧负责），
 * 避免"一个设备登录过多个账号，密钥互相解不开导致误清空对方数据"的问题。
 *
 * 不依赖 Android Keystore——纯标准密码学原语（PBKDF2 + AES-GCM），
 * 换到其他平台时这套派生/包装逻辑可以原样搬过去，只是"存文件"这一步要换成对应平台的 API。
 */
object DbKeyManager {
    private const val KEY_LEN_BYTES = 32 // 真实数据库密钥长度（AES-256 / SQLCipher raw key）
    private const val SALT_LEN_BYTES = 16
    private const val GCM_IV_LEN_BYTES = 12
    private const val GCM_TAG_LEN_BITS = 128
    private const val PBKDF2_ITERATIONS = 100_000

    /**
     * 拿到指定账号可用的真实数据库密钥：本地已有且能用当前 passHash 解开就直接返回；
     * 解不开（首次登录 / 密码在别处改过 / 文件损坏）就重新生成一份并覆盖旧文件——
     * 调用方需保证：旧库因此作废时，上层走 destructive migration 重建（漫游补发恢复历史消息）。
     *
     * 涉及磁盘 I/O 和 PBKDF2（约 10 万次迭代），调用方需在 IO 线程调用，不要放主线程。
     */
    fun getOrCreateRealKey(context: Context, ownerId: Int, passHash: String): ByteArray {
        val file = blobFile(context, ownerId)
        if (file.exists()) {
            runCatching { unwrap(file, passHash) }.getOrNull()?.let { return it }
            // 包装密钥已损坏或登录密码已变化：旧 SQLCipher 库不可能被新密钥打开。
            // fallbackToDestructiveMigration 只处理 Room 版本迁移，处理不了“密钥错误”。
            deleteEncryptedDatabase(context, ownerId)
        } else if (context.getDatabasePath(databaseName(ownerId)).exists()) {
            // 数据库存在但包装密钥丢失时同理；也覆盖从旧版明文 Room 首次升级的场景。
            deleteEncryptedDatabase(context, ownerId)
        }
        return generateAndPersist(file, passHash)
    }

    private fun databaseName(ownerId: Int) = "jitong_$ownerId.db"

    private fun deleteEncryptedDatabase(context: Context, ownerId: Int) {
        check(context.deleteDatabase(databaseName(ownerId))) {
            "无法清理已失去密钥的本地数据库"
        }
    }

    private fun blobFile(context: Context, ownerId: Int): File =
        File(context.filesDir, "db_key_$ownerId.blob")

    private fun generateAndPersist(file: File, passHash: String): ByteArray {
        val random = SecureRandom()
        val realKey = ByteArray(KEY_LEN_BYTES).also { random.nextBytes(it) }
        val salt = ByteArray(SALT_LEN_BYTES).also { random.nextBytes(it) }
        val iv = ByteArray(GCM_IV_LEN_BYTES).also { random.nextBytes(it) }
        val wrapKey = deriveWrapKey(passHash, salt)

        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(wrapKey, "AES"), GCMParameterSpec(GCM_TAG_LEN_BITS, iv))
        val ciphertext = cipher.doFinal(realKey)

        // 落盘格式：salt(16B) + iv(12B) + ciphertext(含 GCM tag)；salt/iv 本身不需要保密
        file.writeBytes(salt + iv + ciphertext)
        return realKey
    }

    private fun unwrap(file: File, passHash: String): ByteArray {
        val bytes = file.readBytes()
        // 32 字节明文经 GCM 加密后附带 16 字节认证标签。
        val expectedSize = SALT_LEN_BYTES + GCM_IV_LEN_BYTES + KEY_LEN_BYTES + GCM_TAG_LEN_BITS / 8
        require(bytes.size == expectedSize) { "数据库密钥文件格式无效" }
        val salt = bytes.copyOfRange(0, SALT_LEN_BYTES)
        val iv = bytes.copyOfRange(SALT_LEN_BYTES, SALT_LEN_BYTES + GCM_IV_LEN_BYTES)
        val ciphertext = bytes.copyOfRange(SALT_LEN_BYTES + GCM_IV_LEN_BYTES, bytes.size)
        val wrapKey = deriveWrapKey(passHash, salt)

        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(wrapKey, "AES"), GCMParameterSpec(GCM_TAG_LEN_BITS, iv))
        return cipher.doFinal(ciphertext) // passHash 不对 / 内容被篡改时 GCM 校验失败，抛异常
    }

    private fun deriveWrapKey(passHash: String, salt: ByteArray): ByteArray {
        val spec = PBEKeySpec(passHash.toCharArray(), salt, PBKDF2_ITERATIONS, KEY_LEN_BYTES * 8)
        val factory = SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256")
        return factory.generateSecret(spec).encoded
    }
}
