package com.jitong.im.core

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.jitong.im.data.crypto.DbKeyFailure
import com.jitong.im.data.crypto.DbKeyManager
import com.jitong.im.data.crypto.DbKeyResult
import com.jitong.im.data.crypto.DeviceKeyWrapper
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * P7：`DbKeyManager` fail-close + ADR-01 双包装（passwordWrapper + deviceWrapper）。
 *
 * 冷启动矩阵（对应 kernel-round7-adr-db-unlock.md 验收矩阵 U01/U02/U04/U05/U10）：
 *   - 首次登录：生成 realKey，同时创建 passwordWrapper 与 deviceWrapper；
 *   - Token 冷启动（passHash 空）：经 deviceWrapper 无感解锁；
 *   - 密码错但 deviceWrapper 在：仍可无感解锁（不 fail-close）；
 *   - 密码错且 deviceWrapper 失效：有数据则 LOCKED_WITH_DATA（锁死，不删库不建空库）。
 */
@RunWith(AndroidJUnit4::class)
class DbKeyManagerFailCloseTest {

    private companion object {
        const val OWNER = 999003
        const val PASS_A = "hash-aaa"
        const val PASS_B = "hash-bbb"
    }

    private lateinit var context: Context

    private val blob: File get() = File(context.filesDir, "db_key_$OWNER.blob")
    private val deviceBlob: File get() = File(context.filesDir, "db_device_key_$OWNER.blob")
    private val dbFile: File get() = context.getDatabasePath("jitong_$OWNER.db")

    @Before
    fun setUp()
    {
        context = ApplicationProvider.getApplicationContext()
        DbKeyManager.clearLocalDatabase(context, OWNER)
        DeviceKeyWrapper.delete(context, OWNER)
    }

    @Test
    fun firstLogin_createsBothWrappers()
    {
        val r = DbKeyManager.getOrCreateRealKey(context, OWNER, PASS_A)
        assertTrue("首次应成功: $r", r is DbKeyResult.Success)
        assertEquals(32, (r as DbKeyResult.Success).key.size)
        assertTrue("passwordWrapper 应持久化", blob.exists())
        assertTrue("deviceWrapper 应创建（ADR-01 双包装）", deviceBlob.exists())
    }

    @Test
    fun samePassHash_unwrapsExistingKey()
    {
        val first = DbKeyManager.getOrCreateRealKey(context, OWNER, PASS_A) as DbKeyResult.Success
        val second = DbKeyManager.getOrCreateRealKey(context, OWNER, PASS_A)
        assertTrue("同 passHash 应解开", second is DbKeyResult.Success)
        assertTrue("解出同一把 key", first.key.contentEquals((second as DbKeyResult.Success).key))
    }

    @Test
    fun tokenColdStart_emptyPassHash_unwrapsViaDeviceWrapper()
    {
        val first = DbKeyManager.getOrCreateRealKey(context, OWNER, PASS_A) as DbKeyResult.Success
        // Token 冷启动：无 passHash（空），应经 deviceWrapper 无感解锁
        val r = DbKeyManager.getOrCreateRealKey(context, OWNER, "")
        assertTrue("Token 冷启动应无感解锁: $r", r is DbKeyResult.Success)
        assertTrue("解出同一把 key", first.key.contentEquals((r as DbKeyResult.Success).key))
    }

    @Test
    fun wrongPassHash_withDeviceWrapper_succeeds()
    {
        val first = DbKeyManager.getOrCreateRealKey(context, OWNER, PASS_A) as DbKeyResult.Success
        dbFile.parentFile?.mkdirs()
        dbFile.writeBytes(ByteArray(4096) { 0xAB.toByte() })

        // 密码错但 deviceWrapper 在：仍应无感解锁（不 fail-close）
        val r = DbKeyManager.getOrCreateRealKey(context, OWNER, PASS_B)
        assertTrue("密码错但 deviceWrapper 在应成功: $r", r is DbKeyResult.Success)
        assertTrue("解出同一把 key", first.key.contentEquals((r as DbKeyResult.Success).key))
        assertTrue("密文库不被删除", dbFile.exists())
    }

    @Test
    fun wrongPassHash_andNoDeviceWrapper_locksWithData()
    {
        DbKeyManager.getOrCreateRealKey(context, OWNER, PASS_A)
        DeviceKeyWrapper.delete(context, OWNER) // 模拟设备凭据失效
        dbFile.parentFile?.mkdirs()
        dbFile.writeBytes(ByteArray(4096) { 0xCD.toByte() })
        val before = dbFile.readBytes()

        val r = DbKeyManager.getOrCreateRealKey(context, OWNER, PASS_B)
        assertTrue("应 Unavailable: $r", r is DbKeyResult.Unavailable)
        assertEquals(DbKeyFailure.LockedWithData, (r as DbKeyResult.Unavailable).reason)
        assertTrue("密文库不得删除", dbFile.exists())
        assertTrue("密文库内容不得改动", before.contentEquals(dbFile.readBytes()))
    }

    @Test
    fun corruptPasswordBlob_withDeviceWrapper_succeeds()
    {
        val first = DbKeyManager.getOrCreateRealKey(context, OWNER, PASS_A) as DbKeyResult.Success
        // 破坏 passwordWrapper（长度异常），deviceWrapper 仍完好
        blob.writeBytes(ByteArray(11) { 0x00 })

        val r = DbKeyManager.getOrCreateRealKey(context, OWNER, PASS_A)
        assertTrue("passwordWrapper 损坏但 deviceWrapper 在应成功: $r", r is DbKeyResult.Success)
        assertTrue("解出同一把 key", first.key.contentEquals((r as DbKeyResult.Success).key))
    }

    @Test
    fun dbExistsWithoutBlob_failsClose_doesNotDeleteDb()
    {
        dbFile.parentFile?.mkdirs()
        dbFile.writeBytes(ByteArray(4096) { 0xEF.toByte() })
        val before = dbFile.readBytes()
        assertFalse("前置：key blob 不应存在", blob.exists())

        val r = DbKeyManager.getOrCreateRealKey(context, OWNER, PASS_A)
        assertTrue("应 Unavailable: $r", r is DbKeyResult.Unavailable)
        assertEquals(DbKeyFailure.DatabaseExistsWithoutKey, (r as DbKeyResult.Unavailable).reason)
        assertTrue("密文库不得删除", dbFile.exists())
        assertTrue("密文库内容不得改动", before.contentEquals(dbFile.readBytes()))
        assertFalse("不得静默生成 key blob", blob.exists())
    }

    @Test
    fun emptyPasswordCannotCreateKey() {
        val r = DbKeyManager.getOrCreateRealKey(context, OWNER, "")
        assertTrue(r is DbKeyResult.Unavailable)
        assertEquals(DbKeyFailure.NeedsPasswordUnlock, (r as DbKeyResult.Unavailable).reason)
        assertFalse(blob.exists())
        assertFalse(deviceBlob.exists())
    }

    @Test
    fun unlockOnlyCannotCreateEvenWithPassword() {
        val r = DbKeyManager.unlockRealKey(context, OWNER, PASS_A)
        assertTrue(r is DbKeyResult.Unavailable)
        assertFalse(blob.exists())
        assertFalse(deviceBlob.exists())
    }

    @Test
    fun legacyPasswordWrapperUpgradesWithoutChangingKey() {
        val key = ByteArray(32) { (it + 1).toByte() }
        val salt = ByteArray(16) { 7 }
        val iv = ByteArray(12) { 8 }
        val spec = javax.crypto.spec.PBEKeySpec(PASS_A.toCharArray(), salt, 100_000, 256)
        val wrap = javax.crypto.SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256").generateSecret(spec).encoded
        spec.clearPassword()
        val cipher = javax.crypto.Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(javax.crypto.Cipher.ENCRYPT_MODE, javax.crypto.spec.SecretKeySpec(wrap, "AES"),
            javax.crypto.spec.GCMParameterSpec(128, iv))
        blob.writeBytes(salt + iv + cipher.doFinal(key))
        wrap.fill(0)
        assertEquals(76L, blob.length())
        val r = DbKeyManager.unlockRealKey(context, OWNER, PASS_A) as DbKeyResult.Success
        assertTrue(key.contentEquals(r.key))
        assertTrue(blob.length() > 76L)
        assertFalse(r.passwordUpgradePending)
        assertTrue(r.deviceWrapperReady)
        val cold = DbKeyManager.unlockRealKey(context, OWNER) as DbKeyResult.Success
        assertTrue(key.contentEquals(cold.key))
    }

    @Test
    fun passwordWrapperCannotBeMovedToAnotherOwner() {
        val other = OWNER + 1
        DbKeyManager.clearLocalDatabase(context, other)
        try {
            DbKeyManager.getOrCreateRealKey(context, OWNER, PASS_A)
            File(context.filesDir, "db_key_$other.blob").writeBytes(blob.readBytes())
            assertTrue(DbKeyManager.unlockRealKey(context, other, PASS_A) is DbKeyResult.Unavailable)
        } finally {
            DbKeyManager.clearLocalDatabase(context, other)
        }
    }

    @Test
    fun missingPasswordBlobStillUsesDeviceKey() {
        val first = DbKeyManager.getOrCreateRealKey(context, OWNER, PASS_A) as DbKeyResult.Success
        assertTrue(blob.delete())
        dbFile.parentFile?.mkdirs()
        dbFile.writeBytes(ByteArray(128) { 7 })
        val before = dbFile.readBytes()
        val r = DbKeyManager.getOrCreateRealKey(context, OWNER, "")
        assertTrue(r is DbKeyResult.Success)
        assertTrue(first.key.contentEquals((r as DbKeyResult.Success).key))
        assertTrue(before.contentEquals(dbFile.readBytes()))
        assertFalse(blob.exists())
    }

    @Test
    fun passwordRepairsCorruptDeviceWrapper() {
        val first = DbKeyManager.getOrCreateRealKey(context, OWNER, PASS_A) as DbKeyResult.Success
        deviceBlob.writeBytes(byteArrayOf(1, 2, 3))
        val repaired = DbKeyManager.getOrCreateRealKey(context, OWNER, PASS_A)
        assertTrue(repaired is DbKeyResult.Success)
        val coldStart = DbKeyManager.getOrCreateRealKey(context, OWNER, "")
        assertTrue(coldStart is DbKeyResult.Success)
        assertTrue(first.key.contentEquals((coldStart as DbKeyResult.Success).key))
    }

    @Test
    fun readingMissingAliasDoesNotCreateIt() {
        DbKeyManager.getOrCreateRealKey(context, OWNER, PASS_A)
        val ks = java.security.KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        val alias = "jitong_db_device_key_$OWNER"
        ks.deleteEntry(alias)
        assertEquals(null, DeviceKeyWrapper.unwrap(context, OWNER))
        assertFalse(ks.containsAlias(alias))
    }

    @Test
    fun clearLocalDatabase_removesWrappersAndData()
    {
        DbKeyManager.getOrCreateRealKey(context, OWNER, PASS_A)
        dbFile.parentFile?.mkdirs()
        dbFile.writeBytes(ByteArray(4096) { 0x11.toByte() })
        assertTrue(blob.exists())
        assertTrue(deviceBlob.exists())
        assertTrue(dbFile.exists())

        val deleted = DbKeyManager.clearLocalDatabase(context, OWNER)
        assertTrue("应报告数据库已删除", deleted)
        assertFalse("passwordWrapper 应删除", blob.exists())
        assertFalse("deviceWrapper 应删除", deviceBlob.exists())
        assertFalse("数据库文件应删除", dbFile.exists())
    }
}
