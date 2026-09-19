package com.jitong.im.core

import java.util.concurrent.ConcurrentHashMap
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.firstOrNull
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/**
 * 文本页面的薄适配器。
 *
 * UI 只提交发送、已读、翻页等意图并观察不可变快照；消息排序、游标、ACK、重试、
 * 漫游和持久化仍由 C++ 内核负责。本类不持有 Socket、数据库或业务状态机。
 */
class NativeMessageController(
    private val sdk: JitongSdk,
    private val scope: CoroutineScope,
) {
    data class ConversationPage(
        val messages: List<JitongSdk.NativeMessage> = emptyList(),
        val hasMore: Boolean = false,
        val loading: Boolean = false,
        val error: String? = null,
    )

    private data class PageSlot(
        val state: MutableStateFlow<ConversationPage>,
        var cursor: JitongSdk.HistoryCursor? = null,
        var refreshJob: Job? = null,
    )

    private val pages = ConcurrentHashMap<Long, PageSlot>()
    private val _conversations = MutableStateFlow<List<JitongSdk.NativeConversation>>(emptyList())
    val conversations: StateFlow<List<JitongSdk.NativeConversation>> = _conversations

    /*
     * SDK 的 operationEvents 是一条可靠 Channel。这里只设置一个消费者，再按 operationId
     * 分发；如果每个按钮各自 filter/collect，会互相抢走别人的完成事件。
     */
    private val _operationResults =
        MutableStateFlow<Map<String, JitongSdk.OperationCompletion>>(emptyMap())

    init {
        scope.launch(Dispatchers.IO) {
            sdk.dataVersions
                .map { it[JitongSdk.DataDomain.Conversations] ?: 0L }
                .distinctUntilChanged()
                .collect { _conversations.value = sdk.loadConversations().orEmpty() }
        }
        scope.launch {
            sdk.operationEvents.collect { event ->
                _operationResults.update { it + (event.operationId to event.completion) }
            }
        }
    }

    /** 打开会话并观察最新一页；消息版本变化后自动重新查询 Native 快照。 */
    fun messages(conversationId: Long): StateFlow<ConversationPage> {
        require(conversationId > 0) { "conversationId must be positive" }
        return pages.computeIfAbsent(conversationId) {
            val slot = PageSlot(MutableStateFlow(ConversationPage(loading = true)))
            slot.refreshJob = scope.launch(Dispatchers.IO) {
                sdk.dataVersions
                    .map { it[JitongSdk.DataDomain.Messages] ?: 0L }
                    .distinctUntilChanged()
                    .collect { reloadFirstPage(conversationId, slot) }
            }
            slot
        }.state
    }

    /** 下一页游标由 SDK/内核返回，UI 不解析或拼装游标。 */
    fun loadMore(conversationId: Long) {
        val slot = pages[conversationId] ?: return
        val before = slot.state.value
        val cursor = slot.cursor
        if (before.loading || !before.hasMore || cursor == null) return
        slot.state.value = before.copy(loading = true, error = null)
        scope.launch(Dispatchers.IO) {
            val page = sdk.loadHistory(conversationId, PAGE_SIZE, cursor)
            if (page == null) {
                slot.state.update { it.copy(loading = false, error = "历史消息读取失败") }
                return@launch
            }
            slot.cursor = page.nextCursor
            slot.state.update { current ->
                val merged = (current.messages + page.messages)
                    .distinctBy { it.msgId }
                    .sortedWith(messageOrder)
                ConversationPage(merged, page.hasMore, loading = false)
            }
        }
    }

    /** 页面离开后停止该会话订阅，避免 ViewModel 生命周期内无限积累。 */
    fun closeConversation(conversationId: Long) {
        pages.remove(conversationId)?.refreshJob?.cancel()
    }

    fun sendText(conversationId: Long, peerId: Long, text: String) =
        sdk.sendText(conversationId, peerId, text)

    /**
     * 拉取漫游历史：beforeSeq 由控制器从当前已加载的最早一条已分配 seq 的消息推导，
     * UI 不参与游标语义。漫游结果落库后由 dataVersions 驱动页面自动刷新。
     */
    fun requestRoamHistory(conversationId: Long, peerId: Long): Boolean {
        val earliestSeq = pages[conversationId]?.state?.value?.messages
            ?.filter { it.conversationSeq > 0 }
            ?.minByOrNull { it.conversationSeq }
            ?.conversationSeq ?: Long.MAX_VALUE
        return sdk.requestRoamMessages(peerId, earliestSeq)
    }

    /** 等待某个操作的终态（用于发送失败反馈）；订阅前先查已到达结果，避免丢失。 */
    suspend fun awaitOperation(operationId: String): JitongSdk.OperationCompletion? {
        consumeOperation(operationId)?.let { return it }
        return operation(operationId).firstOrNull()
    }

    fun markRead(conversationId: Long, readSeq: Long) = sdk.markRead(conversationId, readSeq)

    /** 搜索语义和 UTF-16 高亮区间由 Native SearchService 统一返回。 */
    fun search(keyword: String, conversationId: Long = 0, limit: Int = 100) =
        sdk.searchMessages(keyword, conversationId, limit)

    /** 完成结果可在调用后再订阅，不会因 ACK 很快返回而丢失。 */
    fun operation(operationId: String): Flow<JitongSdk.OperationCompletion> =
        _operationResults.map { it[operationId] }.filterNotNull().distinctUntilChanged()

    fun consumeOperation(operationId: String): JitongSdk.OperationCompletion? {
        val result = _operationResults.value[operationId] ?: return null
        _operationResults.update { it - operationId }
        return result
    }

    fun close() {
        pages.keys.toList().forEach(::closeConversation)
    }

    private fun reloadFirstPage(conversationId: Long, slot: PageSlot) {
        slot.state.update { it.copy(loading = true, error = null) }
        val page = sdk.loadHistory(conversationId, PAGE_SIZE)
        if (page == null) {
            slot.state.update { it.copy(loading = false, error = "消息快照读取失败") }
            return
        }
        slot.cursor = page.nextCursor
        slot.state.value = ConversationPage(
            messages = page.messages.sortedWith(messageOrder),
            hasMore = page.hasMore,
        )
    }

    private companion object {
        const val PAGE_SIZE = 50
        val messageOrder = compareBy<JitongSdk.NativeMessage>(
            { if (it.conversationSeq > 0) it.conversationSeq else Long.MAX_VALUE },
            { it.localOrder },
            { it.msgId },
        )
    }
}
