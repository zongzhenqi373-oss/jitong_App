package com.jitong.im.core

/** 冷启动后端选择的唯一入口；任何损坏/分歧都返回 Repair，绝不静默回 Legacy。 */
object CutoverRecoverySelector {
    enum class Action { Legacy, Native, Repair }

    data class DbEvidence(
        val present: Boolean,
        val epoch: Long = 0,
        val state: Int = -1,
        val highWater: Long = 0,
        val schemaVersion: Int = 0,
        val keyId: String = "",
        val summary: String = "",
        val updatedAt: Long = 0,
    )

    fun parseDb(response: String): DbEvidence? {
        if (response == "ok|present=0") return DbEvidence(false)
        if (!response.startsWith("ok|present=1|")) return null
        fun field(name: String): String? = response.substringAfter("|$name=", "")
            .substringBefore('|').takeIf { it.isNotEmpty() }
        return DbEvidence(
            present = true,
            epoch = field("epoch")?.toLongOrNull() ?: return null,
            state = field("state")?.toIntOrNull()?.takeIf { it in 0..3 } ?: return null,
            highWater = field("highWater")?.toLongOrNull() ?: return null,
            schemaVersion = field("schemaVersion")?.toIntOrNull() ?: return null,
            keyId = field("keyId") ?: return null,
            updatedAt = field("updatedAt")?.toLongOrNull() ?: return null,
            summary = response.substringAfter("|summary=", ""),
        )
    }

    fun resolve(
        ownerId: Long,
        db: DbEvidence?,
        mirror: CutoverMirrorStore.ReadResult,
        nativeExists: Boolean,
        nativeLoadable: Boolean,
    ): Action {
        val mirrorEvidence = (mirror as? CutoverMirrorStore.ReadResult.Valid)?.evidence
        val dbState = when { db == null -> -2; !db.present -> -1; else -> db.state }
        val mirrorState = when (mirror) {
            CutoverMirrorStore.ReadResult.Missing -> -1
            is CutoverMirrorStore.ReadResult.Corrupt -> -2
            is CutoverMirrorStore.ReadResult.Valid -> mirror.evidence.state
        }
        fun valid(state: Int, evidenceOwner: Long, epoch: Long): Boolean =
            state in -1..3 && state != -2 && (state == -1 || (evidenceOwner == ownerId && epoch > 0))
        val dbOwner = if (db?.present == true) ownerId else 0
        val dbEpoch = db?.epoch ?: 0
        val mirrorOwner = mirrorEvidence?.ownerId ?: 0
        val mirrorEpoch = mirrorEvidence?.epoch ?: 0
        if (ownerId <= 0 || !valid(dbState, dbOwner, dbEpoch) ||
            !valid(mirrorState, mirrorOwner, mirrorEpoch)) return Action.Repair
        if (dbState == -1 && mirrorState == -1) return if (nativeExists) Action.Repair else Action.Legacy
        if (dbState == -1 || mirrorState == -1 || dbState != mirrorState || dbEpoch != mirrorEpoch)
            return Action.Repair
        if (db?.present == true && mirrorEvidence != null &&
            (db.highWater != mirrorEvidence.highWater ||
             db.schemaVersion != mirrorEvidence.schemaVersion ||
             db.keyId != mirrorEvidence.keyId || db.summary != mirrorEvidence.summary ||
             db.updatedAt != mirrorEvidence.updatedAt)) return Action.Repair
        if (dbState == 0 || dbState == 1) return Action.Legacy
        return if ((dbState == 2 || dbState == 3) && nativeExists && nativeLoadable) Action.Native
        else Action.Repair
    }
}
