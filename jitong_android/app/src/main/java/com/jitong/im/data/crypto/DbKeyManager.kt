package com.jitong.im.data.crypto

import android.content.Context
import java.io.File
import java.io.FileOutputStream
import java.io.RandomAccessFile
import java.security.SecureRandom
import java.util.concurrent.ConcurrentHashMap
import javax.crypto.Cipher
import javax.crypto.SecretKeyFactory
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.PBEKeySpec
import javax.crypto.spec.SecretKeySpec

/** 密钥不可用原因（用于 fail-close 诊断，不含任何密钥材料）。 */
enum class DbKeyFailure {
    /** key blob 长度/格式异常（文件损坏或被篡改）。 */
    BlobCorrupt,

    /** key blob 存在且格式正常，但当前 passHash 解不开（密码在别处改过）。 */
    UnwrapFailed,

    /** 数据库文件存在但 key blob 丢失——不得静默生成新 key，否则旧库永久不可读。 */
    DatabaseExistsWithoutKey,

    /** Token 冷启动：deviceWrapper 不可用（无 wrapper / Keystore 失效），需密码解锁。 */
    NeedsPasswordUnlock,

    /** 有密文数据但 passwordWrapper 与 deviceWrapper 都解不开（密码错 + 设备凭据坏）。 */
    LockedWithData,
}

/** 取密钥的结果：成功给 32 字节 realKey，失败给原因（绝不删库/覆盖/静默重建）。 */
sealed interface DbKeyResult {
    data class Success(val key: ByteArray, val deviceWrapperReady: Boolean = false,
                       val passwordUpgradePending: Boolean = false) : DbKeyResult
    data class Unavailable(val reason: DbKeyFailure) : DbKeyResult
}

/**
 * 本地 Room 数据库（SQLCipher）的真实密钥管理。
 *
 * 设计：真实密钥（realKey）随机生成，从不直接落盘；落盘的是用"包装密钥"加密后的密文，
 * 包装密钥 = PBKDF2(登录密码哈希 passHash, 设备盐)。只有登录成功、拿到 passHash 之后才能
 * 解出 realKey；升级后的设备包装也可解锁，账号鉴权必须由账号层独立校验。
 *
 * 按 ownerId 隔离：不同账号各自一份 realKey + 各自一个物理库文件（AppDatabase 侧负责），
 * 避免"一个设备登录过多个账号，密钥互相解不开导致误清空对方数据"的问题。
 *
 * 密码包装使用 PBKDF2 + AES-GCM，设备包装使用 Android Keystore；跨平台时设备包装由平台适配器提供。
 */
object DbKeyManager {
    private const val KEY_LEN_BYTES = 32 // 真实数据库密钥长度（AES-256 / SQLCipher raw key）
    private const val SALT_LEN_BYTES = 16
    private const val GCM_IV_LEN_BYTES = 12
    private const val GCM_TAG_LEN_BITS = 128
    private const val PBKDF2_ITERATIONS = 100_000

    private val BLOB_SIZE = SALT_LEN_BYTES + GCM_IV_LEN_BYTES + KEY_LEN_BYTES + GCM_TAG_LEN_BITS / 8

    // F12：按 ownerId 串行化 key 创建，避免并发首次创建各生成一把 key 或出现半写
    private val ownerLocks = ConcurrentHashMap<Int, Any>()

    /**
     * 拿到指定账号可用的真实数据库密钥。
     *
     * **P6-T02 改为 fail-close**：解不开（密码在别处改过 / blob 损坏 / blob 丢失但库还在）时
     * 一律返回 [DbKeyResult.Unavailable]，**绝不**：
     *   - 删除或覆盖密文数据库；
     *   - 覆盖已有的 key blob；
     *   - 静默生成一把新 key（那会让旧库永久不可读，等同于删库）。
     * 旧库保留在磁盘上供诊断与人工恢复；若确需清理，由用户显式调用 [clearLocalDatabase]。
     *
     * 涉及磁盘 I/O 和 PBKDF2（约 10 万次迭代），调用方需在 IO 线程调用，不要放主线程。
     */
    fun getOrCreateRealKey(context: Context, ownerId: Int, passHash: String): DbKeyResult =
        accessKey(context, ownerId, passHash, allowCreate = true)

    /** Unlock only: a missing key is never interpreted as permission to create a database. */
    fun unlockRealKey(context: Context, ownerId: Int, passHash: String = ""): DbKeyResult =
        accessKey(context, ownerId, passHash, allowCreate = false)

    private fun accessKey(context: Context, ownerId: Int, passHash: String, allowCreate: Boolean): DbKeyResult {
        require(ownerId > 0) { "ownerId must be positive" }
        val lock = ownerLocks.computeIfAbsent(ownerId) { Any() }
        synchronized(lock) {
            // synchronized 只解决同进程线程竞争；文件锁覆盖 Android 多进程首次创建。
            val lockFile = File(context.filesDir, "db_key_$ownerId.lock")
            RandomAccessFile(lockFile, "rw").channel.use { channel ->
                channel.lock().use {
                    return getOrCreateRealKeyLocked(context, ownerId, passHash, allowCreate)
                }
            }
        }
    }

    private fun getOrCreateRealKeyLocked(context: Context, ownerId: Int, passHash: String, allowCreate: Boolean): DbKeyResult {
        val file = blobFile(context, ownerId)
        if (file.exists()) {
            // 1) 密码路径：passHash 非空时先用它解（登录/重输密码）
            if (passHash.isNotEmpty()) {
                runCatching { unwrap(file, passHash) }.getOrNull()?.let { key ->
                    // Repair inside the same per-account/process lock, including corrupt blobs.
                    val deviceKey = DeviceKeyWrapper.unwrap(context, ownerId)
                    val deviceReady = if (deviceKey == null || !deviceKey.contentEquals(key) ||
                        !DeviceKeyWrapper.isCurrentFormat(context, ownerId)) {
                        DeviceKeyWrapper.create(context, ownerId, key)
                    } else true
                    val upgradePending = file.length() == BLOB_SIZE.toLong() &&
                        runCatching { generateAndPersist(file, passHash, key) }.isFailure
                    return DbKeyResult.Success(key, deviceReady, upgradePending)
                }
            }
            // 2) 设备路径：deviceWrapper 无感解锁（Token 冷启动，ADR-01 双包装）
            DeviceKeyWrapper.unwrap(context, ownerId)?.let { return DbKeyResult.Success(it, true) }

            // 3) 两者皆失败：fail-close，区分"有数据锁死"与"仅需密码"
            val hasData = roomDatabaseExists(context, ownerId) || nativeDatabaseExists(context, ownerId)
            if (hasData) {
                return DbKeyResult.Unavailable(DbKeyFailure.LockedWithData)
            }
            if (passHash.isEmpty()) {
                return DbKeyResult.Unavailable(DbKeyFailure.NeedsPasswordUnlock)
            }
            val reason = if (file.length() == BLOB_SIZE.toLong() || file.length() == BLOB_SIZE + passwordHeader(file).size.toLong()) {
                DbKeyFailure.UnwrapFailed
            } else {
                DbKeyFailure.BlobCorrupt
            }
            return DbKeyResult.Unavailable(reason)
        }
        // Two independent wrappers: missing password blob must not skip the device path.
        DeviceKeyWrapper.unwrap(context, ownerId)?.let { return DbKeyResult.Success(it, true) }
        if (DeviceKeyWrapper.exists(context, ownerId)) {
            return DbKeyResult.Unavailable(
                if (roomDatabaseExists(context, ownerId) || nativeDatabaseExists(context, ownerId))
                    DbKeyFailure.LockedWithData else DbKeyFailure.BlobCorrupt
            )
        }
        // blob 不存在：若数据库已存在，说明密钥丢失，不得静默生成新 key。
        // R05：已有密文数据 = Room 库 OR Native 影子库任一存在（含 -wal/-shm）。
        if (roomDatabaseExists(context, ownerId) || nativeDatabaseExists(context, ownerId)) {
            return DbKeyResult.Unavailable(DbKeyFailure.DatabaseExistsWithoutKey)
        }
        if (passHash.isBlank() || !allowCreate) {
            return DbKeyResult.Unavailable(DbKeyFailure.NeedsPasswordUnlock)
        }
        // 首次创建：生成 realKey 后同时创建 deviceWrapper（尽力而为，失败回退密码解锁）
        val key = generateAndPersist(file, passHash)
        val deviceReady = DeviceKeyWrapper.create(context, ownerId, key)
        return DbKeyResult.Success(key, deviceReady)
    }

    private fun nativeDatabaseExists(context: Context, ownerId: Int): Boolean {
        val base = File(context.filesDir, "native_db/account_$ownerId.db")
        return base.exists() ||
            File(base.path + "-wal").exists() ||
            File(base.path + "-shm").exists()
    }

    private fun roomDatabaseExists(context: Context, ownerId: Int): Boolean {
        val base = context.getDatabasePath(databaseName(ownerId))
        return base.exists() || File(base.path + "-wal").exists() || File(base.path + "-shm").exists()
    }

    private fun databaseName(ownerId: Int) = "jitong_$ownerId.db"

    /**
     * **显式**清除本账号的本地数据与密钥（供用户"清除本机数据"操作调用）。
     *
     * 这是**唯一**允许删除密文库的路径；认证/登录流程**禁止**自动调用它。
     * @return 是否清除了数据库文件（key blob 无论是否存在都会被删除）
     */
    fun clearLocalDatabase(context: Context, ownerId: Int): Boolean {
        val lock = ownerLocks.computeIfAbsent(ownerId) { Any() }
        synchronized(lock) {
            val lockFile = File(context.filesDir, "db_key_$ownerId.lock")
            RandomAccessFile(lockFile, "rw").channel.use { channel ->
                channel.lock().use {
                    fun deleteIfPresent(file: File): Boolean = !file.exists() || file.delete()
                    val room = context.getDatabasePath(databaseName(ownerId))
                    val native = File(context.filesDir, "native_db/account_$ownerId.db")
                    val databasesDeleted =
                        deleteIfPresent(room) && deleteIfPresent(File(room.path + "-wal")) &&
                            deleteIfPresent(File(room.path + "-shm")) && deleteIfPresent(native) &&
                            deleteIfPresent(File(native.path + "-wal")) &&
                            deleteIfPresent(File(native.path + "-shm"))
                    // 数据文件未完全清除时必须保留 blob，否则剩余密文将永久不可恢复。
                    val blobDeleted = databasesDeleted && deleteIfPresent(blobFile(context, ownerId))
                    // 连同 deviceWrapper 一起清（ADR-01：清理必须显式）
                    if (blobDeleted) DeviceKeyWrapper.delete(context, ownerId)
                    return blobDeleted
                }
            }
        }
    }

    private fun blobFile(context: Context, ownerId: Int): File =
        File(context.filesDir, "db_key_$ownerId.blob")

    private fun passwordHeader(file: File) = "JTDB:1:password:${file.name}:".toByteArray(Charsets.UTF_8)

    private fun generateAndPersist(file: File, passHash: String,
                                   realKey: ByteArray = ByteArray(KEY_LEN_BYTES).also { SecureRandom().nextBytes(it) }): ByteArray {
        require(passHash.isNotBlank() && realKey.size == KEY_LEN_BYTES)
        val random = SecureRandom()
        val salt = ByteArray(SALT_LEN_BYTES).also { random.nextBytes(it) }
        val iv = ByteArray(GCM_IV_LEN_BYTES).also { random.nextBytes(it) }
        val wrapKey = deriveWrapKey(passHash, salt)

        val ciphertext = try {
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(wrapKey, "AES"), GCMParameterSpec(GCM_TAG_LEN_BITS, iv))
            cipher.updateAAD(passwordHeader(file))
            cipher.doFinal(realKey)
        } finally {
            wrapKey.fill(0)
        }

        // R12：原子落盘：写临时文件 → fsync → 原子替换；失败删临时文件、保留旧 blob。
        // v1：AAD header(owner/type/version) + salt(16B) + iv(12B) + ciphertext/tag。
        val tmp = File(file.parentFile, file.name + ".tmp.${android.os.Process.myPid()}.${System.nanoTime()}")
        try {
            FileOutputStream(tmp).use { fos ->
                fos.write(passwordHeader(file))
                fos.write(salt)
                fos.write(iv)
                fos.write(ciphertext)
                fos.fd.sync() // 确保落盘后再替换
            }
            if (!tmp.renameTo(file)) {
                tmp.delete()
                throw IllegalStateException("key blob 原子替换失败")
            }
            file.setReadable(true, true)
            file.setWritable(true, true)
            file.setExecutable(false, false)
        } catch (e: Exception) {
            tmp.delete()
            throw e
        }
        return realKey
    }

    private fun unwrap(file: File, passHash: String): ByteArray {
        val bytes = file.readBytes()
        // 32 字节明文经 GCM 加密后附带 16 字节认证标签。
        val expectedSize = SALT_LEN_BYTES + GCM_IV_LEN_BYTES + KEY_LEN_BYTES + GCM_TAG_LEN_BITS / 8
        val header = passwordHeader(file)
        val legacy = bytes.size == expectedSize
        val offset = if (legacy) 0 else header.size
        require(legacy || (bytes.size == expectedSize + header.size &&
            bytes.copyOfRange(0, header.size).contentEquals(header))) { "数据库密钥文件格式无效" }
        val salt = bytes.copyOfRange(offset, offset + SALT_LEN_BYTES)
        val iv = bytes.copyOfRange(offset + SALT_LEN_BYTES, offset + SALT_LEN_BYTES + GCM_IV_LEN_BYTES)
        val ciphertext = bytes.copyOfRange(offset + SALT_LEN_BYTES + GCM_IV_LEN_BYTES, bytes.size)
        val wrapKey = deriveWrapKey(passHash, salt)

        return try {
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(wrapKey, "AES"), GCMParameterSpec(GCM_TAG_LEN_BITS, iv))
            if (!legacy) cipher.updateAAD(header)
            cipher.doFinal(ciphertext).also { require(it.size == KEY_LEN_BYTES) }
        } finally {
            wrapKey.fill(0)
        }
    }

    private fun deriveWrapKey(passHash: String, salt: ByteArray): ByteArray {
        val spec = PBEKeySpec(passHash.toCharArray(), salt, PBKDF2_ITERATIONS, KEY_LEN_BYTES * 8)
        return try {
            val factory = SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256")
            factory.generateSecret(spec).encoded
        } finally {
            spec.clearPassword()
        }
    }
}
