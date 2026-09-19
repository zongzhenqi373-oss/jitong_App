package com.jitong.im.core

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.jitong.im.core.platform.NativeDbKeyPlatform
import com.jitong.im.data.crypto.DbKeyManager
import com.jitong.im.data.crypto.DbKeyResult
import com.tencent.mmkv.MMKV
import java.io.File
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * cutover DIRTY 镜像闭环（P7-G8）：
 * Native transaction hook 在首次业务事务中原子推进 NO_WRITE→DIRTY 后，
 * 必须经 JNI 回调把带 MAC 的 KV 镜像同步到同一快照（含 updatedAt），
 * 使冷启动双证据校验能进入 Native 而不是 Repair。
 *
 * 同时验证 fail-close：journal=PREPARED（Room fence 生效）时业务写被拒绝。
 */
@RunWith(AndroidJUnit4::class)
class CutoverDirtyMirrorTest {
    private companion object {
        const val OWNER = 999006
        const val PASS = "dirty-mirror-fixture"
        const val EPOCH = 1L
        const val HIGH_WATER = 11L
        const val SCHEMA = 10
    }

    private lateinit var context: Context
    private lateinit var bridge: NativeDbKeyPlatform
    private lateinit var mirror: CutoverMirrorStore
    private var handle: Long = 0L
    private val nativeDir: File get() = File(context.filesDir, "dirty_mirror_native")

    @Before
    fun setUp() {
        context = ApplicationProvider.getApplicationContext()
        MMKV.initialize(context)
        assumeTrue("Native .so 不可用", NativeBindings.isLoaded)
        nativeDir.deleteRecursively()
        nativeDir.mkdirs()
        val r = DbKeyManager.getOrCreateRealKey(context, OWNER, PASS)
        assertTrue("取 realKey 失败: $r", r is DbKeyResult.Success)
        bridge = NativeDbKeyPlatform(context).also { it.setPassHash(PASS) }
        mirror = CutoverMirrorStore(context)
        mirror.clearExplicitly()
        handle = NativeBindings.nativeCreate("im.example.com")
        assertTrue("创建内核句柄失败", handle != 0L)
        NativeBindings.cutoverDirtyHandler = { evidence ->
            assertTrue("镜像写入失败", mirror.write(evidence))
        }
    }

    @After
    fun tearDown() {
        NativeBindings.cutoverDirtyHandler = null
        if (handle != 0L) {
            NativeBindings.nativeCloseAccountDatabase(handle)
            NativeBindings.nativeDestroy(handle)
            handle = 0L
        }
        mirror.clearExplicitly()
        nativeDir.deleteRecursively()
    }

    private fun advance(expected: Int, target: Int, updatedAt: Long) {
        val r = NativeBindings.nativeAdvanceCutoverJournal(
            handle, EPOCH, expected, target, HIGH_WATER, SCHEMA, "kid", "sum", updatedAt,
        )
        assertEquals("advance $expected→$target 失败: $r", "ok", r)
    }

    @Test
    fun firstBusinessWriteAdvancesDirtyAndSyncsMirror() {
        assertTrue(NativeBindings.nativeOpenAccountDatabase(
            handle, OWNER, nativeDir.absolutePath, bridge))
        assertTrue(NativeBindings.nativeCreateRuntime(handle, OWNER))
        assertTrue("Runtime 启动失败", NativeBindings.nativeStartRuntime(handle) > 0)

        // PREPARED：Room fence 生效期，Native 业务写必须 fail-close。
        advance(-1, 0, 1000) // LEGACY_ACTIVE
        advance(0, 1, 1001)  // PREPARED
        val blocked = NativeBindings.nativeRuntimeSendText(handle, 555, 888, "blocked", "blocked", "b")
        assertTrue("PREPARED 期业务写应被拒绝: $blocked", blocked.startsWith("err"))
        assertTrue("PREPARED 期不得写镜像", mirror.read() is CutoverMirrorStore.ReadResult.Missing)

        // NO_WRITE：首次业务写原子推进 DIRTY，并同步镜像。
        advance(1, 2, 1002)
        val sent = NativeBindings.nativeRuntimeSendText(handle, 555, 888, "hello", "hello", "h")
        assertTrue("NO_WRITE 后业务写应成功: $sent", sent.startsWith("ok|"))

        val db = CutoverRecoverySelector.parseDb(NativeBindings.nativeGetCutoverJournal(handle))
        assertTrue("journal 查询失败", db?.present == true)
        assertEquals(3, db!!.state)

        val read = mirror.read()
        assertTrue("镜像应已写入: $read", read is CutoverMirrorStore.ReadResult.Valid)
        val ev = (read as CutoverMirrorStore.ReadResult.Valid).evidence
        assertEquals(OWNER.toLong(), ev.ownerId)
        assertEquals(EPOCH, ev.epoch)
        assertEquals(3, ev.state)
        // 镜像必须与 DB journal 完全一致，否则冷启动 resolve() 会走 Repair。
        assertEquals(db.highWater, ev.highWater)
        assertEquals(db.schemaVersion, ev.schemaVersion)
        assertEquals(db.keyId, ev.keyId)
        assertEquals(db.summary, ev.summary)
        assertEquals(db.updatedAt, ev.updatedAt)

        // 双证据一致 → 冷启动决策为 Native（而非 Repair）。
        val action = CutoverRecoverySelector.resolve(
            OWNER.toLong(), db, read, nativeExists = true, nativeLoadable = true,
        )
        assertEquals(CutoverRecoverySelector.Action.Native, action)

        // 第二次业务写不再改变镜像（不重复推进）。
        val sent2 = NativeBindings.nativeRuntimeSendText(handle, 555, 888, "again", "again", "a")
        assertTrue(sent2.startsWith("ok|"))
        val ev2 = (mirror.read() as CutoverMirrorStore.ReadResult.Valid).evidence
        assertEquals(ev.updatedAt, ev2.updatedAt)
    }
}
