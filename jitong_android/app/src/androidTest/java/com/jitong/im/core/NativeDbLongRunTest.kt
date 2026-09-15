package com.jitong.im.core

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.jitong.im.core.platform.NativeDbKeyPlatform
import com.jitong.im.data.crypto.DbKeyManager
import com.jitong.im.data.crypto.DbKeyResult
import org.junit.After
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * P6 长稳验证（§24.9.3）：
 *   - 循环 open → selfTest → close 100 次，fd 数量回到基线（锁/连接池/队列不泄漏）；
 *   - 循环 open → import(空批写 checkpoint) → query → close 100 次，导入路径无泄漏；
 *   - 关库重开后库仍可正常打开（跨进程锁正确释放 + Schema 持久）。
 *
 * 说明：真正的 OS 级「强杀进程」需要分阶段 instrumentation + 外部脚本协调；其核心语义
 * （checkpoint 持久化 + 从 checkpoint 续跑）已由桌面 test_room_import 覆盖，这里以
 * 「关库重开」等价验证 Android 上的持久化恢复。
 */
@RunWith(AndroidJUnit4::class)
class NativeDbLongRunTest {

    private companion object {
        const val OWNER = 999005
        const val PASS = "longrun-fixture"
        const val LOOPS = 100
    }

    private lateinit var context: Context
    private lateinit var bridge: NativeDbKeyPlatform
    private var handle: Long = 0L
    private val nativeDir: File get() = File(context.filesDir, "longrun_native")

    @Before
    fun setUp()
    {
        context = ApplicationProvider.getApplicationContext()
        assumeTrue("Native .so 不可用", NativeBindings.isLoaded)
        System.loadLibrary("sqlcipher")
        DbKeyManager.clearLocalDatabase(context, OWNER)
        nativeDir.deleteRecursively()
        nativeDir.mkdirs()
        val r = DbKeyManager.getOrCreateRealKey(context, OWNER, PASS)
        assertTrue("取 realKey 失败: $r", r is DbKeyResult.Success)
        bridge = NativeDbKeyPlatform(context).also { it.setPassHash(PASS) }
        handle = NativeBindings.nativeCreate("im.example.com")
        assertTrue("创建内核句柄失败", handle != 0L)
    }

    @After
    fun tearDown()
    {
        if (handle != 0L) {
            NativeBindings.nativeCloseAccountDatabase(handle)
            NativeBindings.nativeDestroy(handle)
            handle = 0L
        }
    }

    private fun fdCount(): Int =
        runCatching { File("/proc/self/fd").listFiles()?.size ?: 0 }.getOrDefault(0)

    private fun open(): Boolean =
        NativeBindings.nativeOpenAccountDatabase(handle, OWNER, nativeDir.absolutePath, bridge)

    private fun close() = NativeBindings.nativeCloseAccountDatabase(handle)

    /** 构造一个合法的空迁移批次（magic + version + count=0，小端）。 */
    private fun emptyBatch(): ByteArray =
        ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN)
            .putInt(0x4A544D47).putInt(1).putInt(0).array()

    @Test
    fun openCloseHundredTimes_fdReturnsToBaseline()
    {
        val baseFd = fdCount()
        for (i in 1..LOOPS) {
            assertTrue("第 $i 次打开失败", open())
            val self = NativeBindings.nativeRunDatabaseSelfTest(handle)
            assertTrue("第 $i 次自检失败: $self", self.startsWith("ok|status=Ready"))
            close()
        }
        System.gc()
        val endFd = fdCount()
        assertTrue(
            "fd 应回到基线（base=$baseFd end=$endFd）",
            endFd <= baseFd + 10
        )
    }

    @Test
    fun importQueryCloseHundredTimes_noLeak()
    {
        val baseFd = fdCount()
        for (i in 1..LOOPS) {
            assertTrue("第 $i 次打开失败", open())
            assertTrue("第 $i 次 beginMigration 失败", NativeBindings.nativeBeginMigration(handle))
            val res = NativeBindings.nativeSubmitMigrationBatch(handle, emptyBatch(), "0")
            assertTrue("第 $i 次提交空批失败: $res", res.startsWith("ok|"))
            val state = NativeBindings.nativeGetMigrationState(handle)
            assertTrue("第 $i 次查询状态失败: $state", state.startsWith("ok|"))
            close()
        }
        System.gc()
        val endFd = fdCount()
        assertTrue("fd 应回到基线（base=$baseFd end=$endFd）", endFd <= baseFd + 10)
    }

    @Test
    fun reopenAfterClose_persistsSchemaAndUnlocks()
    {
        // 第一次打开并写一次空批（落 checkpoint），然后关闭
        assertTrue("首次打开失败", open())
        assertTrue("beginMigration 失败", NativeBindings.nativeBeginMigration(handle))
        val res = NativeBindings.nativeSubmitMigrationBatch(handle, emptyBatch(), "42")
        assertTrue("写 checkpoint 失败: $res", res.startsWith("ok|"))
        close()

        // 重开：跨进程锁应已释放，库可再次打开，Schema 与 checkpoint 持久
        assertTrue("重开失败（锁未释放或库损坏）", open())
        val self = NativeBindings.nativeRunDatabaseSelfTest(handle)
        assertTrue("重开后自检正常: $self", self.startsWith("ok|status=Ready"))
        val state = NativeBindings.nativeGetMigrationState(handle)
        assertTrue("重开后 checkpoint 持久: $state", state.contains("checkpoint=42"))
        close()
    }
}
