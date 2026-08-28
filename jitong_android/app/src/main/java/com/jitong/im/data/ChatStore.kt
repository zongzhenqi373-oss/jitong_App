package com.jitong.im.data

import android.content.Context
import com.jitong.im.data.db.AppDatabase
import com.jitong.im.data.db.ConversationEntity
import com.jitong.im.data.db.MessageEntity
import com.jitong.im.ui.ChatMessage
import com.jitong.im.ui.MsgKind
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/**
 * 消息仓储：ViewModel 内存态 ↔ Room 持久态的桥。
 * 图片字节写应用私有目录 filesDir/img/，库里只存路径（media_path 口径与服务端一致）。
 * key：SQLCipher 真实数据库密钥，由 DbKeyManager 在登录成功后派生/解出，调用方负责传入。
 */
class ChatStore(context: Context, ownerId: Int, key: ByteArray) {

    private val appContext = context.applicationContext
    private val db = AppDatabase.get(appContext, ownerId, key)

    fun close() = AppDatabase.closeCurrent()

    /** 会话 id：小 ID 放高 32 位、大 ID 放低 32 位，与服务端 makeConversationId 完全一致。 */
    fun conversationId(a: Int, b: Int): Long {
        require(a > 0 && b > 0) { "用户 ID 必须为正数" }
        val lo = minOf(a, b).toLong() and 0xFFFF_FFFFL
        val hi = maxOf(a, b).toLong() and 0xFFFF_FFFFL
        return (lo shl 32) or hi
    }

    /** 消息落库（含 FTS 同事务双写 + 会话行刷新）。重复 msg_id 幂等忽略。
     *  bumpConversation=false：只落消息，不刷新会话预览行（漫游历史是旧消息，避免把预览回退到更早） */
    suspend fun save(ownerId: Int, m: ChatMessage, incrUnread: Boolean, bumpConversation: Boolean = true) =
        withContext(Dispatchers.IO) {
            var mediaPath: String? = null
            if (m.kind == MsgKind.IMAGE && m.imageBytes != null) {
                mediaPath = writeImageFile(m.msgId, m.imageBytes)
            }
            val inserted = db.messageDao().insertWithFts(m.toEntity(ownerId, mediaPath))
            if (!inserted && m.kind == MsgKind.FILE && !m.fromMe) {
                reconcileIncomingFile(ownerId, m)
            }
            if (inserted && bumpConversation) {
                val lastMsg = when (m.kind) {
                    MsgKind.IMAGE -> "[图片]"
                    MsgKind.FILE -> "[文件] ${m.fileName}"
                    else -> m.text
                }
                db.conversationDao().upsertOnMessage(
                    conversationId(ownerId, m.peerId), ownerId, m.peerId,
                    lastMsg, m.ts, incrUnread && !m.fromMe,
                )
            }
        }

    suspend fun updateStatus(ownerId: Int, msgId: String, status: ChatMessage.Status) =
        withContext(Dispatchers.IO) {
            db.messageDao().updateStatus(ownerId, msgId, status.toDb())
        }

    /** 回执带回服务端 seq 后，校正本端消息的会话序列号 */
    suspend fun updateSeq(ownerId: Int, msgId: String, seq: Long) =
        withContext(Dispatchers.IO) {
            db.messageDao().updateSeq(ownerId, msgId, seq)
        }

    /** 用服务端补发卡片修正本地同 msgId 的遗留文件记录。 */
    suspend fun reconcileIncomingFile(ownerId: Int, m: ChatMessage) =
        withContext(Dispatchers.IO) {
            db.messageDao().reconcileIncomingFile(
                ownerId, m.msgId, m.fileId, m.fileName, m.fileSize, m.ts, m.seq,
            )
        }

    /** 文件下载完成后写入最终本地路径 */
    suspend fun updateFileLocalPath(ownerId: Int, fileId: String, path: String) =
        withContext(Dispatchers.IO) { db.messageDao().updateLocalPath(ownerId, fileId, path) }

    suspend fun repairFileId(ownerId: Int, msgId: String, fileId: String) =
        withContext(Dispatchers.IO) { db.messageDao().repairFileId(ownerId, msgId, fileId) }

    /** SAF 的 SIZE 元数据可能不准确；复制后以本地文件实际长度回写发送卡片。 */
    suspend fun updateOutgoingFile(ownerId: Int, msgId: String, path: String, size: Long) =
        withContext(Dispatchers.IO) { db.messageDao().updateOutgoingFile(ownerId, msgId, path, size) }

    suspend fun updateMediaMetadata(ownerId: Int, msgId: String, fileId: String, contentType: String, sha256: String) =
        withContext(Dispatchers.IO) {
            db.messageDao().updateMediaMetadata(ownerId, msgId, fileId, contentType, sha256)
        }

    suspend fun clearUnread(ownerId: Int, peerId: Int) = withContext(Dispatchers.IO) {
        db.conversationDao().clearUnread(conversationId(ownerId, peerId))
    }

    /** 登录后装载：历史消息（含图片字节读回）+ 会话行 */
    suspend fun hydrate(ownerId: Int): Pair<Map<Int, List<ChatMessage>>, Map<Int, ConversationEntity>> =
        withContext(Dispatchers.IO) {
            // 下载任务只存在于当前进程内；重登/重启后遗留的接收方 SENDING 不可能仍在运行。
            db.messageDao().recoverInterruptedFileDownloads(ownerId)
            val messages = db.messageDao().allForOwner(ownerId)
                .map { it.toChatMessage() }
                .groupBy { it.peerId }
            val convs = db.conversationDao().allForOwner(ownerId).associateBy { it.peerId }
            messages to convs
        }

    /** FTS 前缀 + LIKE 子串双路搜索，按 msgId 去重、时间倒序 */
    suspend fun search(ownerId: Int, kw: String): List<MessageEntity> = withContext(Dispatchers.IO) {
        val pattern = "\"" + kw.replace("\"", "\"\"") + "\"*"
        val fts = runCatching { db.messageDao().searchFts(ownerId, pattern) }.getOrDefault(emptyList())
        val like = db.messageDao().searchLike(ownerId, kw)
        (fts + like).distinctBy { it.msgId }.sortedByDescending { it.ts }
    }

    /** 当前单聊内搜索。结果仍按时间倒序，FTS 不可用时 LIKE 查询仍可工作。 */
    suspend fun searchConversation(ownerId: Int, peerId: Int, kw: String): List<MessageEntity> =
        withContext(Dispatchers.IO) {
            val conversationId = conversationId(ownerId, peerId)
            val pattern = "\"" + kw.replace("\"", "\"\"") + "\"*"
            val fts = runCatching {
                db.messageDao().searchConversationFts(ownerId, conversationId, pattern)
            }.getOrDefault(emptyList())
            val like = db.messageDao().searchConversationLike(ownerId, conversationId, kw)
            (fts + like).distinctBy { it.msgId }.sortedByDescending { it.ts }
        }

    // ---------------- 内部 ----------------

    private fun writeImageFile(msgId: String, bytes: ByteArray): String {
        val dir = File(appContext.filesDir, "img").apply { mkdirs() }
        val file = File(dir, msgId + extOf(bytes))
        if (!file.exists()) file.writeBytes(bytes) // msg_id 幂等
        return file.absolutePath
    }

    private fun extOf(bytes: ByteArray): String = when {
        bytes.size >= 8 && bytes[0] == 0x89.toByte() && bytes[1] == 0x50.toByte() -> ".png"
        bytes.size >= 3 && bytes[0] == 0xFF.toByte() && bytes[1] == 0xD8.toByte() -> ".jpg"
        else -> ".bin"
    }

    private fun MessageEntity.toChatMessage(): ChatMessage {
        val bytes = mediaPath?.let { p -> runCatching { File(p).readBytes() }.getOrNull() }
        return ChatMessage(
            msgId = msgId, peerId = peerId, fromMe = fromMe,
            text = content.orEmpty(),
            kind = when (type) {
                1 -> MsgKind.IMAGE
                2 -> MsgKind.FILE
                else -> MsgKind.TEXT
            },
            imageBytes = bytes, imgW = imgW, imgH = imgH, ts = ts, seq = seq,
            fileId = fileId, fileName = fileName, fileSize = fileSize,
            contentType = contentType, sha256 = sha256,
            localPath = localPath, transferred = transferred,
            status = when (status) {
                0 -> ChatMessage.Status.SENDING
                1 -> ChatMessage.Status.DELIVERED
                2 -> ChatMessage.Status.RECEIVED
                3 -> ChatMessage.Status.OFFLINE_STORED
                4 -> ChatMessage.Status.FAILED
                else -> ChatMessage.Status.RECEIVED
            },
        )
    }

    private fun ChatMessage.toEntity(ownerId: Int, mediaPath: String?) = MessageEntity(
        ownerId = ownerId, msgId = msgId,
        conversationId = conversationId(ownerId, peerId),
        peerId = peerId, fromMe = fromMe,
        type = when (kind) {
            MsgKind.IMAGE -> 1
            MsgKind.FILE -> 2
            else -> 0
        },
        content = if (kind == MsgKind.TEXT) text else null,
        mediaPath = mediaPath,
        imgW = imgW, imgH = imgH, ts = ts, seq = seq,
        fileId = fileId, fileName = fileName, fileSize = fileSize,
        contentType = contentType, sha256 = sha256,
        localPath = localPath, transferred = transferred,
        status = status.toDb(),
    )

    private fun ChatMessage.Status.toDb() = when (this) {
        ChatMessage.Status.SENDING -> 0
        ChatMessage.Status.DELIVERED -> 1
        ChatMessage.Status.RECEIVED -> 2
        ChatMessage.Status.OFFLINE_STORED -> 3
        ChatMessage.Status.FAILED -> 4
    }

}
