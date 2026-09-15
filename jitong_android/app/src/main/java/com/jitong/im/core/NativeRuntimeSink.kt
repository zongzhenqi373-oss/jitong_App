package com.jitong.im.core

import androidx.annotation.Keep

/** JNI 运行时通知桥：只转发值，不查询数据库、不编排业务。 */
class NativeRuntimeSink(
    private val invalidated: (Int, Long, Long, Long) -> Unit,
    private val completed: (String, Int, String?, Long, Long) -> Unit,
) {
    @Keep
    fun onInvalidated(domain: Int, version: Long, generation: Long, ownerId: Long) =
        invalidated(domain, version, generation, ownerId)

    @Keep
    fun onOperationCompleted(
        operationId: String, status: Int, error: String?, generation: Long, ownerId: Long,
    ) = completed(operationId, status, error, generation, ownerId)
}
