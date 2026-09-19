package com.jitong.im.data

import androidx.sqlite.db.SupportSQLiteDatabase
import com.jitong.im.core.CutoverMirrorStore
import com.jitong.im.core.CutoverRecoverySelector
import com.jitong.im.core.JitongSdk

/** Room→Native 单向 cutover 总协调器。调用方必须在 IO 线程执行，成功后立即重启进程。 */
class CutoverMigrationCoordinator(
    private val sdk:JitongSdk,
    private val mirror:CutoverMirrorStore,
) {
    /**
     * 在最终停写前阻止新 Legacy 网络事件入队，等待已在飞的 Room/媒体写任务排空。
     * 返回 false 时绝不设置 Room 停写闸门。返回 true 后调用方必须保持静默，
     * 直到进程重启或进入明确的修复流程；协调器不会自行恢复旧连接。
     */
    fun interface LegacyQuiescence {
        fun stopAndDrain(): Boolean
    }

    sealed interface Result {
        data class Committed(val epoch:Long,val highWater:Long):Result
        data class Retryable(val phase:String,val reason:String):Result
        data class RepairRequired(val reason:String):Result
    }

    fun execute(db:SupportSQLiteDatabase,ownerId:Int,epoch:Long,
                quiescence:LegacyQuiescence):Result {
        require(ownerId>0&&epoch>0)
        val now=System.currentTimeMillis()/1000
        val observedHighWater=highWater(db,ownerId)
        val journal=CutoverRecoverySelector.parseDb(sdk.getCutoverJournal())
            ?: return Result.RepairRequired("Native journal 无法读取")
        val baseline:Long
        if(!journal.present){
            baseline=observedHighWater
            val initial=CutoverMirrorStore.Evidence(ownerId.toLong(),epoch,LEGACY,baseline,
                ROOM_SCHEMA,KEY_ID,"migration-start",now)
            if(sdk.advanceCutoverJournal(epoch,-1,LEGACY,baseline,ROOM_SCHEMA,KEY_ID,
                    initial.summary,now)!="ok"||!mirror.write(initial))
                return Result.RepairRequired("LEGACY journal 或 HMAC 镜像提交不完整")
        }else{
            // PREPARED 之前的失败是可重试的：基线必须沿用首次 journal，不能用
            // 当前 high-water 覆盖，否则中间产生的 Legacy 写会被跳过。
            val saved=(mirror.read() as? CutoverMirrorStore.ReadResult.Valid)?.evidence
                ?:return Result.RepairRequired("LEGACY 恢复时 HMAC 镜像缺失或损坏")
            if(journal.state!=LEGACY||journal.epoch!=epoch||saved.ownerId!=ownerId.toLong()||
                saved.epoch!=journal.epoch||saved.state!=journal.state||
                saved.highWater!=journal.highWater||saved.schemaVersion!=journal.schemaVersion||
                saved.keyId!=journal.keyId||saved.summary!=journal.summary||
                saved.updatedAt!=journal.updatedAt)
                return Result.RepairRequired("已存在的 cutover 双证据不允许续跑")
            baseline=journal.highWater
        }
        if(!sdk.beginMigration())return Result.Retryable("begin","无法进入 shadow_import")
        val seeded=sdk.seedLegacyDeltaCheckpoint(epoch,baseline,now)
        if(seeded!="ok|checkpoint=$baseline")return Result.Retryable("baseline",seeded)

        var messageCursor=0L
        while(true){
            val batch=MigrationExporter.nextBatch(db,messageCursor)?:break
            val response=sdk.submitMigrationBatch(batch.encoded,batch.lastId.toString())
            if(!response.startsWith("ok|"))return Result.Retryable("message_snapshot",response)
            messageCursor=batch.lastId
            if(batch.count<MigrationExporter.BATCH_SIZE)break
        }
        var conversationCursor=0L
        while(true){
            val batch=MigrationExporter.nextConversationBatch(db,conversationCursor)?:break
            val response=sdk.submitConversationBatch(batch.encoded,batch.lastId.toString())
            if(!response.startsWith("ok|"))return Result.Retryable("conversation_snapshot",response)
            conversationCursor=batch.lastId
            if(batch.count<MigrationExporter.BATCH_SIZE)break
        }
        catchUp(db,ownerId,epoch)?.let{return it}

        // Snapshot/delta 可以在线追平，但最终 fence 必须在旧连接与所有旧 writer
        // 已停止后建立。没有生产级 stop+drain 证明时，不允许“试着切一下”。
        if (!quiescence.stopAndDrain())
            return Result.Retryable("quiesce","Legacy 网络或写任务尚未完全静默")

        // SQLite 单 Writer 顺序保证：早先 Legacy 写先完成；fence 事务提交后，后续所有权威写被触发器拒绝。
        try {
            db.beginTransaction()
            db.execSQL("""INSERT INTO legacy_cutover_control(ownerId,epoch,state,updatedAt)
                VALUES(?,?,1,?) ON CONFLICT(ownerId) DO UPDATE SET epoch=excluded.epoch,
                state=1,updatedAt=excluded.updatedAt""",arrayOf(ownerId,epoch,System.currentTimeMillis()/1000))
            db.setTransactionSuccessful()
        } catch(t:Throwable) {
            return Result.Retryable("freeze",t.message?:"Legacy 停写失败")
        } finally { if(db.inTransaction())db.endTransaction() }

        var prepared=false
        try {
            catchUp(db,ownerId,epoch)?.let{return it}
            val finalHighWater=highWater(db,ownerId)
            val summary=MigrationExporter.summary(db)
            val summaryText="messages=${summary.messages},conversations=${summary.conversations},"+
                "minSeq=${summary.minSeq},maxSeq=${summary.maxSeq},fts=${summary.fts}"
            val finish=sdk.finishMigration(summary.messages,summary.conversations,summary.minSeq,
                summary.maxSeq,summary.fts)
            if(!finish.startsWith("ok|"))return Result.Retryable("verify",finish)
            val preparedAt=System.currentTimeMillis()/1000
            val preparedEvidence=CutoverMirrorStore.Evidence(ownerId.toLong(),epoch,PREPARED,
                finalHighWater,ROOM_SCHEMA,KEY_ID,summaryText,preparedAt)
            if(sdk.advanceCutoverJournal(epoch,LEGACY,PREPARED,finalHighWater,ROOM_SCHEMA,KEY_ID,
                    summaryText,preparedAt)!="ok")return Result.RepairRequired("PREPARED DB journal 失败")
            prepared=true
            if(!mirror.write(preparedEvidence))return Result.RepairRequired("PREPARED 镜像失败")
            val committedAt=System.currentTimeMillis()/1000
            val committed=preparedEvidence.copy(state=NO_WRITE,updatedAt=committedAt)
            if(sdk.advanceCutoverJournal(epoch,PREPARED,NO_WRITE,finalHighWater,ROOM_SCHEMA,KEY_ID,
                    summaryText,committedAt)!="ok")return Result.RepairRequired("NO_WRITE DB journal 失败")
            if(!mirror.write(committed))return Result.RepairRequired("NO_WRITE 镜像失败")
            return Result.Committed(epoch,finalHighWater)
        } finally {
            // 只有尚未写 PREPARED 时允许恢复 Legacy 写；一旦有跨库提交证据，不再自动猜测回退。
            if(!prepared)runCatching { db.execSQL(
                "DELETE FROM legacy_cutover_control WHERE ownerId=? AND epoch=? AND state=1",
                arrayOf(ownerId,epoch)) }
        }
    }

    private fun catchUp(db:SupportSQLiteDatabase,ownerId:Int,epoch:Long):Result? {
        repeat(MAX_DELTA_PAGES){
            val checkpoint=sdk.getLegacyDeltaCheckpoint(epoch).substringAfter("ok|checkpoint=","")
                .toLongOrNull()?:return Result.Retryable("delta","无法读取 checkpoint")
            val page=MigrationExporter.nextChanges(db,ownerId,checkpoint)
            if(page.changes.isEmpty())return if(page.highWater<=checkpoint)null
                else Result.Retryable("delta","high-water 前进，需重读")
            val response=sdk.submitLegacyDeltaBatch(epoch,checkpoint,
                MigrationExporter.encodeChanges(page.changes))
            if(!response.startsWith("ok|checkpoint="))return Result.Retryable("delta",response)
            val committed=response.substringAfter("ok|checkpoint=").toLongOrNull()
                ?:return Result.RepairRequired("Native checkpoint 格式非法")
            db.execSQL("DELETE FROM legacy_change_log WHERE ownerId=? AND changeSeq<=?",
                arrayOf(ownerId,committed))
        }
        return Result.Retryable("delta","超过单次追平页数上限")
    }

    private fun highWater(db:SupportSQLiteDatabase,ownerId:Int):Long=
        db.query("SELECT COALESCE(MAX(changeSeq),0) FROM legacy_change_log WHERE ownerId=?",
            arrayOf(ownerId)).use{if(it.moveToFirst())it.getLong(0)else 0}

    private companion object {
        const val LEGACY=0;const val PREPARED=1;const val NO_WRITE=2
        const val ROOM_SCHEMA=11
        const val KEY_ID="android-keystore:jitong.cutover.hmac.v1"
        const val MAX_DELTA_PAGES=10_000
    }
}
