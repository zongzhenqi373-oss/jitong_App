package com.jitong.im.core

import android.content.Context
import android.content.ContextWrapper
import android.content.SharedPreferences
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.jitong.im.data.ColdStartCutoverRequest
import com.jitong.im.data.ColdStartCutoverRunner
import com.jitong.im.data.crypto.DbKeyManager
import com.jitong.im.data.crypto.DbKeyResult
import com.jitong.im.data.db.AppDatabase
import com.jitong.im.data.db.MessageEntity
import com.jitong.im.data.db.ConversationEntity
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import kotlinx.coroutines.runBlocking
import java.io.File

/** 独立文件根和偏移账号：不读取或改写测试设备上原有账号的消息库/Token/镜像。 */
@RunWith(AndroidJUnit4::class)
class ColdStartCutoverRunnerTest {
    private val owner = 998877
    private val epoch = 99112233L

    @Test fun existingRoomToNativeAndNoWrite() = runBlocking {
        val base = ApplicationProvider.getApplicationContext<Context>()
        assumeTrue(NativeBindings.isLoaded)
        val root = File(base.cacheDir, "cold_cutover_fixture_$owner")
        root.mkdirs()
        val isolated = object : ContextWrapper(base) {
            override fun getApplicationContext(): Context = this
            override fun getFilesDir(): File = File(root, "files").apply { mkdirs() }
            override fun getDatabasePath(name: String): File =
                File(root, "databases/$name").also { it.parentFile?.mkdirs() }
            override fun getSharedPreferences(name: String, mode: Int): SharedPreferences =
                base.getSharedPreferences("fixture_${owner}_$name", mode)
        }
        var key: ByteArray? = null
        try {
            val result = DbKeyManager.getOrCreateRealKey(isolated, owner, "fixture-pass")
            assertTrue(result is DbKeyResult.Success)
            key = (result as DbKeyResult.Success).key
            val room = AppDatabase.get(isolated, owner, key)
            room.messageDao().insertWithFts(MessageEntity(
                ownerId = owner, msgId = "fixture-msg", conversationId = 12345,
                peerId = 42, fromMe = true, type = 0, content = "隔离迁移验证",
                ts = 100, seq = 1, status = 1,
            ))
            room.conversationDao().insertIgnore(ConversationEntity(
                conversationId = 12345, ownerId = owner, peerId = 42,
                lastMsg = "隔离迁移验证", lastTs = 100, unread = 0,
            ))
            AppDatabase.closeCurrent()
            assertTrue(isolated.getSharedPreferences("jitong_cold_start_cutover", 0).edit()
                .putInt("owner", owner).putLong("epoch", epoch).commit())
            val pending = ColdStartCutoverRequest.Pending(owner, epoch)
            val outcome = ColdStartCutoverRunner(isolated) { it == owner }.run(pending)
            assertTrue("迁移失败：$outcome", outcome is ColdStartCutoverRunner.Outcome.Committed)
            val mirror = CutoverMirrorStore(isolated).read()
            assertTrue(mirror is CutoverMirrorStore.ReadResult.Valid)
            assertEquals(2, (mirror as CutoverMirrorStore.ReadResult.Valid).evidence.state)
            assertTrue(File(isolated.filesDir, "native_kernel/native_db/account_$owner.db").isFile)
            val reopened = AppDatabase.get(isolated, owner, key)
            assertTrue("NO_WRITE 后旧库必须拒写", runCatching {
                reopened.messageDao().insertWithFts(MessageEntity(
                    ownerId = owner, msgId = "late", conversationId = 12345,
                    peerId = 42, fromMe = true, type = 0, content = "不能写",
                    ts = 101, seq = 2, status = 1,
                ))
            }.isFailure)
        } finally {
            AppDatabase.closeCurrent()
            key?.fill(0)
            isolated.getSharedPreferences("jitong_cold_start_cutover", 0).edit().clear().commit()
            CutoverMirrorStore(isolated).clearExplicitly()
            root.deleteRecursively()
        }
    }
}
