package com.jitong.im

import android.app.Application
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.jitong.im.core.CutoverMirrorStore
import com.jitong.im.core.CutoverRecoverySelector
import com.jitong.im.core.JitongSdk
import com.jitong.im.core.KernelBackendSelector
import com.jitong.im.core.platform.NativeDbKeyPlatform
import com.jitong.im.data.Prefs
import com.jitong.im.data.ColdStartCutoverRequest
import com.tencent.mmkv.MMKV
import java.io.File

/**
 * 进程级的唯一后端决策点。Activity 创建前完成双证据校验，然后冻结
 * [KernelBackendSelector]；运行期不允许 Legacy/Native 热切换。
 */
class JitongApplication : Application() {
    private val migrationRunning = java.util.concurrent.atomic.AtomicBoolean(false)
    sealed interface Startup {
        data object Legacy : Startup
        data class Migrating(val ownerId: Int) : Startup
        data class Native(val ownerId: Int) : Startup
        data class Repair(val reason: String) : Startup
    }

    var startup: Startup by mutableStateOf<Startup>(Startup.Legacy)
        private set

    override fun onCreate() {
        super.onCreate()
        MMKV.initialize(this)
        // cutover DIRTY 镜像闭环：Native transaction hook 推进 DIRTY 后同步带 MAC 镜像。
        val mirrorStore = CutoverMirrorStore(this)
        com.jitong.im.core.NativeBindings.cutoverDirtyHandler = { evidence ->
            if (!mirrorStore.write(evidence)) {
                android.util.Log.w("JitongKernel", "DIRTY 镜像写入失败，冷启动将 fail-close 为 Repair")
            }
        }
        val pending = ColdStartCutoverRequest.pending(this)
        startup = if (pending != null) {
            if (Prefs.loadTokenSession()?.userId != pending.ownerId)
                Startup.Repair("切换预约账号与登录凭据不一致")
            else when (val recovered = decideBackend()) {
                is Startup.Native -> { ColdStartCutoverRequest.clear(this); recovered }
                is Startup.Repair -> recovered
                Startup.Legacy -> Startup.Migrating(pending.ownerId)
                is Startup.Migrating -> error("unexpected migration decision")
            }
        } else decideBackend()
        KernelBackendSelector.initialize(startup is Startup.Native || startup is Startup.Migrating)
    }

    /** 仅由冷启动迁移页调用；当前进程从未创建 Legacy ViewModel 或 Socket。 */
    suspend fun completePendingCutover() {
        if (!migrationRunning.compareAndSet(false, true)) return
        val pending = ColdStartCutoverRequest.pending(this) ?: run {
            startup = Startup.Repair("切换预约不存在")
            return
        }
        // Activity 重建不能取消已进入事务的迁移，也不能并发启动第二次。
        val outcome = kotlinx.coroutines.withContext(
            kotlinx.coroutines.NonCancellable + kotlinx.coroutines.Dispatchers.IO,
        ) {
            com.jitong.im.data.ColdStartCutoverRunner(this@JitongApplication).run(pending)
        }
        startup = when (outcome) {
            is com.jitong.im.data.ColdStartCutoverRunner.Outcome.Committed -> {
                val recovered = kotlinx.coroutines.withContext(
                    kotlinx.coroutines.NonCancellable + kotlinx.coroutines.Dispatchers.IO,
                ) { decideBackend() }
                if (recovered is Startup.Native) {
                    ColdStartCutoverRequest.clear(this)
                    recovered
                } else Startup.Repair("迁移完成但冷启动证据验证失败")
            }
            is com.jitong.im.data.ColdStartCutoverRunner.Outcome.Failed -> Startup.Repair(outcome.reason)
        }
    }

    private fun decideBackend(): Startup {
        val mirrorStore = CutoverMirrorStore(this)
        val mirror = mirrorStore.read()
        val mirroredOwner = (mirror as? CutoverMirrorStore.ReadResult.Valid)?.evidence?.ownerId
        val ownerId = mirroredOwner?.takeIf { it in 1..Int.MAX_VALUE }?.toInt()
            ?: Prefs.loadTokenSession()?.userId
        if (ownerId == null || ownerId <= 0) {
            return if (mirror is CutoverMirrorStore.ReadResult.Missing) Startup.Legacy
            else Startup.Repair("切换证据存在，但无法确定本地账号")
        }

        val root = File(filesDir, NATIVE_ROOT)
        val database = File(File(root, "native_db"), "account_${ownerId}.db")
        val nativeExists = database.isFile
        if (!nativeExists && mirror is CutoverMirrorStore.ReadResult.Missing) return Startup.Legacy
        if (!nativeExists) return Startup.Repair("Native 数据库缺失，禁止根据单边镜像回退")

        val sdk = JitongSdk()
        var opened = false
        return try {
            if (!sdk.start(null)) return Startup.Repair("Native 内核无法加载")
            opened = sdk.openAccountDatabase(ownerId, root.absolutePath, NativeDbKeyPlatform(this))
            val dbEvidence = if (opened) CutoverRecoverySelector.parseDb(sdk.getCutoverJournal()) else null
            val selfTestOk = opened && sdk.runDatabaseSelfTest().startsWith("ok|")
            val action = CutoverRecoverySelector.resolve(
                ownerId.toLong(), dbEvidence, mirror, nativeExists, selfTestOk,
            )
            // PREPARED 时 Room fence 已持久生效，不能再启动普通 Legacy UI。
            // 双证据一致且 Native 自检通过时，冷启动幂等补完 NO_WRITE；任一边
            // 提交失败都 fail-close 到 Repair，绝不启动旧 writer。
            if (action == CutoverRecoverySelector.Action.Legacy && dbEvidence?.state == PREPARED &&
                mirror is CutoverMirrorStore.ReadResult.Valid && selfTestOk) {
                val old = mirror.evidence
                val committedAt = System.currentTimeMillis() / 1000
                val advanced = sdk.advanceCutoverJournal(
                    old.epoch, PREPARED, NO_WRITE, old.highWater, old.schemaVersion,
                    old.keyId, old.summary, committedAt,
                ) == "ok"
                if (!advanced || !mirrorStore.write(old.copy(state = NO_WRITE, updatedAt = committedAt)))
                    return Startup.Repair("PREPARED 恢复到 NO_WRITE 时双证据提交失败")
                return Startup.Native(ownerId)
            }
            when (action) {
                CutoverRecoverySelector.Action.Legacy -> Startup.Legacy
                CutoverRecoverySelector.Action.Native -> Startup.Native(ownerId)
                CutoverRecoverySelector.Action.Repair -> Startup.Repair(
                    if (!opened) "Native 数据库密钥不可用或数据库无法打开"
                    else "Cutover journal 与 HMAC 镜像不一致",
                )
            }
        } catch (t: Throwable) {
            Startup.Repair("冷启动恢复检查失败：${t.message ?: t.javaClass.simpleName}")
        } finally {
            if (opened) sdk.closeAccountDatabase()
            sdk.stop()
        }
    }

    companion object {
        const val NATIVE_ROOT = "native_kernel"
        private const val PREPARED = 1
        private const val NO_WRITE = 2
    }
}
