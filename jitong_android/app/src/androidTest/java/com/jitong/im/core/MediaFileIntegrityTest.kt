package com.jitong.im.core

import android.content.Context
import android.system.Os
import androidx.sqlite.db.SupportSQLiteDatabase
import androidx.sqlite.db.SupportSQLiteOpenHelper
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.jitong.im.core.platform.NativeDbKeyPlatform
import com.jitong.im.data.MigrationExporter
import com.jitong.im.data.crypto.DbKeyManager
import com.jitong.im.data.crypto.DbKeyResult
import net.zetetic.database.sqlcipher.SupportOpenHelperFactory
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.security.MessageDigest

/**
 * P6-T05 媒体文件完整性对账（§24.7「媒体路径逐字段对账，迁移前后媒体文件 inode/hash/数量不变」）。
 *
 * 迁移器只搬媒体**路径字符串**，绝不移动/复制/删除文件。本测试在 Room 夹具里插入指向真实
 * 媒体文件的消息，走完整 Room→Native 迁移，再逐文件对账 hash/inode/数量，作为回归防护。
 */
@RunWith(AndroidJUnit4::class)
class MediaFileIntegrityTest {

    private companion object {
        const val OWNER = 999006
        const val PASS = "media-fixture"
        const val CONV = 777L
        const val PEER = 999L
        const val ROOM_DB = "media_integrity_room.db"
        const val MEDIA_COUNT = 3
    }

    private data class FileFingerprint(val sha256: String, val inode: Long)

    private lateinit var context: Context
    private lateinit var key: ByteArray
    private lateinit var bridge: NativeDbKeyPlatform
    private var handle: Long = 0L
    private val nativeDir: File get() = File(context.filesDir, "media_integrity_native")
    private val mediaDir: File get() = File(context.filesDir, "media_integrity_files")

    @Before
    fun setUp()
    {
        context = ApplicationProvider.getApplicationContext()
        assumeTrue("Native .so 不可用", NativeBindings.isLoaded)
        System.loadLibrary("sqlcipher")
        DbKeyManager.clearLocalDatabase(context, OWNER)
        context.deleteDatabase(ROOM_DB)
        runCatching { context.getDatabasePath("$ROOM_DB-wal").delete() }
        runCatching { context.getDatabasePath("$ROOM_DB-shm").delete() }
        nativeDir.deleteRecursively(); nativeDir.mkdirs()
        mediaDir.deleteRecursively(); mediaDir.mkdirs()

        val r = DbKeyManager.getOrCreateRealKey(context, OWNER, PASS)
        assertTrue("取 realKey 失败: $r", r is DbKeyResult.Success)
        key = (r as DbKeyResult.Success).key
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

    private fun mediaFile(i: Int): File = File(mediaDir, "photo_$i.jpg")

    private fun sha256(f: File): String =
        MessageDigest.getInstance("SHA-256").digest(f.readBytes()).joinToString("") { "%02x".format(it) }

    private fun fingerprint(f: File): FileFingerprint = FileFingerprint(sha256(f), Os.lstat(f.path).st_ino)

    /** 创建 MEDIA_COUNT 个内容互不相同的媒体文件。 */
    private fun createMediaFiles()
    {
        for (i in 0 until MEDIA_COUNT) {
            mediaFile(i).writeBytes(ByteArray(64) { (i * 37 + it).toByte() })
        }
    }

    private fun withRoom(block: (SupportSQLiteDatabase) -> Unit)
    {
        val factory = SupportOpenHelperFactory(key.copyOf())
        val config = SupportSQLiteOpenHelper.Configuration.builder(context)
            .name(ROOM_DB)
            .callback(object : SupportSQLiteOpenHelper.Callback(1) {
                override fun onCreate(db: SupportSQLiteDatabase)
                {
                    db.execSQL("CREATE TABLE messages(id INTEGER PRIMARY KEY AUTOINCREMENT," +
                        "msgId TEXT UNIQUE, conversationId INTEGER, peerId INTEGER, seq INTEGER," +
                        "ts INTEGER, localOrder INTEGER, fromMe INTEGER, type INTEGER," +
                        "content TEXT, status INTEGER, mediaPath TEXT, imgW INTEGER, imgH INTEGER," +
                        "fileId TEXT, fileName TEXT, fileSize INTEGER, contentType TEXT," +
                        "sha256 TEXT, thumbnailFileId TEXT, thumbnailPath TEXT," +
                        "thumbnailSize INTEGER, thumbnailSha256 TEXT, thumbnailW INTEGER," +
                        "thumbnailH INTEGER, largeThumbnailFileId TEXT, largeThumbnailPath TEXT," +
                        "largeThumbnailSize INTEGER, largeThumbnailSha256 TEXT," +
                        "largeThumbnailW INTEGER, largeThumbnailH INTEGER," +
                        "localPath TEXT, transferred INTEGER)")
                    db.execSQL("CREATE VIRTUAL TABLE messages_fts USING fts4(content,pinyin," +
                        "initials,msgId)")
                    db.execSQL("CREATE TABLE conversations(conversationId INTEGER PRIMARY KEY," +
                        "ownerId INTEGER, peerId INTEGER, lastMsg TEXT, lastTs INTEGER, unread INTEGER)")
                }
                override fun onUpgrade(db: SupportSQLiteDatabase, o: Int, n: Int) = Unit
            })
            .build()
        val helper = factory.create(config)
        try { block(helper.writableDatabase) } finally { helper.close() }
    }

    private fun insertMediaMessages(db: SupportSQLiteDatabase)
    {
        for (i in 0 until MEDIA_COUNT) {
            db.execSQL(
                "INSERT INTO messages(msgId,conversationId,peerId,seq,ts,localOrder,fromMe,type," +
                    "content,status,mediaPath,imgW,imgH,fileId,fileName,fileSize,contentType," +
                    "sha256,localPath,transferred) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                arrayOf<Any?>(
                    "mm-$i", CONV, PEER, i + 1, 1700000000L + i, i, 0, 1,
                    "图片$i", 1, mediaFile(i).absolutePath, 640, 480, "fid-$i",
                    "photo_$i.jpg", 64L + i, "image/jpeg", "sha-$i",
                    mediaFile(i).absolutePath, 1L
                )
            )
            db.execSQL(
                "INSERT INTO messages_fts(content,pinyin,initials,msgId) VALUES(?,?,?,?)",
                arrayOf<Any?>("图片$i", "tupian$i", "tp$i", "mm-$i")
            )
        }
    }

    private fun migrateAll(roomDb: SupportSQLiteDatabase): MigrationExporter.Summary
    {
        assertTrue("beginMigration 失败", NativeBindings.nativeBeginMigration(handle))
        var cursor = 0L
        var guard = 0
        while (guard++ < 1000) {
            val batch = MigrationExporter.nextBatch(roomDb, cursor) ?: break
            val res = NativeBindings.nativeSubmitMigrationBatch(handle, batch.encoded, batch.lastId.toString())
            assertTrue("批次提交失败: $res", res.startsWith("ok|"))
            cursor = batch.lastId
            if (batch.count < MigrationExporter.BATCH_SIZE) break
        }
        var convCursor = 0L
        var convGuard = 0
        while (convGuard++ < 1000) {
            val batch = MigrationExporter.nextConversationBatch(roomDb, convCursor) ?: break
            val res = NativeBindings.nativeSubmitConversationBatch(handle, batch.encoded, batch.lastId.toString())
            assertTrue("会话批次提交失败: $res", res.startsWith("ok|"))
            convCursor = batch.lastId
            if (batch.count < MigrationExporter.BATCH_SIZE) break
        }
        return MigrationExporter.summary(roomDb)
    }

    @Test
    fun migrationDoesNotTouchMediaFiles()
    {
        createMediaFiles()
        val before = (0 until MEDIA_COUNT).map { mediaFile(it) to fingerprint(mediaFile(it)) }.toMap()

        withRoom { insertMediaMessages(it) }

        // 打开 Native 库并迁移
        val ok = NativeBindings.nativeOpenAccountDatabase(handle, OWNER, nativeDir.absolutePath, bridge)
        assertTrue("打开 Native 库失败", ok)
        try {
            withRoom { room ->
                val sum = migrateAll(room)
                assertEquals(MEDIA_COUNT.toLong(), sum.messages)
                assertEquals(MEDIA_COUNT.toLong(), sum.fts)
                val fin = NativeBindings.nativeFinishMigration(
                    handle, sum.messages, sum.conversations, sum.minSeq, sum.maxSeq, sum.fts
                )
                assertTrue("对账通过: $fin", fin.startsWith("ok|"))
            }
        } finally {
            NativeBindings.nativeCloseAccountDatabase(handle)
        }

        // 对账媒体文件：数量、inode、hash 均不变
        val after = (0 until MEDIA_COUNT).map { mediaFile(it) to fingerprint(mediaFile(it)) }.toMap()
        assertEquals("媒体文件数量不变", before.size, after.size)
        for ((f, fpBefore) in before) {
            val fpAfter = after[f] ?: error("媒体文件丢失: ${f.path}")
            assertEquals("inode 不变（${f.name}）", fpBefore.inode, fpAfter.inode)
            assertEquals("hash 不变（${f.name}）", fpBefore.sha256, fpAfter.sha256)
        }
    }
}
