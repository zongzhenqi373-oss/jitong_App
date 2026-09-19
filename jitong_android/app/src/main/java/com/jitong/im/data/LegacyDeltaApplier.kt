package com.jitong.im.data

import androidx.sqlite.db.SupportSQLiteDatabase
import com.jitong.im.core.NativeBindings

/**
 * Room v10 change log → Native Writer 的唯一编排入口。
 *
 * 只有 Native 明确返回 committed checkpoint 后才清理 Room 日志；超时意味着可能已提交，
 * 必须保留日志并用同一 expectedAfter 重放，由 Native checkpoint 幂等判定。
 */
object LegacyDeltaApplier {
    sealed interface Result {
        data class Committed(val checkpoint: Long, val highWater: Long, val count: Int) : Result
        data class Retryable(val reason: String) : Result
        data class Rejected(val reason: String) : Result
        data object CaughtUp : Result
    }

    /** 以 Native 持久 checkpoint 为恢复事实源，调用方无需在 Kotlin 另存一份水位。 */
    fun applyNextPage(
        db: SupportSQLiteDatabase,
        handle: Long,
        ownerId: Int,
        epoch: Long,
        expectedAfter: Long? = null,
        limit: Int = MigrationExporter.BATCH_SIZE,
    ): Result {
        val recovered = expectedAfter ?: NativeBindings.nativeGetLegacyDeltaCheckpoint(handle, epoch)
            .takeIf { it.startsWith("ok|checkpoint=") }?.substringAfter("ok|checkpoint=")?.toLongOrNull()
            ?: return Result.Retryable("无法恢复 Native delta checkpoint")
        val page = MigrationExporter.nextChanges(db, ownerId, recovered, limit)
        if (!page.contiguous) return Result.Rejected("Room delta 页内 changeSeq 非严格递增")
        if (page.changes.isEmpty()) return if (page.highWater <= recovered) {
            Result.CaughtUp
        } else {
            Result.Retryable("Room high-water 已前进，重新读取 delta 页")
        }
        val response = NativeBindings.nativeSubmitLegacyDeltaBatch(
            handle, epoch, recovered, MigrationExporter.encodeChanges(page.changes),
        )
        if (response.startsWith("err|TimedOutButMayCommit|")) return Result.Retryable(response)
        if (!response.startsWith("ok|checkpoint=")) return Result.Rejected(response)
        val checkpoint = response.substringAfter("ok|checkpoint=").toLongOrNull()
            ?: return Result.Rejected("Native 返回非法 checkpoint")
        if (checkpoint < page.changes.last().changeSeq) {
            return Result.Rejected("Native checkpoint 落后于已提交页")
        }
        // 必须在明确成功后执行；重复 GC 安全。删除的是 tombstone/log，不删源业务表。
        db.execSQL(
            "DELETE FROM legacy_change_log WHERE ownerId=? AND changeSeq<=?",
            arrayOf(ownerId, checkpoint),
        )
        return Result.Committed(checkpoint, page.highWater, page.changes.size)
    }
}
