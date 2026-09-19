package com.jitong.im.core

import android.content.Context
import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.jitong.im.data.db.AppDatabase
import com.jitong.im.data.MigrationExporter
import com.jitong.im.data.db.ConversationEntity
import com.jitong.im.data.db.MessageEntity
import com.jitong.im.data.db.LegacyCutoverControlEntity
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class LegacyChangeLogTest {
    @Test fun triggersCaptureAllAuthorityMutationsAndKeepTombstones() = runBlocking {
        val context = ApplicationProvider.getApplicationContext<Context>()
        val db = Room.inMemoryDatabaseBuilder(context, AppDatabase::class.java)
            .allowMainThreadQueries().build()
        try {
            AppDatabase.installChangeLogTriggers(db.openHelper.writableDatabase)
            val owner = 7001
            val conversation = 11L
            val message = MessageEntity(
                ownerId = owner, msgId = "delta-1", conversationId = conversation,
                peerId = 7002, fromMe = true, type = 0, content = "增量",
                ts = 1, status = 0,
            )
            assertTrue(db.messageDao().insertWithFts(message))
            db.messageDao().updateStatus(owner, "delta-1", 1)
            db.conversationDao().insertIgnore(
                ConversationEntity(conversation, owner, 7002, "增量", 1, 0),
            )
            db.conversationDao().clearUnread(conversation)
            db.openHelper.writableDatabase.execSQL(
                "DELETE FROM messages WHERE ownerId=? AND msgId=?", arrayOf(owner, "delta-1"),
            )
            db.openHelper.writableDatabase.execSQL(
                "DELETE FROM conversations WHERE ownerId=? AND conversationId=?",
                arrayOf(owner, conversation),
            )

            val rows = db.legacyChangeLogDao().page(owner, 0, 100)
            assertEquals(listOf("MESSAGE", "MESSAGE", "MESSAGE", "CONVERSATION", "CONVERSATION",
                "MESSAGE", "CONVERSATION"), rows.map { it.entityType })
            assertEquals(listOf("UPSERT", "UPSERT", "UPSERT", "UPSERT", "UPSERT", "DELETE", "DELETE"),
                rows.map { it.operation })
            val upserts = rows.filter { it.operation == "UPSERT" }
            assertTrue(upserts.all { it.payloadVersion == 1 && it.payload.isNotEmpty() })
            assertTrue(rows.filter { it.operation == "DELETE" }
                .all { it.payloadVersion == 0 && it.payload.isEmpty() })
            // 最终消息快照在 FTS 写入后的 touch 产生，必须固化拼音；后续删除不改变历史 payload。
            assertTrue(upserts.filter { it.entityType == "MESSAGE" }
                .any { it.payload.contains("\"pinyin\":") && it.payload.contains("zengliang") })
            assertEquals(rows.last().changeSeq, db.legacyChangeLogDao().highWater(owner))
            val exported = MigrationExporter.nextChanges(
                db.openHelper.readableDatabase, owner, 0, 100,
            )
            assertTrue(exported.contiguous)
            assertEquals(rows.map { it.changeSeq }, exported.changes.map { it.changeSeq })
            assertEquals(rows.last().changeSeq, exported.highWater)
            assertEquals(rows.size, db.legacyChangeLogDao().deleteThrough(owner, rows.last().changeSeq))
            AppDatabase.installWriteFenceTriggers(db.openHelper.writableDatabase)
            db.legacyCutoverControlDao().put(LegacyCutoverControlEntity(owner,9,1,1000))
            val blocked=runCatching { db.messageDao().insertWithFts(
                message.copy(id=0,msgId="after-freeze")) }.isFailure
            assertTrue("持久 fence 后旧消息 writer 必须失败",blocked)
            // 其它账号不受当前账号 fence 影响。
            assertTrue(db.messageDao().insertWithFts(
                message.copy(id=0,ownerId=owner+1,msgId="other-owner")))
        } finally {
            db.close()
        }
    }
}
