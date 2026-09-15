package com.jitong.im.core

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch

/**
 * 消息页面的薄适配器：只把 Native 快照映射为 StateFlow。
 * 不维护 seq、不处理 ACK、不重试、不拼分页游标；这些都属于 C++ 内核。
 */
class NativeMessageController(private val sdk: JitongSdk,scope: CoroutineScope) {
    private val _conversations=MutableStateFlow<List<JitongSdk.NativeConversation>>(emptyList())
    val conversations: StateFlow<List<JitongSdk.NativeConversation>> = _conversations

    init {
        scope.launch {
            sdk.dataVersions
                .map { it[JitongSdk.DataDomain.Conversations] ?: 0L }
                .distinctUntilChanged()
                .map { sdk.loadConversations().orEmpty() }
                .flowOn(Dispatchers.IO)
                .collect { _conversations.value=it }
        }
    }

    /** 进入某会话后返回最新一页；消息版本变化自动重查，取消旧会话查询。 */
    fun messages(conversationId: Long): Flow<List<JitongSdk.NativeMessage>> =
        sdk.dataVersions
            .map { it[JitongSdk.DataDomain.Messages] ?: 0L }
            .distinctUntilChanged()
            .filter { conversationId>0 }
            .map { sdk.loadHistory(conversationId,50)?.messages.orEmpty() }
            .flowOn(Dispatchers.IO)

    fun sendText(conversationId:Long,peerId:Long,text:String)=
        sdk.sendText(conversationId,peerId,text)

    fun markRead(conversationId:Long,readSeq:Long)=sdk.markRead(conversationId,readSeq)

    fun operation(operationId:String):Flow<JitongSdk.OperationCompletion> =
        sdk.operationEvents.filter { it.operationId==operationId }.map { it.completion }
}
