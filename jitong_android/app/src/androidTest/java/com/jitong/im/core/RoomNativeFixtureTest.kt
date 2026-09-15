package com.jitong.im.core

import android.content.Context
import androidx.sqlite.db.SupportSQLiteDatabase
import androidx.sqlite.db.SupportSQLiteOpenHelper
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.jitong.im.data.crypto.DbKeyManager
import com.jitong.im.data.crypto.DbKeyResult
import net.zetetic.database.sqlcipher.SupportOpenHelperFactory
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * S1-3：Room（sqlcipher-android）与 Native（C++ CipherDatabase）**双向互开**验证。
 *
 * 这是 P6 最高优先级的前置验证：只有证明两边用同一 raw key、同一 cipher 参数可以互相
 * 打开同一份加密库，后续的 Schema/Migration/影子迁移才有意义。
 *
 * 方向一（Native → Room）：Native 用 `CipherDatabase` 建加密库并写入固定 marker，
 *   再由 Room 使用的 `SupportOpenHelperFactory`（同一 SQLCipher 栈）打开并读出 marker。
 * 方向二（Room → Native）：Room 侧建加密库并写入内容，再由 Native 只读打开，
 *   回报 cipher 版本与表数量。
 * 负向：错误 key 时 Native 必须 fail-close（不删库、不改文件）。
 *
 * key 取自真实 `DbKeyManager.getOrCreateRealKey()`（测试专用 ownerId），保证与生产
 * 语义一致（32 字节随机 realKey + PBKDF2(passHash) 包装）。
 *
 * 运行：
 * ```bash
 * ./gradlew :app:connectedDebugAndroidTest -x lint \
 *   -Pandroid.testInstrumentationRunnerArguments.class=com.jitong.im.core.RoomNativeFixtureTest
 * ```
 */
@RunWith(AndroidJUnit4::class)
class RoomNativeFixtureTest {

    private companion object {
        const val OWNER_A = 999001
        const val OWNER_B = 999002
        const val PASS_HASH = "testhash-for-fixture-only"
        val NATIVE_MARKER = "NATIVE_FIXTURE_MARKER_3E7B"
    }

    private lateinit var context: Context

    @Before
    fun setUp()
    {
        context = ApplicationProvider.getApplicationContext()
        assumeTrue("Native .so 不可用", NativeBindings.isLoaded)
        // 与 AppDatabase 一致：显式加载 Room 使用的 SQLCipher 库
        System.loadLibrary("sqlcipher")
    }

    private fun realKey(ownerId: Int): ByteArray
    {
        // P6-T02 后 getOrCreateRealKey 返回 DbKeyResult（fail-close）
        val result = DbKeyManager.getOrCreateRealKey(context, ownerId, PASS_HASH)
        assertTrue("取 realKey 失败: $result", result is DbKeyResult.Success)
        val key = (result as DbKeyResult.Success).key
        assertEquals("realKey 必须是 32 字节", 32, key.size)
        return key
    }

    /** 用 Room 的 SQLCipher 栈打开一个加密库并执行传入的 SQL 块。 */
    private fun withRoomDb(path: String, key: ByteArray, block: (SupportSQLiteDatabase) -> Unit)
    {
        val factory = SupportOpenHelperFactory(key.copyOf())
        val config = SupportSQLiteOpenHelper.Configuration.builder(context)
            .name(path)
            .callback(object : SupportSQLiteOpenHelper.Callback(1) {
                override fun onCreate(db: SupportSQLiteDatabase) = Unit
                override fun onUpgrade(db: SupportSQLiteDatabase, old: Int, new: Int) = Unit
            })
            .build()
        val helper = factory.create(config)
        try {
            block(helper.writableDatabase)
        } finally {
            helper.close()
        }
    }

    @Test
    fun nativeCreatedDb_isReadableByRoom()
    {
        val key = realKey(OWNER_A)
        val path = File(context.filesDir, "s13_fixture_native.db").absolutePath
        File(path).delete()

        // 1) Native 建库 + 写 marker
        val created = NativeBindings.nativeCipherCreateFixtureForTest(path, key)
        assertTrue("Native 建 fixture 失败: $created", created.startsWith("ok|"))
        assertTrue("cipher 版本不符: $created", created.contains("cipher=4.6.1"))

        // 2) Room（sqlcipher-android）打开同一文件并读出 marker
        var marker: String? = null
        withRoomDb(path, key) { db ->
            db.query("SELECT marker FROM native_probe LIMIT 1").use { c ->
                if (c.moveToFirst()) marker = c.getString(0)
            }
        }
        assertEquals("Room 应能读到 Native 写入的 marker", NATIVE_MARKER, marker)
    }

    @Test
    fun roomCreatedDb_isReadableByNative()
    {
        val key = realKey(OWNER_B)
        val path = File(context.filesDir, "s13_fixture_room.db").absolutePath
        File(path).delete()

        // 1) Room 建库 + 建表 + 写内容
        withRoomDb(path, key) { db ->
            db.execSQL("CREATE TABLE IF NOT EXISTS room_probe(marker TEXT)")
            db.execSQL("INSERT INTO room_probe(marker) VALUES('ROOM_MARKER_1A2B')")
        }
        assertTrue("Room 建的库文件应存在", File(path).exists())

        // 2) Native 只读打开
        val verified = NativeBindings.nativeCipherVerifyFixtureForTest(path, key)
        assertTrue("Native 打开 Room 库失败: $verified", verified.startsWith("ok|"))
        assertTrue("cipher 版本不符: $verified", verified.contains("cipher=4.6.1"))
        assertTrue("应至少有一张表: $verified", verified.contains("tables=1") || verified.contains("tables=2"))
    }

    @Test
    fun wrongKey_failsClosedAndLeavesFileIntact()
    {
        val key = realKey(OWNER_A)
        val path = File(context.filesDir, "s13_fixture_wrongkey.db").absolutePath
        File(path).delete()

        val created = NativeBindings.nativeCipherCreateFixtureForTest(path, key)
        assertTrue(created.startsWith("ok|"))
        val before = File(path).readBytes().copyOf()

        val wrong = ByteArray(32) { 0x77 }
        val verified = NativeBindings.nativeCipherVerifyFixtureForTest(path, wrong)
        assertTrue("错误 key 必须失败: $verified", verified.startsWith("err|"))
        assertTrue("错误码应为 NotEncrypted: $verified", verified.contains("NotEncrypted"))

        // fail-close：文件不得被删除/覆盖/截断
        val after = File(path).readBytes()
        assertTrue("错误 key 后文件必须保持不变（不删库、不覆盖）", before.contentEquals(after))
    }
}
