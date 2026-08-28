package com.jitong.im.data.db

import androidx.room.Dao
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.Query
import androidx.room.Transaction

@Dao
interface MessageDao {

    /** @return 新行 rowid；-1 = msg_id 已存在（幂等忽略） */
    @Insert(onConflict = OnConflictStrategy.IGNORE)
    suspend fun insert(entity: MessageEntity): Long

    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun insertFts(entity: MessageFtsEntity)

    /** 同一事务双写 messages 与 messages_fts（独立存储模式的同步策略） */
    @Transaction
    suspend fun insertWithFts(entity: MessageEntity): Boolean {
        val rowId = insert(entity)
        if (rowId == -1L) return false
        if (entity.type == 0 && !entity.content.isNullOrEmpty()) {
            insertFts(MessageFtsEntity(content = entity.content, msgId = entity.msgId))
        }
        return true
    }

    @Query("SELECT * FROM messages WHERE ownerId = :ownerId ORDER BY CASE WHEN seq > 0 THEN seq ELSE 9223372036854775807 END, ts, id")
    suspend fun allForOwner(ownerId: Int): List<MessageEntity>

    @Query("UPDATE messages SET status = :status WHERE ownerId = :ownerId AND msgId = :msgId")
    suspend fun updateStatus(ownerId: Int, msgId: String, status: Int)

    @Query("UPDATE messages SET seq = :seq WHERE ownerId = :ownerId AND msgId = :msgId")
    suspend fun updateSeq(ownerId: Int, msgId: String, seq: Long)

    /** App/连接中断后不存在仍在执行的下载任务；接收文件的 SENDING 均为遗留状态。 */
    @Query("UPDATE messages SET status = 2 WHERE ownerId = :ownerId AND type = 2 AND fromMe = 0 AND status = 0")
    suspend fun recoverInterruptedFileDownloads(ownerId: Int)

    /** 服务端离线补发是权威元数据：校正同 msgId 的旧接收记录，而不是被 INSERT IGNORE 丢弃。 */
    @Query("""UPDATE messages SET fileId = :fileId, fileName = :fileName,
        fileSize = CASE WHEN :fileSize > 0 THEN :fileSize ELSE fileSize END,
        ts = CASE WHEN :ts > 0 THEN :ts ELSE ts END,
        seq = CASE WHEN :seq > 0 THEN :seq ELSE seq END, status = 2
        WHERE ownerId = :ownerId AND msgId = :msgId AND type = 2 AND fromMe = 0""")
    suspend fun reconcileIncomingFile(
        ownerId: Int, msgId: String, fileId: String, fileName: String,
        fileSize: Long, ts: Long, seq: Long,
    )

    @Query("""UPDATE messages SET localPath = :path,
        fileId = CASE WHEN fileId = '' THEN :fileId ELSE fileId END
        WHERE ownerId = :ownerId AND (fileId = :fileId OR msgId = :fileId)""")
    suspend fun updateLocalPath(ownerId: Int, fileId: String, path: String)

    @Query("UPDATE messages SET fileId = :fileId WHERE ownerId = :ownerId AND msgId = :msgId AND type = 2 AND fileId = ''")
    suspend fun repairFileId(ownerId: Int, msgId: String, fileId: String)

    @Query("UPDATE messages SET localPath = :path, fileSize = :size WHERE ownerId = :ownerId AND msgId = :msgId")
    suspend fun updateOutgoingFile(ownerId: Int, msgId: String, path: String, size: Long)

    @Query("UPDATE messages SET fileId=:fileId, contentType=:contentType, sha256=:sha256 WHERE ownerId=:ownerId AND msgId=:msgId")
    suspend fun updateMediaMetadata(ownerId: Int, msgId: String, fileId: String, contentType: String, sha256: String)

    /** FTS 前缀匹配（simple 分词：英文按词、中文整串前缀） */
    @Query(
        """SELECT m.* FROM messages m JOIN messages_fts f ON m.msgId = f.msgId
           WHERE m.ownerId = :ownerId AND messages_fts MATCH :pattern
           ORDER BY m.ts DESC LIMIT 100"""
    )
    suspend fun searchFts(ownerId: Int, pattern: String): List<MessageEntity>

    /** 中文子串兜底（simple 分词对 CJK 只支持整串前缀，LIKE 补"包含"语义） */
    @Query(
        """SELECT * FROM messages WHERE ownerId = :ownerId AND type = 0
           AND content LIKE '%' || :kw || '%' ORDER BY ts DESC LIMIT 100"""
    )
    suspend fun searchLike(ownerId: Int, kw: String): List<MessageEntity>

    /** 单个会话内的全文搜索；conversationId 条件保证不会混入其他聊天。 */
    @Query(
        """SELECT m.* FROM messages m JOIN messages_fts f ON m.msgId = f.msgId
           WHERE m.ownerId = :ownerId AND m.conversationId = :conversationId
           AND messages_fts MATCH :pattern
           ORDER BY m.ts DESC LIMIT 100"""
    )
    suspend fun searchConversationFts(
        ownerId: Int,
        conversationId: Long,
        pattern: String,
    ): List<MessageEntity>

    /** 中文子串兜底；只搜索当前会话的文本消息。 */
    @Query(
        """SELECT * FROM messages WHERE ownerId = :ownerId
           AND conversationId = :conversationId AND type = 0
           AND content LIKE '%' || :kw || '%' ORDER BY ts DESC LIMIT 100"""
    )
    suspend fun searchConversationLike(
        ownerId: Int,
        conversationId: Long,
        kw: String,
    ): List<MessageEntity>
}

@Dao
interface ConversationDao {

    @Query("SELECT * FROM conversations WHERE ownerId = :ownerId")
    suspend fun allForOwner(ownerId: Int): List<ConversationEntity>

    @Insert(onConflict = OnConflictStrategy.IGNORE)
    suspend fun insertIgnore(c: ConversationEntity): Long

    @Query(
        """UPDATE conversations SET lastMsg = :lastMsg, lastTs = :ts, unread = unread + :delta
           WHERE conversationId = :id"""
    )
    suspend fun bump(id: Long, lastMsg: String, ts: Long, delta: Int)

    @Query("UPDATE conversations SET unread = 0 WHERE conversationId = :id")
    suspend fun clearUnread(id: Long)

    /** 消息驱动会话行：不存在则建，存在则刷新最后消息/时间/未读 */
    @Transaction
    suspend fun upsertOnMessage(
        id: Long, ownerId: Int, peerId: Int,
        lastMsg: String, ts: Long, incrUnread: Boolean,
    ) {
        if (insertIgnore(ConversationEntity(id, ownerId, peerId, lastMsg, ts, 0)) == -1L) {
            bump(id, lastMsg, ts, if (incrUnread) 1 else 0)
        }
    }
}
