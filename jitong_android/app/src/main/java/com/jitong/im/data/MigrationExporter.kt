package com.jitong.im.data

import android.database.Cursor
import androidx.sqlite.db.SupportSQLiteDatabase
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Room → Native 影子迁移的**导出侧**（P6-T05）。
 *
 * 职责：以**游标分页**方式从既有 Room 库读出消息（含 FTS 的 pinyin/initials），
 * 编码成 C++ 侧约定的长度前缀二进制批次，交由 JNI 提交给 Native 库。
 *
 * 约束：
 *   - 只读 Room 库，**不修改、不删除**任何旧数据；
 *   - 分页用稳定游标（主键 id），不用大 OFFSET；
 *   - 媒体只搬元数据字符串（路径/大小/sha256），**不移动/复制/删除文件**；
 *   - 拼音（pinyin/initials）必须搬运，否则会出现"正文存在但拼音搜不到"的倒退。
 */
object MigrationExporter {

    /** 单批条数（背压与内存的折中；过大易 OOM，过小往返开销大）。 */
    const val BATCH_SIZE = 500

    /** wire format 魔数（"JTMG" 小端）与版本号；与 im_db_jni.cpp 的 decodeBatch 严格对应。 */
    const val MAGIC = 0x4A544D47
    const val FORMAT_VERSION = 1

    /** 一次导出的批次：encoded 为可直接交给 JNI 的二进制；lastId 为本批最后一条的主键。 */
    data class Batch(
        val encoded: ByteArray,
        val count: Int,
        val lastId: Long,
    )

    /**
     * 从 [fromIdExclusive] 之后取出最多 [limit] 条消息并编码。
     * @return null 表示没有更多数据
     */
    fun nextBatch(db: SupportSQLiteDatabase, fromIdExclusive: Long, limit: Int = BATCH_SIZE): Batch?
    {
        // 消息与其 FTS 行按 msgId 左连接，一次带走 pinyin/initials + 全部缩略图元数据
        val sql = """
            SELECT m.id, m.msgId, m.conversationId, m.peerId, m.seq, m.ts, m.localOrder,
                   m.fromMe, m.type, m.content, m.status, m.mediaPath, m.imgW, m.imgH,
                   m.fileId, m.fileName, m.fileSize, m.contentType, m.sha256,
                   m.thumbnailFileId, m.thumbnailPath, m.thumbnailSize, m.thumbnailSha256,
                   m.thumbnailW, m.thumbnailH,
                   m.largeThumbnailFileId, m.largeThumbnailPath, m.largeThumbnailSize,
                   m.largeThumbnailSha256, m.largeThumbnailW, m.largeThumbnailH,
                   m.localPath, m.transferred, f.pinyin, f.initials, f.msgId AS ftsMsgId
            FROM messages m
            LEFT JOIN messages_fts f ON f.msgId = m.msgId
            WHERE m.id > ?
            ORDER BY m.id ASC
            LIMIT ?
        """.trimIndent()

        db.query(sql, arrayOf(fromIdExclusive, limit)).use { c ->
            if (!c.moveToFirst()) return null

            val rows = ArrayList<Row>(limit)
            var lastId = fromIdExclusive
            do {
                rows.add(readRow(c))
                lastId = c.getLong(c.getColumnIndexOrThrow("id"))
            } while (c.moveToNext())

            return Batch(encode(rows), rows.size, lastId)
        }
    }

    /** 导出侧统计的对账摘要（用于迁移完成时的全量对账）。 */
    data class Summary(
        val messages: Long,
        val conversations: Long,
        val minSeq: Long,
        val maxSeq: Long,
        val fts: Long,
    )

    fun summary(db: SupportSQLiteDatabase): Summary
    {
        fun scalar(sql: String): Long =
            db.query(sql).use { c -> if (c.moveToFirst()) c.getLong(0) else 0L }

        return Summary(
            messages = scalar("SELECT count(*) FROM messages"),
            // 会话是独立事实流：空会话也可能携带未读、草稿或置顶状态，不能从 messages 推导。
            conversations = scalar("SELECT count(*) FROM conversations"),
            minSeq = scalar("SELECT COALESCE(min(seq),0) FROM messages WHERE seq > 0"),
            maxSeq = scalar("SELECT COALESCE(max(seq),0) FROM messages WHERE seq > 0"),
            fts = scalar("SELECT count(*) FROM messages_fts"),
        )
    }

    // ---------------- 内部：行读取与二进制编码 ----------------

    private data class Row(
        val msgId: String, val conversationId: Long, val peerId: Long, val seq: Long,
        val ts: Long, val localOrder: Long, val fromMe: Int, val type: Int,
        val content: String, val status: Int, val mediaPath: String,
        val imgW: Int, val imgH: Int, val fileId: String, val fileName: String,
        val fileSize: Long, val contentType: String, val sha256: String,
        val thumbnailFileId: String, val thumbnailPath: String, val thumbnailSize: Long,
        val thumbnailSha256: String, val thumbnailW: Int, val thumbnailH: Int,
        val largeThumbnailFileId: String, val largeThumbnailPath: String,
        val largeThumbnailSize: Long, val largeThumbnailSha256: String,
        val largeThumbnailW: Int, val largeThumbnailH: Int,
        val localPath: String, val transferred: Long,
        val pinyin: String, val initials: String,
    )

    private fun readRow(c: Cursor): Row
    {
        fun s(col: String): String = c.getStringOrNull(col) ?: ""
        fun l(col: String): Long = c.getLongOrNull(col) ?: 0L
        fun i(col: String): Int = c.getIntOrNull(col) ?: 0

        return Row(
            msgId = s("msgId"),
            conversationId = l("conversationId"),
            peerId = l("peerId"),
            seq = l("seq"),
            ts = l("ts"),
            localOrder = l("localOrder"),
            fromMe = i("fromMe"),
            type = i("type"),
            content = s("content"),
            status = i("status"),
            mediaPath = s("mediaPath"),
            imgW = i("imgW"),
            imgH = i("imgH"),
            fileId = s("fileId"),
            fileName = s("fileName"),
            fileSize = l("fileSize"),
            contentType = s("contentType"),
            sha256 = s("sha256"),
            thumbnailFileId = s("thumbnailFileId"),
            thumbnailPath = s("thumbnailPath"),
            thumbnailSize = l("thumbnailSize"),
            thumbnailSha256 = s("thumbnailSha256"),
            thumbnailW = i("thumbnailW"),
            thumbnailH = i("thumbnailH"),
            largeThumbnailFileId = s("largeThumbnailFileId"),
            largeThumbnailPath = s("largeThumbnailPath"),
            largeThumbnailSize = l("largeThumbnailSize"),
            largeThumbnailSha256 = s("largeThumbnailSha256"),
            largeThumbnailW = i("largeThumbnailW"),
            largeThumbnailH = i("largeThumbnailH"),
            localPath = s("localPath"),
            transferred = l("transferred"),
            pinyin = s("pinyin"),
            initials = s("initials"),
        )
    }

    /**
     * 编码为小端长度前缀二进制（与 im_db_jni.cpp 的 decodeBatch 严格对应）：
     * magic:i32 + version:i32 + count:i32，随后每条：msgId, conversationId:i64, peerId:i64,
     * seq:i64, ts:i64, localOrder:i64, fromMe:i32, type:i32, content, status:i32, pinyin,
     * initials, mediaPath, imgW:i32, imgH:i32, fileId, fileName, fileSize:i64, contentType,
     * sha256, thumbnailFileId, thumbnailPath, thumbnailSize:i64, thumbnailSha256,
     * thumbnailW:i32, thumbnailH:i32, largeThumbnailFileId, largeThumbnailPath,
     * largeThumbnailSize:i64, largeThumbnailSha256, largeThumbnailW:i32, largeThumbnailH:i32,
     * localPath, transferred:i64
     */
    private fun encode(rows: List<Row>): ByteArray
    {
        val bos = ByteArrayOutputStream()
        val buf = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN)
        fun putI32(v: Int)
        {
            buf.clear(); buf.putInt(v); bos.write(buf.array(), 0, 4)
        }
        fun putI64(v: Long)
        {
            val b8 = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN)
            b8.putLong(v); bos.write(b8.array(), 0, 8)
        }
        fun putStr(s: String)
        {
            val bytes = s.toByteArray(Charsets.UTF_8)
            putI32(bytes.size)
            bos.write(bytes)
        }

        // wire format v1 头：magic + version + count
        putI32(MAGIC)
        putI32(FORMAT_VERSION)
        putI32(rows.size)
        for (r in rows) {
            putStr(r.msgId)
            putI64(r.conversationId); putI64(r.peerId); putI64(r.seq)
            putI64(r.ts); putI64(r.localOrder)
            putI32(r.fromMe); putI32(r.type)
            putStr(r.content); putI32(r.status)
            putStr(r.pinyin); putStr(r.initials)
            putStr(r.mediaPath)
            putI32(r.imgW); putI32(r.imgH)
            putStr(r.fileId); putStr(r.fileName); putI64(r.fileSize)
            putStr(r.contentType); putStr(r.sha256)
            putStr(r.thumbnailFileId); putStr(r.thumbnailPath); putI64(r.thumbnailSize)
            putStr(r.thumbnailSha256); putI32(r.thumbnailW); putI32(r.thumbnailH)
            putStr(r.largeThumbnailFileId); putStr(r.largeThumbnailPath); putI64(r.largeThumbnailSize)
            putStr(r.largeThumbnailSha256); putI32(r.largeThumbnailW); putI32(r.largeThumbnailH)
            putStr(r.localPath); putI64(r.transferred)
        }
        return bos.toByteArray()
    }

    // ---------------- 会话流（R03：独立于消息流的游标与 checkpoint） ----------------

    private data class ConversationRow(
        val conversationId: Long, val peerId: Long,
        val lastMsg: String, val lastTs: Long, val unread: Long,
    )

    /**
     * 从 [fromConversationIdExclusive] 之后取出最多 [limit] 条会话并编码。
     * 会话流保留 unread/lastMsg/lastTs 与空会话（只有 conversation 无 message）。
     * @return null 表示没有更多数据
     */
    fun nextConversationBatch(
        db: SupportSQLiteDatabase,
        fromConversationIdExclusive: Long,
        limit: Int = BATCH_SIZE,
    ): Batch?
    {
        val sql = """
            SELECT conversationId, peerId, lastMsg, lastTs, unread
            FROM conversations
            WHERE conversationId > ?
            ORDER BY conversationId ASC
            LIMIT ?
        """.trimIndent()

        db.query(sql, arrayOf(fromConversationIdExclusive, limit)).use { c ->
            if (!c.moveToFirst()) return null

            val rows = ArrayList<ConversationRow>(limit)
            var lastId = fromConversationIdExclusive
            do {
                rows.add(readConversationRow(c))
                lastId = c.getLong(c.getColumnIndexOrThrow("conversationId"))
            } while (c.moveToNext())

            return Batch(encodeConversations(rows), rows.size, lastId)
        }
    }

    private fun readConversationRow(c: Cursor): ConversationRow
    {
        fun s(col: String): String = c.getStringOrNull(col) ?: ""
        fun l(col: String): Long = c.getLongOrNull(col) ?: 0L

        return ConversationRow(
            conversationId = l("conversationId"),
            peerId = l("peerId"),
            lastMsg = s("lastMsg"),
            lastTs = l("lastTs"),
            unread = l("unread"),
        )
    }

    /** 编码会话批次：magic + version + count，随后每条 conversationId/peerId/lastMsg/lastTs/unread。 */
    private fun encodeConversations(rows: List<ConversationRow>): ByteArray
    {
        val bos = ByteArrayOutputStream()
        val buf = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN)
        fun putI32(v: Int) { buf.clear(); buf.putInt(v); bos.write(buf.array(), 0, 4) }
        fun putI64(v: Long)
        {
            val b8 = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN)
            b8.putLong(v); bos.write(b8.array(), 0, 8)
        }
        fun putStr(s: String)
        {
            val bytes = s.toByteArray(Charsets.UTF_8)
            putI32(bytes.size)
            bos.write(bytes)
        }

        putI32(MAGIC)
        putI32(FORMAT_VERSION)
        putI32(rows.size)
        for (r in rows) {
            putI64(r.conversationId); putI64(r.peerId)
            putStr(r.lastMsg)
            putI64(r.lastTs); putI64(r.unread)
        }
        return bos.toByteArray()
    }

    private fun Cursor.getStringOrNull(col: String): String? =
        getColumnIndex(col).takeIf { it >= 0 }?.let { if (isNull(it)) null else getString(it) }

    private fun Cursor.getLongOrNull(col: String): Long? =
        getColumnIndex(col).takeIf { it >= 0 }?.let { if (isNull(it)) null else getLong(it) }

    private fun Cursor.getIntOrNull(col: String): Int? =
        getColumnIndex(col).takeIf { it >= 0 }?.let { if (isNull(it)) null else getInt(it) }
}
