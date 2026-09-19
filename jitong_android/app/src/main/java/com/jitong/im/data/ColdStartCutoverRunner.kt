package com.jitong.im.data

import android.content.Context
import com.jitong.im.JitongApplication
import com.jitong.im.core.CutoverMirrorStore
import com.jitong.im.core.JitongSdk
import com.jitong.im.core.NativeBindings
import com.jitong.im.core.platform.NativeDbKeyPlatform
import com.jitong.im.data.crypto.DbKeyManager
import com.jitong.im.data.crypto.DbKeyResult
import com.jitong.im.data.db.AppDatabase
import java.io.File

/** 冷启动迁移执行器。调用前进程不得创建 Legacy ViewModel/Socket。 */
class ColdStartCutoverRunner(
    private val context: Context,
    private val ownerMatches: (Int) -> Boolean = { Prefs.loadTokenSession()?.userId == it },
) {
    sealed interface Outcome {
        data object Committed : Outcome
        data class Failed(val reason: String) : Outcome
    }

    fun run(pending: ColdStartCutoverRequest.Pending): Outcome {
        val owner = pending.ownerId
        if (owner <= 0 || pending.epoch <= 0 ||
            ColdStartCutoverRequest.pending(context) != pending ||
            !ownerMatches(owner))
            return Outcome.Failed("切换预约或账号已变化")
        if (!NativeBindings.isLoaded) return Outcome.Failed("Native 内核不可用")
        if (!context.getDatabasePath("jitong_$owner.db").isFile)
            return Outcome.Failed("旧消息库不存在")
        val key = DbKeyManager.unlockRealKey(context, owner) as? DbKeyResult.Success
            ?: return Outcome.Failed("旧消息库密钥不可用，原数据保留")
        val sdk = JitongSdk()
        var opened = false
        try {
            val room = AppDatabase.get(context, owner, key.key)
            val db = room.openHelper.writableDatabase
            if (!sdk.start(null)) return Outcome.Failed("Native 内核启动失败")
            val root = File(context.filesDir, JitongApplication.NATIVE_ROOT)
            if (!root.isDirectory && !root.mkdirs())
                return Outcome.Failed("Native 数据目录无法创建，原数据保留")
            opened = sdk.openAccountDatabase(owner, root.absolutePath, NativeDbKeyPlatform(context))
            if (!opened) return Outcome.Failed("Native 数据库无法打开，原数据保留")
            // 本进程从未启动旧连接与旧写入者；不是对在线 ViewModel 做乐观停写。
            val result = CutoverMigrationCoordinator(sdk, CutoverMirrorStore(context))
                .execute(db, owner, pending.epoch, CutoverMigrationCoordinator.LegacyQuiescence { true })
            return when (result) {
                is CutoverMigrationCoordinator.Result.Committed -> Outcome.Committed
                is CutoverMigrationCoordinator.Result.Retryable -> Outcome.Failed(
                    "迁移停在 ${result.phase}：${result.reason}；旧库已保留，请勿清理数据")
                is CutoverMigrationCoordinator.Result.RepairRequired -> Outcome.Failed(result.reason)
            }
        } catch (t: Throwable) {
            return Outcome.Failed("迁移异常：${t.message ?: t.javaClass.simpleName}；旧库已保留")
        } finally {
            key.key.fill(0)
            if (opened) sdk.closeAccountDatabase()
            sdk.stop()
            AppDatabase.closeCurrent()
        }
    }
}
