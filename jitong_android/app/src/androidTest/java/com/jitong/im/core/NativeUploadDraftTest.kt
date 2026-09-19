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
 * 分片上传草稿 JNI 真机验证（无服务端语义）：
 * 登记（含 C++ 侧流式 SHA-256/大小）、进度查询、无服务端时 pump 失败落 Failed、
 * 取消写终态、resume 不触碰已取消草稿、取消后不可再 pump。
 * 真实服务端分片/秒传/恢复由桌面 test_chunked_upload + test_media_resume_e2e 覆盖。
 */
@RunWith(AndroidJUnit4::class)
class NativeUploadDraftTest {
    private companion object {
        const val OWNER = 999007
        const val PASS = "upload-draft-fixture"
    }

    private lateinit var context: Context
    private lateinit var bridge: NativeDbKeyPlatform
    private var handle: Long = 0L
    private val nativeDir: File get() = File(context.filesDir, "upload_draft_native")
    private val srcFile: File get() = File(nativeDir, "clip.bin")

    @Before
    fun setUp() {
        context = ApplicationProvider.getApplicationContext()
        MMKV.initialize(context)
        assumeTrue("Native .so 不可用", NativeBindings.isLoaded)
        System.loadLibrary("sqlcipher")
        DbKeyManager.clearLocalDatabase(context, OWNER)
        nativeDir.deleteRecursively()
        nativeDir.mkdirs()
        val r = DbKeyManager.getOrCreateRealKey(context, OWNER, PASS)
        assertTrue("取 realKey 失败: $r", r is DbKeyResult.Success)
        bridge = NativeDbKeyPlatform(context).also { it.setPassHash(PASS) }
        handle = NativeBindings.nativeCreate("im.example.com")
        assertTrue(handle != 0L)
        assertTrue("打开账号库失败",
            NativeBindings.nativeOpenAccountDatabase(handle, OWNER, nativeDir.absolutePath, bridge))
        // 2.5MiB 测试文件
        srcFile.outputStream().use { out ->
            val block = ByteArray(65536) { (it % 251).toByte() }
            repeat(40) { out.write(block) }
        }
    }

    @After
    fun tearDown() {
        if (handle != 0L) {
            NativeBindings.nativeCloseAccountDatabase(handle)
            NativeBindings.nativeDestroy(handle)
            handle = 0L
        }
        nativeDir.deleteRecursively()
    }

    private fun state(msgId: String) = NativeBindings.nativeMediaUploadState(handle, msgId)

    @Test
    fun draftLifecycleWithoutServer() {
        // 登记：C++ 侧流式计算大小与摘要
        assertEquals("ok", NativeBindings.nativeMediaEnqueueUpload(
            handle, "up-1", 100, 200, 0, srcFile.absolutePath, "clip.bin", "application/octet-stream", 0, 0))
        var s = state("up-1")
        assertTrue("登记后 Pending: $s", s.startsWith("0,"))

        // 重复登记幂等：保留状态
        assertEquals("ok", NativeBindings.nativeMediaEnqueueUpload(
            handle, "up-1", 100, 200, 0, srcFile.absolutePath, "clip.bin", "application/octet-stream", 0, 0))
        assertTrue(state("up-1").startsWith("0,"))

        // 无服务端：pump 失败 → Failed(5)，草稿保留
        val pumped = NativeBindings.nativeMediaPumpUpload(handle, "up-1")
        assertTrue("无服务端应失败: $pumped", pumped.startsWith("err"))
        s = state("up-1")
        assertTrue("失败后 Failed(5): $s", s.startsWith("5,"))

        // 取消：终态 Cancelled(4)
        assertTrue(NativeBindings.nativeMediaCancelUpload(handle, "up-1"))
        s = state("up-1")
        assertTrue("取消后 Cancelled(4): $s", s.startsWith("4,"))

        // 取消后不可再 pump / 不被恢复扫描触碰
        val again = NativeBindings.nativeMediaPumpUpload(handle, "up-1")
        assertTrue("终态不可再推进: $again", again.startsWith("err"))
        assertEquals(0, NativeBindings.nativeMediaResumeUploads(handle))
        assertTrue("保持 Cancelled: ${state("up-1")}", state("up-1").startsWith("4,"))
    }

    @Test
    fun enqueueRejectsBadInput() {
        assertTrue(NativeBindings.nativeMediaEnqueueUpload(
            handle, "", 100, 200, 0, srcFile.absolutePath, "a", "application/octet-stream", 0, 0)
            .startsWith("err"))
        assertTrue(NativeBindings.nativeMediaEnqueueUpload(
            handle, "up-x", 100, 200, 0, "/nonexistent/no.bin", "a", "application/octet-stream", 0, 0)
            .startsWith("err"))
    }
}
