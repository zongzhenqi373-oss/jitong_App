package com.jitong.im.data

import kotlinx.coroutines.Job
import kotlinx.coroutines.withTimeoutOrNull

/** UI/网络入口关闭之后，等待此前在 ViewModel scope 派生的任务全部落盘。 */
object CutoverTaskDrain {
    suspend fun awaitIdle(parent: Job?, current: Job?, timeoutMs: Long,
                          hasActiveMedia: () -> Boolean): Boolean {
        if (parent == null || current == null || timeoutMs <= 0) return false
        return withTimeoutOrNull(timeoutMs) {
            // 子任务在完成前可能继续派生子任务，需连续观察多轮。
            repeat(3) {
                val siblings = parent.children.filter { it != current && it.isActive }.toList()
                for (job in siblings) job.join()
                if (hasActiveMedia()) return@withTimeoutOrNull false
            }
            parent.children.none { it != current && it.isActive } && !hasActiveMedia()
        } ?: false
    }
}
