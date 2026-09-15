package com.jitong.im.core

import android.content.Context
import androidx.sqlite.db.SupportSQLiteDatabase
import androidx.sqlite.db.SupportSQLiteOpenHelper
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.jitong.im.core.platform.NativeDbKeyPlatform
import com.jitong.im.data.MigrationExporter
import com.jitong.im.data.crypto.DbKeyManager
import com.jitong.im.data.crypto.DbKeyResult
import net.zetetic.database.sqlcipher.SupportOpenHelperFactory
import com.tencent.mmkv.MMKV
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * P6-T05 影子迁移端到端验证（Room → Native）。
 *
 * 用真实 Room（sqlcipher-android）库作为导出源，经 [MigrationExporter] 分页编码，
 * 由 JNI 提交给 Native 库（CipherDatabase + SchemaManager + DbCommandQueue）。
 * 走 **S6 正式路径**：数据库由 NativeSdkHandle 持有，key 经 [NativeDbKeyPlatform]
 * 从 DbKeyManager 解出（不再直接传 key），验证：迁移结果、幂等、对账、失败不误标完成。
 *
 * 说明：本测试用**测试夹具库**，不触碰真实聊天库；10 万条与强杀恢复在桌面单测与
 * 后续压力验证中覆盖（此处用较小数据集保证稳定与快速）。
 *
 * 运行：
 * ```bash
 * ./gradlew :app:connectedDebugAndroidTest -x lint \
 *   -Pandroid.testInstrumentationRunnerArguments.class=com.jitong.im.core.ShadowMigrationTest
 * ```
 */
@RunWith(AndroidJUnit4::class)
class ShadowMigrationTest {

    private companion object {
        const val OWNER = 999004
        const val PASS = "migration-fixture"
        const val CONV = 555L
        const val PEER = 888L
        const val ROOM_DB = "shadow_migration_room.db"
    }

    private lateinit var context: Context
    private lateinit var key: ByteArray
    private lateinit var bridge: NativeDbKeyPlatform
    private var handle: Long = 0L
    private val nativeDir: File get() = File(context.filesDir, "shadow_migration_native")

    @Before
    fun setUp()
    {
        context = ApplicationProvider.getApplicationContext()
        MMKV.initialize(context)
        assumeTrue("Native .so 不可用", NativeBindings.isLoaded)
        System.loadLibrary("sqlcipher")
        // 清理上一轮遗留（含夹具库）：
        // 注意顺序——先删 key blob 会令下一次取 key 重新生成随机 realKey，
        // 若遗留的加密夹具库是用**旧 key** 写的，再用新 key 打开会报 code 26。
        // 因此夹具库必须与 key 一起清理。
        DbKeyManager.clearLocalDatabase(context, OWNER)
        context.deleteDatabase(ROOM_DB)
        runCatching { context.getDatabasePath("$ROOM_DB-wal").delete() }
        runCatching { context.getDatabasePath("$ROOM_DB-shm").delete() }
        nativeDir.deleteRecursively()
        nativeDir.mkdirs()

        val r = DbKeyManager.getOrCreateRealKey(context, OWNER, PASS)
        assertTrue("取 realKey 失败: $r", r is DbKeyResult.Success)
        key = (r as DbKeyResult.Success).key

        // S6 正式路径：key 经平台密钥桥取，数据库由句柄持有。
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

    private fun insertRoomMessages(db: SupportSQLiteDatabase, count: Int)
    {
        db.execSQL(
            "INSERT OR IGNORE INTO conversations(conversationId,ownerId,peerId,lastMsg,lastTs,unread) " +
                "VALUES(?,?,?,?,?,?)",
            arrayOf<Any?>(CONV, OWNER, PEER, "", 0L, 0L)
        )
        for (i in 1..count) {
            db.execSQL(
                "INSERT INTO messages(msgId,conversationId,peerId,seq,ts,localOrder,fromMe,type," +
                    "content,status,mediaPath,imgW,imgH,fileId,fileName,fileSize,contentType," +
                    "sha256,localPath,transferred) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                arrayOf<Any?>(
                    "rm-$i", CONV, PEER, i, 1700000000L + i, i, i % 2, 0,
                    "内容$i", 1, "/media/$i.jpg", 320, 240, "fid-$i", "file$i.jpg",
                    1024L + i, "image/jpeg", "sha-$i", "/local/$i.bin", 1L
                )
            )
            db.execSQL(
                "INSERT INTO messages_fts(content,pinyin,initials,msgId) VALUES(?,?,?,?)",
                arrayOf<Any?>("内容$i", "neirong$i", "nr$i", "rm-$i")
            )
        }
    }

    private fun openNative()
    {
        val ok = NativeBindings.nativeOpenAccountDatabase(
            handle, OWNER, nativeDir.absolutePath, bridge
        )
        assertTrue("打开 Native 库失败", ok)
    }

    private fun closeNative()
    {
        NativeBindings.nativeCloseAccountDatabase(handle)
    }

    private fun migrateAll(roomDb: SupportSQLiteDatabase): MigrationExporter.Summary
    {
        // F05：先进入 shadow_import 状态，否则 submit 会被三态校验拒绝
        assertTrue("beginMigration 失败", NativeBindings.nativeBeginMigration(handle))

        // 消息流
        var cursor = 0L
        var guard = 0
        while (guard++ < 1000) {
            val batch = MigrationExporter.nextBatch(roomDb, cursor) ?: break
            val res = NativeBindings.nativeSubmitMigrationBatch(
                handle, batch.encoded, batch.lastId.toString()
            )
            assertTrue("批次提交失败: $res", res.startsWith("ok|"))
            cursor = batch.lastId
            if (batch.count < MigrationExporter.BATCH_SIZE) break
        }

        // 会话流（R03：独立游标与 checkpoint；保留 unread/lastMsg/lastTs 与空会话）
        var convCursor = 0L
        var convGuard = 0
        while (convGuard++ < 1000) {
            val batch = MigrationExporter.nextConversationBatch(roomDb, convCursor) ?: break
            val res = NativeBindings.nativeSubmitConversationBatch(
                handle, batch.encoded, batch.lastId.toString()
            )
            assertTrue("会话批次提交失败: $res", res.startsWith("ok|"))
            convCursor = batch.lastId
            if (batch.count < MigrationExporter.BATCH_SIZE) break
        }
        return MigrationExporter.summary(roomDb)
    }

    @Test
    fun emptyRoom_migratesToZeroMessageNativeDb()
    {
        withRoom { /* 空库 */ }
        openNative()
        try {
            // 数据库自检（正式接口）：cipher 版本 / schema 版本 / 迁移状态
            val self = NativeBindings.nativeRunDatabaseSelfTest(handle)
            assertTrue("自检应为 ok: $self", self.startsWith("ok|status=Ready|cipher="))
            assertTrue("自检应报 schema 版本: $self", self.contains("|schema="))

            withRoom { room ->
                val sum = migrateAll(room)
                assertEquals("导出侧应为 0 条", 0L, sum.messages)
                val state = NativeBindings.nativeGetMigrationState(handle)
                assertTrue("应为 ok: $state", state.startsWith("ok|"))
                assertTrue("0 条时消息数为 0: $state", state.contains("messages=0"))
            }
        } finally {
            closeNative()
        }
    }

    @Test
    fun migrateMessages_thenVerifyAndIdempotent()
    {
        withRoom { insertRoomMessages(it, 25) }

        openNative()
        try {
            withRoom { room ->
                val sum = migrateAll(room)
                assertEquals(25L, sum.messages)
                assertEquals(25L, sum.fts)
                assertEquals(1L, sum.minSeq)
                assertEquals(25L, sum.maxSeq)

                // 完成：全量对账通过才写完成标记
                val fin = NativeBindings.nativeFinishMigration(
                    handle, sum.messages, sum.conversations, sum.minSeq, sum.maxSeq, sum.fts
                )
                assertTrue("finish 应成功: $fin", fin.startsWith("ok|"))

                val state = NativeBindings.nativeGetMigrationState(handle)
                assertTrue("应已完成: $state", state.contains("completed=1"))
                assertTrue("消息数应为 25: $state", state.contains("messages=25"))
                assertTrue("FTS 应与消息数一致: $state", state.contains("fts=25"))

                // F05：verified 后重复迁移被拒（保护已完成数据不被覆盖）
                assertTrue("verified 后 beginMigration 被拒",
                    !NativeBindings.nativeBeginMigration(handle))
                val state2 = NativeBindings.nativeGetMigrationState(handle)
                assertTrue("消息数仍为 25: $state2", state2.contains("messages=25"))
                assertTrue("FTS 仍为 25: $state2", state2.contains("fts=25"))
            }
        } finally {
            closeNative()
        }
    }

    @Test
    fun nativeSearch_usesPinyinInitialContract()
    {
        withRoom { insertRoomMessages(it, 5) }
        openNative()
        try {
            withRoom { room -> migrateAll(room) }
            fun count(keyword: String): Int {
                val raw = NativeBindings.nativeSearchMessages(handle, CONV, keyword, 100)
                assertTrue("搜索结果不得为空: $keyword", raw != null)
                val input = ByteBuffer.wrap(raw!!).order(ByteOrder.LITTLE_ENDIAN)
                assertEquals("JTSR magic", 0x4A545352, input.int)
                assertEquals("搜索协议版本", 1, input.int)
                return input.int
            }
            assertEquals("n 只按首字母命中", 5, count("n"))
            assertEquals("不存在的首字母不得误命中全拼", 0, count("z"))
            assertEquals("全拼前缀可命中", 5, count("neirong"))
        } finally {
            closeNative()
        }
    }

    @Test
    fun sdkHistory_decodesStableCursorAndAllMediaFields()
    {
        withRoom { insertRoomMessages(it, 5) }
        openNative()
        withRoom { room -> migrateAll(room) }
        closeNative()

        val sdk = JitongSdk()
        assertTrue("SDK start", sdk.start("im.example.com"))
        try {
            assertTrue("SDK 打开同一 Native 库", sdk.openAccountDatabase(OWNER, nativeDir.absolutePath, bridge))
            assertTrue("创建 Runtime", sdk.createRuntime(OWNER))
            assertTrue("启动 Runtime", sdk.startRuntime() > 0)
            val conversations=sdk.loadConversations()
            assertEquals("会话快照由 Native 解码",1,conversations?.size)
            assertEquals(CONV,conversations!!.first().conversationId)
            assertEquals(PEER,conversations.first().peerId)
            val first = sdk.loadHistory(CONV, limit = 2)
            assertTrue("历史首页可解码", first != null)
            assertEquals(listOf("rm-5", "rm-4"), first!!.messages.map { it.msgId })
            assertTrue("首页还有下一页", first.hasMore)
            assertEquals("原始媒体字段完整", "fid-5", first.messages.first().media.fileId)
            val second = sdk.loadHistory(CONV, limit = 2, cursor = first.nextCursor)
            assertEquals(listOf("rm-3", "rm-2"), second!!.messages.map { it.msgId })
            assertTrue("两页不重复", first.messages.map { it.msgId }.intersect(
                second.messages.map { it.msgId }.toSet()).isEmpty())
            sdk.stopRuntime()
            assertTrue("stop 后数据库仍可查询", sdk.loadHistory(CONV, limit = 1) != null)
            assertTrue("stop 后 Runtime 可带同一数据库重启", sdk.startRuntime() > 0)
        } finally {
            sdk.closeAccountDatabase()
            sdk.stop()
        }
    }

    @Test
    fun runtimeSendText_commitsBeforeNetworkAndIsSearchable()
    {
        openNative()
        closeNative()
        val sdk=JitongSdk()
        assertTrue("SDK start",sdk.start("im.example.com"))
        try {
            assertTrue("打开 Native DB",sdk.openAccountDatabase(OWNER,nativeDir.absolutePath,bridge))
            assertTrue("创建 Runtime",sdk.createRuntime(OWNER))
            assertTrue("启动 Runtime",sdk.startRuntime()>0)
            val receipt=sdk.sendText(CONV,PEER,"哈哈，你好")
            assertTrue("断网时仍先接受并落本地 Outbox: ${receipt.error}",receipt.accepted)
            assertTrue("msgId/operationId/localOrder 完整",
                receipt.msgId.isNotBlank()&&receipt.operationId.isNotBlank()&&receipt.localOrder>0)
            val page=sdk.loadHistory(CONV,10)
            assertEquals(receipt.msgId,page!!.messages.first().msgId)
            assertEquals(0,page.messages.first().status)
            assertTrue("未连接不谎报已发",sdk.flushOutbox()==0)
            assertEquals(receipt.msgId,sdk.searchMessages("n",CONV).first().msgId)
            assertTrue("消息提交主动更新 Messages StateFlow",
                (sdk.dataVersions.value[JitongSdk.DataDomain.Messages]?:0)>0)
            assertTrue("会话摘要主动更新 Conversations StateFlow",
                (sdk.dataVersions.value[JitongSdk.DataDomain.Conversations]?:0)>0)
            assertEquals("哈哈，你好",sdk.loadConversations()!!.first().lastMessage)
            val version=sdk.consumeInvalidation(JitongSdk.DataDomain.Messages)
            assertTrue("本地提交后发布消息失效版本",version>0)
            assertEquals("失效通知允许合并且只消费一次",0,
                sdk.consumeInvalidation(JitongSdk.DataDomain.Messages))
            assertEquals("断网等待 ACK 时操作尚无终态",null,
                sdk.consumeOperation(receipt.operationId))
        } finally { sdk.stop() }
    }

    @Test
    fun conversations_preserveUnreadAndEmptyConversation()
    {
        withRoom { room ->
            insertRoomMessages(room, 3)
            // 一个空会话（无消息）+ 一个非零 unread 会话（F04：会话元数据不丢）
            room.execSQL(
                "INSERT INTO conversations(conversationId,ownerId,peerId,lastMsg,lastTs,unread) " +
                    "VALUES(?,?,?,?,?,?)",
                arrayOf<Any?>(999L, OWNER, 777L, "空会话", 0L, 0L)
            )
            room.execSQL(
                "UPDATE conversations SET unread=5 WHERE conversationId=?",
                arrayOf<Any?>(CONV)
            )
        }
        openNative()
        try {
            withRoom { room ->
                migrateAll(room)
                // 会话数 = 消息派生的 CONV + 空会话 999 = 2
                val fin = NativeBindings.nativeFinishMigration(handle, 3L, 2L, 1L, 3L, 3L)
                assertTrue("finish 应成功: $fin", fin.startsWith("ok|"))
                val state = NativeBindings.nativeGetMigrationState(handle)
                assertTrue("会话数应为 2: $state", state.contains("conversations=2"))
            }
        } finally {
            closeNative()
        }
    }

    @Test
    fun wrongExpectation_doesNotMarkCompleted()
    {
        withRoom { insertRoomMessages(it, 5) }
        openNative()
        try {
            withRoom { room ->
                migrateAll(room)
                // 故意给一个偏大的期望 → 对账失败，不得写完成标记
                val fin = NativeBindings.nativeFinishMigration(handle, 999, 1, 1, 5, 5)
                assertTrue("对账不一致应失败: $fin", fin.startsWith("err|VerifyFailed|"))
                val state = NativeBindings.nativeGetMigrationState(handle)
                assertTrue("不得标记完成: $state", state.contains("completed=0"))
            }
        } finally {
            closeNative()
        }
    }
}
