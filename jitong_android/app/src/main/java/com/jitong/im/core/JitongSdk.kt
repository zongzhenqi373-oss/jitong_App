package com.jitong.im.core

import com.jitong.im.core.platform.NativeDbKeyPlatform
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.receiveAsFlow
import kotlinx.coroutines.flow.update
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.CodingErrorAction
import com.jitong.im.util.PinyinIndex

/**
 * C++ Native 内核的 Kotlin 门面。
 *
 * 职责边界（V3 计划 §3.2）：
 *   - 只做句柄管理与事件投递；
 *   - **不做** Token 判断、登录重试、消息去重、数据库双写、文件重试、缩略图选择。
 *
 * 是否接管业务由进程启动时冻结的 [KernelBackendSelector] 与 cutover 状态共同决定；本类自身
 * 不创建 Legacy 后端，也不允许运行期热切换。
 */
class JitongSdk internal constructor() {
    @Volatile private var runtimeOwnerId: Long = 0
    @Volatile private var runtimeGeneration: Long = 0

    private val _dataVersions = MutableStateFlow<Map<DataDomain, Long>>(emptyMap())
    /** 各数据域的最新 Native dbVersion；UI 观察版本变化后只重新查询快照。 */
    val dataVersions: StateFlow<Map<DataDomain, Long>> = _dataVersions

    private val operationChannel = Channel<OperationEvent>(Channel.UNLIMITED)
    /** 命令终态可靠流；operationId exactly-once，UI 无需轮询 consumeOperation。 */
    val operationEvents: Flow<OperationEvent> = operationChannel.receiveAsFlow()

    data class OperationEvent(
        val operationId: String,val completion: OperationCompletion,
        val generation: Long,val ownerId: Long,
    )

    private val runtimeSink = NativeRuntimeSink(
        invalidated = { domain, version, generation, ownerId ->
            val key=DataDomain.values().firstOrNull { it.nativeValue==domain }
            if(key!=null&&generation==runtimeGeneration&&ownerId==runtimeOwnerId)
                _dataVersions.update { current -> current.toMutableMap().also { it[key]=version } }
        },
        completed = { operationId, status, error, generation, ownerId ->
            val value=OperationStatus.values().getOrNull(status)
            if(value!=null&&generation==runtimeGeneration&&ownerId==runtimeOwnerId)
                operationChannel.trySend(OperationEvent(operationId,
                    OperationCompletion(value,error?.ifBlank { null }),generation,ownerId))
        },
    )

    data class SearchRange(val startUtf16: Int, val endUtf16: Int)
    data class SearchHit(
        val msgId: String,
        val conversationId: Long,
        val peerId: Long,
        val timestamp: Long,
        val snippet: String,
        val highlights: List<SearchRange>,
    )
    data class HistoryCursor(
        val serverTime: Long, val conversationSeq: Long, val localOrder: Long, val msgId: String,
    )
    data class NativeMedia(
        val mediaPath: String, val width: Int, val height: Int,
        val fileId: String, val fileName: String, val fileSize: Long,
        val contentType: String, val sha256: String,
        val thumbnailFileId: String, val thumbnailPath: String, val thumbnailSize: Long,
        val thumbnailSha256: String, val thumbnailWidth: Int, val thumbnailHeight: Int,
        val largeThumbnailFileId: String, val largeThumbnailPath: String,
        val largeThumbnailSize: Long, val largeThumbnailSha256: String,
        val largeThumbnailWidth: Int, val largeThumbnailHeight: Int,
        val localPath: String, val transferred: Long,
    )
    data class NativeMessage(
        val ownerId: Long, val msgId: String, val conversationId: Long, val peerId: Long,
        val conversationSeq: Long, val serverTime: Long, val localOrder: Long,
        val fromMe: Boolean, val type: Int, val content: String, val status: Int,
        val pinyin: String, val initials: String, val media: NativeMedia,
    )
    data class HistoryPage(
        val messages: List<NativeMessage>, val hasMore: Boolean, val nextCursor: HistoryCursor?,
    )
    data class NativeConversation(
        val conversationId: Long,val ownerId: Long,val peerId: Long,
        val lastMessage: String,val lastMessageTime: Long,val unread: Long,
    )
    data class NativeFriend(
        val friendId: Long,
        val nick: String,
        val tel: String,
        val avatar: String,
        val signature: String,
        val sex: Int,
        val online: Boolean,
    )
    data class NativeFriendRequest(
        val requestId: String,
        val fromUserId: Long,
        val toUserId: Long,
        val direction: Int,
        val state: Int,
        val message: String,
        val createdAt: Long,
    )

    private val sink = NativeEventSink()
    private val authPlatform = com.jitong.im.core.platform.NativeAuthPlatform()

    /** 内核上抛的事件流；订阅方自行切到 UI 线程。 */
    val events: Flow<CoreEvent> = sink.events

    @Volatile
    private var handle: Long = 0L

    /** 创建内核句柄并挂上事件桥。重复调用会先销毁旧句柄。 */
    @Synchronized
    fun start(serverName: String?): Boolean {
        if (!NativeBindings.isLoaded) return false
        if (handle != 0L) stop()

        val created = NativeBindings.nativeCreate(serverName)
        if (created == 0L) return false

        if (!NativeBindings.nativeSetEventSink(created, sink)) {
            NativeBindings.nativeDestroy(created)
            return false
        }
        handle = created
        return true
    }

    /** 销毁句柄。幂等，可重复调用。 */
    @Synchronized
    fun stop() {
        val current = handle
        handle = 0L
        accountReady = false
        _accountState.value = AccountState.LoggedOut
        runtimeGeneration=0;runtimeOwnerId=0;_dataVersions.value=emptyMap()
        if (current != 0L) NativeBindings.nativeDestroy(current)
    }

    /** Native 自检报告；.so 不可用时返回 null。 */
    fun selfTest(): Map<String, String>? =
        if (NativeBindings.isLoaded) NativeBindings.selfTestMap() else null

    /** 生命周期压力测试报告；.so 不可用时返回 null。 */
    fun lifecycleStressTest(iterations: Int = 100): Map<String, String>? {
        if (!NativeBindings.isLoaded) return null
        val text = NativeBindings.nativeLifecycleStressTest(iterations, sink)
        return text.lineSequence().mapNotNull { line ->
            val idx = line.indexOf('=')
            if (idx > 0) line.substring(0, idx) to line.substring(idx + 1) else null
        }.toMap()
    }

    /** Instrumented test 专用，不参与业务协议。 */
    internal fun emitUtf8Test(): Boolean {
        val current = handle
        return current != 0L && NativeBindings.nativeEmitUtf8Test(current)
    }

    /** 内核版本；.so 不可用时返回 null。 */
    fun version(): String? =
        if (NativeBindings.isLoaded) runCatching { NativeBindings.nativeVersion() }.getOrNull() else null

    // ---------------- P5-T06：认证会话（厚内核编排） ----------------

    /** 一条账号事件（由 C++ AccountSession 上抛）。 */
    data class AccountEvent(
        val type: AccountEventType,
        val operationId: Long,
        val accountState: AccountState,
        val connectionState: ConnectionState,
        val error: AuthError,
        val userId: Int,
        val message: String?,
    )

    private val _accountState = MutableStateFlow(AccountState.LoggedOut)
    /** 当前账号状态（UI 只读映射，不自行判断 token）。 */
    val accountState: StateFlow<AccountState> = _accountState

    private val _accountEvents = MutableSharedFlow<AccountEvent>(extraBufferCapacity = 32)
    /** 账号事件流（登录成功/失败、刷新、被踢、登出等）。 */
    val accountEvents: SharedFlow<AccountEvent> = _accountEvents

    private val accountSink = object : NativeAccountSink {
        override fun onAccountEvent(
            type: Int, operationId: Long, accountState: Int, connectionState: Int,
            error: Int, userId: Int, message: String?,
        ) {
            val ev = AccountEvent(
                type = AccountEventType.fromOrdinal(type),
                operationId = operationId,
                accountState = AccountState.fromOrdinal(accountState),
                connectionState = ConnectionState.fromOrdinal(connectionState),
                error = AuthError.fromOrdinal(error),
                userId = userId,
                message = message,
            )
            _accountState.value = ev.accountState
            _accountEvents.tryEmit(ev)
        }
    }

    @Volatile
    private var accountReady = false

    /**
     * 建立认证会话（在 [start] 之后调用）。之后所有连接/握手/设备签名/Token/刷新/重连/
     * 被踢处理都在 C++ 内核编排；UI 只调用登录/登出意图并订阅 [accountState]/[accountEvents]。
     */
    @Synchronized
    fun setupAccount(serverIp: String, port: Int): Boolean {
        val current = handle
        if (current == 0L || !NativeBindings.isLoaded) return false
        accountReady = NativeBindings.nativeAccountSetup(current, serverIp, port, accountSink, authPlatform)
        return accountReady
    }

    /** 密码登录。返回 operationId（0 表示被拒/未就绪）。 */
    fun loginWithPassword(account: String, password: String): Long {
        val current = handle
        if (current == 0L || !accountReady) return 0L
        return NativeBindings.nativeAccountLoginWithPassword(current, account, password)
    }

    /** 冷启动自动登录：有未过期凭据则 Token 登录，否则返回 0（需密码登录）。 */
    fun startWithSavedToken(account: String): Long {
        val current = handle
        if (current == 0L || !accountReady) return 0L
        return NativeBindings.nativeAccountStartWithSavedToken(current, account)
    }

    /**
     * 一次性注册（先于认证）。返回 false 表示本地拒绝（未就绪/账号非空闲/参数非法）；
     * true 仅表示已提交，结果经 [events] 的 [CoreEvent.RegisterResult] 异步到达：
     * 1=成功，2=昵称占用，3=手机号已注册，0=本地连接失败。
     */
    fun register(nick: String, tel: String, pass: String): Boolean {
        val current = handle
        if (current == 0L || !accountReady) return false
        if (nick.isBlank() || tel.isBlank() || pass.isBlank()) return false
        return NativeBindings.nativeAccountRegister(current, nick, tel, pass)
    }

    /**
     * 冷启动便捷入口：只组装 Native 认证会话并尝试恢复加密凭据。
     * 返回 0 表示本地无有效凭据，调用方应停留登录页；这里不会回退到软件密钥或明文存储。
     */
    fun restoreAccount(serverIp: String, port: Int, account: String): Long {
        if (!setupAccount(serverIp, port)) return 0L
        return startWithSavedToken(account)
    }

    /** 登出。 */
    fun logout(allDevices: Boolean = false) {
        val current = handle
        if (current != 0L && accountReady) NativeBindings.nativeAccountLogout(current, allDevices)
    }

    /** 取消进行中的认证（连点/放弃）。 */
    fun cancelAuthentication() {
        val current = handle
        if (current != 0L && accountReady) NativeBindings.nativeAccountCancel(current)
    }

    // ---------------- P6-T05/S6：数据库生命周期与影子迁移 ----------------
    // 本轮只暴露迁移/自检入口，默认 Backend 与 UI 路径保持不变；key 经平台密钥桥获取，
    // 不做任意 SQL / 裸句柄 / key getter。

    /**
     * 打开账号影子库（经平台密钥桥取 key，Token-only 冷启动无 passHash 时保持锁定）。
     */
    fun openAccountDatabase(ownerId: Int, filesDir: String, bridge: NativeDbKeyPlatform): Boolean {
        val current = handle
        if (current == 0L || !NativeBindings.isLoaded) return false
        return NativeBindings.nativeOpenAccountDatabase(current, ownerId, filesDir, bridge)
    }

    /** 关闭当前账号数据库（只关库，不删数据）。 */
    fun closeAccountDatabase() {
        val current = handle
        if (current != 0L) NativeBindings.nativeCloseAccountDatabase(current)
    }

    /** 开始影子导入（Disabled → ShadowImport；已 Verified 拒绝）。 */
    fun beginMigration(): Boolean {
        val current = handle
        if (current == 0L) return false
        return NativeBindings.nativeBeginMigration(current)
    }

    /** 回滚到 ShadowImport（清完成标记与 checkpoint，不删生产数据）。 */
    fun resetMigration(): Boolean {
        val current = handle
        if (current == 0L) return false
        return NativeBindings.nativeResetMigration(current)
    }

    /** 提交一个迁移批次。 */
    fun submitMigrationBatch(batch: ByteArray, checkpoint: String): String {
        val current = handle
        if (current == 0L) return "err|NotOpen|句柄无效"
        return NativeBindings.nativeSubmitMigrationBatch(current, batch, checkpoint)
    }

    /** 提交一个会话元数据批次（unread/lastMsg/lastTs，权威覆盖）。 */
    fun submitConversationBatch(batch: ByteArray, checkpoint: String): String {
        val current = handle
        if (current == 0L) return "err|NotOpen|句柄无效"
        return NativeBindings.nativeSubmitConversationBatch(current, batch, checkpoint)
    }

    fun seedLegacyDeltaCheckpoint(epoch:Long,baseline:Long,updatedAt:Long):String {
        val current=handle
        if(current==0L)return "err|NotOpen|句柄无效"
        return NativeBindings.nativeSeedLegacyDeltaCheckpoint(current,epoch,baseline,updatedAt)
    }

    fun getLegacyDeltaCheckpoint(epoch:Long):String {
        val current=handle
        if(current==0L)return "err|NotOpen|句柄无效"
        return NativeBindings.nativeGetLegacyDeltaCheckpoint(current,epoch)
    }

    fun submitLegacyDeltaBatch(epoch:Long,expectedAfter:Long,batch:ByteArray):String {
        val current=handle
        if(current==0L)return "err|NotOpen|句柄无效"
        return NativeBindings.nativeSubmitLegacyDeltaBatch(current,epoch,expectedAfter,batch)
    }

    fun getCutoverJournal():String {
        val current=handle
        if(current==0L)return "err|NotOpen|句柄无效"
        return NativeBindings.nativeGetCutoverJournal(current)
    }

    fun advanceCutoverJournal(epoch:Long,expectedState:Int,targetState:Int,highWater:Long,
        schemaVersion:Int,keyId:String,summary:String,updatedAt:Long):String {
        val current=handle
        if(current==0L)return "err|NotOpen|句柄无效"
        return NativeBindings.nativeAdvanceCutoverJournal(current,epoch,expectedState,targetState,
            highWater,schemaVersion,keyId,summary,updatedAt)
    }

    /** 全量对账并写完成标记。 */
    fun finishMigration(
        expectedMessages: Long,
        expectedConversations: Long,
        expectedMinSeq: Long,
        expectedMaxSeq: Long,
        expectedFts: Long,
    ): String {
        val current = handle
        if (current == 0L) return "err|NotOpen|句柄无效"
        return NativeBindings.nativeFinishMigration(
            current, expectedMessages, expectedConversations, expectedMinSeq, expectedMaxSeq,
            expectedFts
        )
    }

    /** 查询迁移状态。 */
    fun getMigrationState(): String {
        val current = handle
        if (current == 0L) return "err|NotOpen|句柄无效"
        return NativeBindings.nativeGetMigrationState(current)
    }

    /** 数据库自检。 */
    fun runDatabaseSelfTest(): String {
        val current = handle
        if (current == 0L) return "err|NotOpen|句柄无效"
        return NativeBindings.nativeRunDatabaseSelfTest(current)
    }

    /**
     * 搜索当前账号的本地消息。conversationId=0 表示跨会话；高亮下标均为 Kotlin String
     * 使用的 UTF-16 单元，可直接交给 AnnotatedString。解析失败时 fail-close 返回空列表。
     */
    fun searchMessages(keyword: String, conversationId: Long = 0, limit: Int = 100): List<SearchHit> {
        val current = handle
        if (current == 0L || keyword.isBlank() || conversationId < 0 || limit !in 1..100) return emptyList()
        val bytes = NativeBindings.nativeSearchMessages(current, conversationId, keyword, limit)
            ?: return emptyList()
        return decodeSearchHits(bytes) ?: emptyList()
    }

    /** 按新到旧读取会话历史；nextCursor 原样用于下一页，禁止由 UI 自行拼游标。 */
    fun loadHistory(
        conversationId: Long,
        limit: Int = 50,
        cursor: HistoryCursor? = null,
    ): HistoryPage? {
        val current = handle
        if (current == 0L || conversationId <= 0 || limit !in 1..200 ||
            (cursor != null && cursor.msgId.isBlank())) return null
        val raw = NativeBindings.nativeLoadHistory(
            current, conversationId, limit, cursor != null,
            cursor?.serverTime ?: 0, cursor?.conversationSeq ?: 0,
            cursor?.localOrder ?: 0, cursor?.msgId.orEmpty(),
        ) ?: return null
        return decodeHistoryPage(raw)
    }

    /** 读取当前账号的会话快照；顺序由内核定义，UI 不二次推导最后消息。 */
    fun loadConversations(): List<NativeConversation>? {
        val current=handle;if(current==0L)return null
        val raw=NativeBindings.nativeLoadConversations(current)?:return null
        return runCatching {
            val input=ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN)
            fun need(n:Int)=require(n>=0&&input.remaining()>=n)
            fun long():Long{need(8);return input.long}
            fun string():String{need(4);val n=input.int;require(n in 0..1_048_576);need(n)
                val bytes=ByteArray(n);input.get(bytes);return Charsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(ByteBuffer.wrap(bytes)).toString()}
            need(12);require(input.int==0x4A54434C);require(input.int==1)
            val count=input.int;require(count in 0..100_000)
            List(count){NativeConversation(long(),long(),long(),string(),long(),long())}
                .also { require(!input.hasRemaining()) }
        }.getOrNull()
    }

    private fun decodeHistoryPage(bytes: ByteArray): HistoryPage? = runCatching {
        val input = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        fun need(size: Int) = require(size >= 0 && input.remaining() >= size)
        fun string(): String {
            need(4); val size=input.int; require(size in 0..1_048_576); need(size)
            val raw=ByteArray(size); input.get(raw)
            return Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(raw)).toString()
        }
        fun long(): Long { need(8); return input.long }
        fun int(): Int { need(4); return input.int }
        need(16); require(input.int==0x4A544850); require(input.int==1)
        val hasMore=int().also { require(it==0||it==1) }==1
        val count=int(); require(count in 0..200)
        val messages=ArrayList<NativeMessage>(count)
        repeat(count) {
            val owner=long(); val msgId=string(); val conversation=long(); val peer=long()
            val seq=long(); val time=long(); val order=long(); val fromMe=int().also{require(it==0||it==1)}==1
            val type=int(); val content=string(); val status=int(); val pinyin=string(); val initials=string()
            val mediaPath=string(); val width=int(); val height=int(); val fileId=string(); val fileName=string()
            val fileSize=long(); val contentType=string(); val sha256=string()
            val thumbId=string(); val thumbPath=string(); val thumbSize=long(); val thumbHash=string()
            val thumbW=int(); val thumbH=int(); val largeId=string(); val largePath=string()
            val largeSize=long(); val largeHash=string(); val largeW=int(); val largeH=int()
            val localPath=string(); val transferred=long()
            require(owner>0 && msgId.isNotEmpty() && conversation>0 && peer>0)
            val media=NativeMedia(mediaPath,width,height,fileId,fileName,fileSize,contentType,sha256,
                thumbId,thumbPath,thumbSize,thumbHash,thumbW,thumbH,largeId,largePath,largeSize,
                largeHash,largeW,largeH,localPath,transferred)
            messages+=NativeMessage(owner,msgId,conversation,peer,seq,time,order,fromMe,type,content,
                status,pinyin,initials,media)
        }
        require(!input.hasRemaining())
        val last=messages.lastOrNull()
        val next=last?.let { HistoryCursor(it.serverTime,it.conversationSeq,it.localOrder,it.msgId) }
        HistoryPage(messages,hasMore,next)
    }.getOrNull()

    private fun decodeSearchHits(bytes: ByteArray): List<SearchHit>? = runCatching {
        val input = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        fun requireBytes(size: Int) { require(size >= 0 && input.remaining() >= size) }
        fun readString(): String {
            requireBytes(Int.SIZE_BYTES)
            val size = input.int
            require(size in 0..1_048_576)
            requireBytes(size)
            val raw = ByteArray(size)
            input.get(raw)
            val decoder = Charsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
            return decoder.decode(ByteBuffer.wrap(raw)).toString()
        }
        requireBytes(12)
        require(input.int == 0x4A545352)
        require(input.int == 1)
        val count = input.int
        require(count in 0..100)
        val result = ArrayList<SearchHit>(count)
        repeat(count) {
            val msgId = readString()
            requireBytes(8 * 3)
            val conversation = input.long
            val peer = input.long
            val timestamp = input.long
            val snippet = readString()
            requireBytes(8)
            require(input.int == 0) // 当前契约只接受 UTF-16 下标
            val rangeCount = input.int
            require(rangeCount in 0..1_024)
            val ranges = ArrayList<SearchRange>(rangeCount)
            repeat(rangeCount) {
                requireBytes(8)
                val start = input.int
                val end = input.int
                require(start >= 0 && end >= start && end <= snippet.length)
                ranges += SearchRange(start, end)
            }
            result += SearchHit(msgId, conversation, peer, timestamp, snippet, ranges)
        }
        require(!input.hasRemaining())
        result
    }.getOrNull()

    // ---------------- P7-G3：账号级 Runtime 生命周期 ----------------

    /**
     * 创建账号级 Runtime（同账号唯一）。
     *
     * Runtime 拥有业务服务、数据库与 completion executor；
     * 本方法只创建并挂载，不启动。
     */
    fun createRuntime(ownerId: Int): Boolean {
        val current = handle
        if (current == 0L || !NativeBindings.isLoaded) return false
        if(!NativeBindings.nativeCreateRuntime(current, ownerId))return false
        runtimeOwnerId=ownerId.toLong();runtimeGeneration=0;_dataVersions.value=emptyMap()
        return NativeBindings.nativeSetRuntimeEventSink(current,runtimeSink).also { ok ->
            if(!ok)runtimeOwnerId=0
        }
    }

    /** 启动 Runtime；幂等。返回 runtimeGeneration（0 表示失败）。 */
    fun startRuntime(): Long {
        val current = handle
        if (current == 0L) return 0L
        return NativeBindings.nativeStartRuntime(current).also { runtimeGeneration=it }
    }

    /** 停止 Runtime（保留对象，可再次 start）。 */
    fun stopRuntime() {
        val current = handle
        if (current != 0L) NativeBindings.nativeStopRuntime(current)
        runtimeGeneration=0
    }

    /** 登出：停止并递增 generation，使在途旧 generation 事件失效。 */
    fun logoutRuntime() {
        val current = handle
        if (current != 0L) NativeBindings.nativeLogoutRuntime(current)
        runtimeGeneration=0;runtimeOwnerId=0;_dataVersions.value=emptyMap()
    }

    /** 摘除账号 Runtime，保留认证会话；用于登出后换号，避免复用旧 owner Runtime。 */
    fun destroyRuntime() {
        val current = handle
        runtimeGeneration=0;runtimeOwnerId=0;_dataVersions.value=emptyMap()
        if (current != 0L) NativeBindings.nativeDestroyRuntime(current)
    }

    /** Runtime 状态名：Idle/Starting/Running/Stopping/Stopped/Destroyed/None。 */
    fun runtimeState(): String {
        val current = handle
        if (current == 0L) return "None"
        return NativeBindings.nativeGetRuntimeState(current)
    }

    data class SendTextReceipt(
        val accepted: Boolean, val operationId: String, val msgId: String,
        val localOrder: Long, val error: String? = null,
    )

    /** 文本发送意图：Native 先事务落库和 Outbox，commit 后才尝试经唯一安全连接发送。 */
    fun sendText(conversationId: Long, peerId: Long, text: String): SendTextReceipt {
        val current=handle
        if(current==0L||conversationId<=0||peerId<=0||text.isBlank())
            return SendTextReceipt(false,"","",0,"invalid send intent")
        // 当前沿用 Android 已交付的 TinyPinyin 字典生成派生索引；它不是权威消息字段。
        val tokens=PinyinIndex.of(text)
        val raw=NativeBindings.nativeRuntimeSendText(
            current,conversationId,peerId,text,tokens.full,tokens.initials)
        val parts=raw.split('|',limit=5)
        return if(parts.firstOrNull()=="ok"&&parts.size==4)
            SendTextReceipt(true,parts[1],parts[2],parts[3].toLongOrNull()?:0)
        else SendTextReceipt(false,parts.getOrElse(1){""},parts.getOrElse(2){""},0,
            parts.getOrElse(3){"native send rejected"})
    }

    fun beginMediaOperation():Long = handle.takeIf { it!=0L }
        ?.let(NativeBindings::nativeBeginMediaOperation) ?: 0L

    data class RecoverableDownload(val taskId:String,val msgId:String,val fileId:String,
        val localPath:String,val sha256:String,val totalSize:Long,val transferred:Long,
        val generation:Long)

    fun beginDownloadTask(taskId:String,msgId:String,fileId:String,localPath:String):Long {
        val current=handle
        val generation=runtimeGeneration
        return if(current!=0L&&generation>0&&NativeBindings.nativeBeginDownloadTask(
            current,taskId,msgId,fileId,localPath,generation))generation else 0L
    }

    /** 1=running, 2=retryable failure, 3=completed, 4=cancelled, 5=abandoned. */
    fun finishDownloadTask(taskId:String,generation:Long,state:Int,transferred:Long):Boolean {
        val current=handle
        return current!=0L&&generation>0&&NativeBindings.nativeFinishDownloadTask(
            current,taskId,generation,state,transferred.coerceAtLeast(0))
    }

    fun recoverableDownloads():List<RecoverableDownload> {
        val current=handle;if(current==0L)return emptyList()
        val raw=NativeBindings.nativeListRecoverableDownloads(current)?:return emptyList()
        return decodeList(raw,0x4A54444C){ input,string ->
            val taskId=string();val msgId=string();val fileId=string()
            val path=string();val hash=string()
            require(input.remaining()>=24)
            RecoverableDownload(taskId,msgId,fileId,path,hash,input.long,input.long,input.long)
        } ?: emptyList()
    }

    fun cancelMediaOperation(operation:Long) {
        if(handle!=0L&&operation>0)NativeBindings.nativeCancelMediaOperation(handle,operation)
    }

    fun endMediaOperation(operation:Long) {
        if(handle!=0L&&operation>0)NativeBindings.nativeEndMediaOperation(handle,operation)
    }

    fun uploadMedia(path:String,receiverId:Long,isImage:Boolean,operation:Long):String {
        val current=handle
        return if(current==0L||operation<=0||receiverId<=0||path.isBlank())""
        else NativeBindings.nativeRuntimeUploadMedia(current,operation,path,receiverId,isImage)
    }

    fun downloadMedia(fileId:String,destination:String,operation:Long):Boolean {
        val current=handle
        return current!=0L&&operation>0&&fileId.isNotBlank()&&destination.isNotBlank()&&
            NativeBindings.nativeRuntimeDownloadMedia(current,operation,fileId,destination)
    }

    // ---------------- 分片上传（断点续传） ----------------
    /** variant：0=原图/文件 1=大缩略图 2=小缩略图（MediaVariant 序）。 */
    fun enqueueUpload(msgId:String,conversationId:Long,peerId:Long,variant:Int,
                      localPath:String,fileName:String,contentType:String,
                      imageWidth:Int=0,imageHeight:Int=0):Boolean {
        val current=handle
        if(current==0L||msgId.isBlank()||conversationId<=0||peerId<=0||localPath.isBlank())return false
        return NativeBindings.nativeMediaEnqueueUpload(current,msgId,conversationId,peerId,
            variant,localPath,fileName,contentType,imageWidth,imageHeight)=="ok"
    }

    /** 推进上传直到 Sent/Failed（阻塞，IO 线程调用）。 */
    fun pumpUpload(msgId:String):Boolean {
        val current=handle
        return current!=0L&&msgId.isNotBlank()&&
            NativeBindings.nativeMediaPumpUpload(current,msgId)=="ok"
    }

    /** 启动/网络恢复时恢复全部活跃上传草稿（阻塞，IO 线程调用）。 */
    fun resumeUploads():Int {
        val current=handle
        return if(current==0L)0 else NativeBindings.nativeMediaResumeUploads(current)
    }

    /** 用户主动取消（终态，不自动恢复）。 */
    fun cancelUpload(msgId:String):Boolean {
        val current=handle
        return current!=0L&&msgId.isNotBlank()&&
            NativeBindings.nativeMediaCancelUpload(current,msgId)
    }

    /** 上传进度快照；无草稿返回 null。 */
    fun uploadState(msgId:String):String? {
        val current=handle
        if(current==0L||msgId.isBlank())return null
        return NativeBindings.nativeMediaUploadState(current,msgId).takeIf{it.isNotEmpty()}
    }

    fun sendMedia(conversationId:Long,peerId:Long,type:Int,fileId:String,fileName:String,
        fileSize:Long,contentType:String,sha256:String,width:Int=0,height:Int=0,localPath:String="",
        thumbnailFileId:String="",thumbnailSize:Long=0,thumbnailSha256:String="",
        thumbnailWidth:Int=0,thumbnailHeight:Int=0,largeThumbnailFileId:String="",
        largeThumbnailSize:Long=0,largeThumbnailSha256:String="",largeThumbnailWidth:Int=0,
        largeThumbnailHeight:Int=0):SendTextReceipt {
        val current=handle
        if(current==0L)return SendTextReceipt(false,"","",0,"runtime unavailable")
        val raw=NativeBindings.nativeRuntimeSendMedia(current,conversationId,peerId,type,fileId,
            fileName,fileSize,contentType,sha256,width,height,localPath,thumbnailFileId,thumbnailSize,
            thumbnailSha256,thumbnailWidth,thumbnailHeight,largeThumbnailFileId,largeThumbnailSize,
            largeThumbnailSha256,largeThumbnailWidth,largeThumbnailHeight)
        val parts=raw.split('|',limit=5)
        return if(parts.firstOrNull()=="ok"&&parts.size==4)
            SendTextReceipt(true,parts[1],parts[2],parts[3].toLongOrNull()?:0)
        else SendTextReceipt(false,parts.getOrElse(1){""},parts.getOrElse(2){""},0,
            parts.getOrElse(3){"native media rejected"})
    }

    /** 已读意图：Native 原子更新 read watermark/unread，并通过 operationEvents 返回终态。 */
    fun markRead(conversationId:Long,readSeq:Long):String? {
        val current=handle;if(current==0L||conversationId<=0||readSeq<0)return null
        return NativeBindings.nativeRuntimeMarkRead(current,conversationId,readSeq).ifBlank { null }
    }

    /** 网络恢复后主动驱动到期 Outbox；重试仍复用原 msgId。 */
    fun flushOutbox(nowSeconds: Long=System.currentTimeMillis()/1000,limit: Int=32): Int {
        val current=handle
        return if(current==0L||nowSeconds<0||limit !in 1..64)0
        else NativeBindings.nativeRuntimeFlushOutbox(current,nowSeconds,limit)
    }

    enum class DataDomain(val nativeValue: Int) {
        Conversations(0), Messages(1), Friends(2), Transfers(3), Account(4),
    }

    /**
     * 消费该数据域最新一次失效版本。0 表示没有新变化；大于 0 时 UI 应重新查询快照。
     * 多次更新允许合并，避免把每条网络事件逐个跨 JNI 推送造成背压。
     */
    fun consumeInvalidation(domain: DataDomain): Long {
        val current=handle
        return if(current==0L)0 else
            NativeBindings.nativeRuntimeConsumeInvalidation(current,domain.nativeValue)
    }

    enum class OperationStatus { Ok, Cancelled, Failed, Timeout }
    data class OperationCompletion(val status: OperationStatus,val error: String?)

    /**
     * 消费一次命令终态；null 表示仍在等待 ACK，返回非 null 后同一 operationId 不再重复投递。
     */
    fun consumeOperation(operationId: String): OperationCompletion? {
        val current=handle
        if(current==0L||operationId.isBlank())return null
        val raw=NativeBindings.nativeRuntimeConsumeOperation(current,operationId)
        if(!raw.startsWith("done|"))return null
        val parts=raw.split('|',limit=3)
        val status=OperationStatus.values().getOrNull(parts.getOrNull(1)?.toIntOrNull()?:-1)
            ?: return null
        return OperationCompletion(status,parts.getOrNull(2)?.ifBlank { null })
    }

    /** 登录完成后拉取服务端每个会话的最后一条消息，响应由 Native 直接合并入库。 */
    fun requestRoamConversations(): Boolean {
        val current=handle
        return current!=0L&&NativeBindings.nativeRuntimeRequestRoamConversations(current)
    }

    /** 好友事实快照；Presence 只在 Native 内存合并，不写入好友事实表。 */
    fun loadFriends(): List<NativeFriend>? {
        val current = handle
        if (current == 0L) return null
        val raw = NativeBindings.nativeRuntimeLoadFriends(current) ?: return null
        return decodeList(raw, 0x4A544644) { input, string ->
            val id=input.long
            val nick=string();val tel=string();val avatar=string();val signature=string()
            val sex=input.int;val online=input.int.also { require(it==0||it==1) }==1
            require(id>0)
            NativeFriend(id,nick,tel,avatar,signature,sex,online)
        }
    }

    fun loadFriendRequests(): List<NativeFriendRequest>? {
        val current = handle
        if (current == 0L) return null
        val raw = NativeBindings.nativeRuntimeLoadFriendRequests(current) ?: return null
        return decodeList(raw, 0x4A544652) { input, string ->
            val requestId=string();val from=input.long;val to=input.long
            val direction=input.int;val state=input.int;val message=string();val created=input.long
            require(requestId.isNotEmpty()&&from>0&&to>0&&direction in 0..1&&state in 0..3)
            NativeFriendRequest(requestId,from,to,direction,state,message,created)
        }
    }

    fun requestFriendRequests(): Boolean {
        val current=handle
        return current!=0L&&NativeBindings.nativeRuntimeRequestFriendRequests(current)
    }

    fun sendAddFriend(nick: String): Boolean {
        val current=handle
        return current!=0L&&nick.isNotBlank()&&NativeBindings.nativeRuntimeSendAddFriend(current,nick.trim())
    }

    fun answerFriend(requesterId:Long,requesterNick:String,agree:Boolean):Boolean {
        val current=handle
        return current!=0L&&requesterId>0&&NativeBindings.nativeRuntimeAnswerFriend(
            current,requesterId,requesterNick,agree)
    }

    fun deleteFriend(friendId:Long):Boolean {
        val current=handle
        return current!=0L&&friendId>0&&NativeBindings.nativeRuntimeDeleteFriend(current,friendId)
    }

    private fun <T> decodeList(
        raw: ByteArray,
        magic: Int,
        read: (ByteBuffer, () -> String) -> T,
    ): List<T>? = runCatching {
        val input=ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN)
        fun need(size:Int)=require(size>=0&&input.remaining()>=size)
        fun string():String {
            need(4);val size=input.int;require(size in 0..1_048_576);need(size)
            val bytes=ByteArray(size);input.get(bytes)
            return Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(bytes)).toString()
        }
        need(12);require(input.int==magic);require(input.int==1)
        val count=input.int;require(count in 0..100_000)
        List(count){read(input,::string)}.also { require(!input.hasRemaining()) }
    }.getOrNull()

    /** 按服务端 conversation seq 游标拉取历史；首次可传 Long.MAX_VALUE。 */
    fun requestRoamMessages(peerId: Long,beforeSeq: Long=Long.MAX_VALUE,limit: Int=20): Boolean {
        val current=handle
        return current!=0L&&peerId>0&&beforeSeq>0&&limit in 1..100&&
            NativeBindings.nativeRuntimeRequestRoamMessages(current,peerId,beforeSeq,limit)
    }
}
