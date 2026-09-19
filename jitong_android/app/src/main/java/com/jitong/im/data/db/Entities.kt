package com.jitong.im.data.db

import androidx.room.Entity
import androidx.room.Fts4
import androidx.room.Index
import androidx.room.PrimaryKey

/**
 * 本地消息表。索引方法论（设计文档 4.2）：按查询场景反推、等值在前范围在后、克制数量。
 * ownerId = 本地登录账号 id：同一设备多账号数据隔离（换号登录互不可见）。
 */
@Entity(
    tableName = "messages",
    indices = [
        Index(value = ["ownerId", "msgId"], unique = true), // msg_id 幂等：漫游合并/重发不重复（D7）
        Index(value = ["ownerId", "conversationId", "seq"]), // 会话内按服务端权威序号分页/排序
        Index(value = ["ownerId", "conversationId", "status"]), // 失败重发/未读类查询
    ],
)
data class MessageEntity(
    @PrimaryKey(autoGenerate = true) val id: Long = 0,
    val ownerId: Int,
    val msgId: String,
    val conversationId: Long,
    val peerId: Int,
    val fromMe: Boolean,
    val type: Int,                // 0=TEXT 1=IMAGE 2=FIFE
    val content: String? = null,  // TEXT 正文
    val mediaPath: String? = null, // IMAGE 本地文件路径（字节落文件，库里只存路径）
    val imgW: Int = 0,
    val imgH: Int = 0,
    val ts: Long,
    val seq: Long = 0,            // 会话级序列号（服务端分配），聊天排序主键
    val fileId: String = "",
    val fileName: String = "",
    val fileSize: Long = 0,
    val contentType: String = "",
    val sha256: String = "",
    val thumbnailFileId: String = "",
    val thumbnailPath: String? = null,
    val thumbnailSize: Long = 0,
    val thumbnailSha256: String = "",
    val thumbnailW: Int = 0,
    val thumbnailH: Int = 0,
    val largeThumbnailFileId: String = "",
    val largeThumbnailPath: String? = null,
    val largeThumbnailSize: Long = 0,
    val largeThumbnailSha256: String = "",
    val largeThumbnailW: Int = 0,
    val largeThumbnailH: Int = 0,
    val localPath: String? = null,   // 文件本地路径（.part 进行中 / 成品）
    val transferred: Int = 0,        // 已传/已收块数（进度 + 续传游标）
    val status: Int,              // 0发送中 1已送达 2已接收 3离线转存
)

/**
 * FTS4 全文索引表（独立存储模式）。
 * 设计文档 4.2：不用 content= 外部内容模式（依赖触发器同步，Room 支持别扭，
 * 新手常踩"插了消息但搜不到"），独立存储多存一份文本，逻辑直白、无同步遗漏。
 */
@Fts4
@Entity(tableName = "messages_fts")
data class MessageFtsEntity(
    val content: String,
    val pinyin: String,
    val initials: String,
    val msgId: String,
)

/** 会话表：好友列表行展示最后消息 + 未读数 */
@Entity(tableName = "conversations")
data class ConversationEntity(
    @PrimaryKey val conversationId: Long,
    val ownerId: Int,
    val peerId: Int,
    val lastMsg: String,
    val lastTs: Long,
    val unread: Int,
)

/**
 * Legacy Room 在线迁移变更流。这里只保存权威表的定位信息，不复制大字段；导出 delta 时按 key
 * 回读当前行，DELETE 使用 tombstone。数据库触发器负责写入，避免旧 writer 绕过 Kotlin gateway。
 */
@Entity(
    tableName = "legacy_change_log",
    indices = [Index(value = ["ownerId", "changeSeq"])],
)
data class LegacyChangeLogEntity(
    @PrimaryKey(autoGenerate = true) val changeSeq: Long = 0,
    val ownerId: Int,
    val entityType: String,
    val entityKey: String,
    val operation: String,
    val changedAt: Long,
    /** v10：触发器固化的权威行快照版本；DELETE tombstone 为 0。 */
    val payloadVersion: Int = 0,
    /** v10：UPSERT 时的 JSON 快照；DELETE 为空，禁止导出时回读当前表。 */
    val payload: String = "",
)

/** state: 0=Legacy 可写，1=cutover 最终停写；按 owner 持久化，进程重启后仍生效。 */
@Entity(tableName = "legacy_cutover_control")
data class LegacyCutoverControlEntity(
    @PrimaryKey val ownerId: Int,
    val epoch: Long,
    val state: Int,
    val updatedAt: Long,
)
