package com.jitong.im.ui

import android.R
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.jitong.im.data.ChatStore
import com.jitong.im.data.Prefs
import com.jitong.im.data.crypto.DbKeyManager
import com.jitong.im.data.db.ConversationEntity
import com.jitong.im.data.db.MessageEntity
import com.jitong.im.net.ImClient
import com.jitong.im.net.HttpMediaClient
import com.jitong.im.net.Protocol
import com.jitong.im.net.sha256Hex
import im.proto.Im
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.withLock
import java.util.UUID

enum class Screen { Login, FriendList, Chat, ChatSearch }

data class Friend(
    val id: Int,
    val nick: String,
    val feeling: String,
    val online: Boolean,
)

//添加好友请求和回复

enum class MsgKind { TEXT, IMAGE, FILE }

data class ChatMessage(
    val msgId: String,
    val peerId: Int,
    val fromMe: Boolean,
    val text: String = "",
    val kind: MsgKind = MsgKind.TEXT,
    val imageBytes: ByteArray? = null,
    val imgW: Int = 0,
    val imgH: Int = 0,
    val ts: Long = System.currentTimeMillis(),
    val seq: Long = 0,      // 会话级序列号（服务端分配）；本端刚发出、未确认的消息为 0，排在末尾
    val fileId: String = "",
    val fileName: String = "",
    val fileSize: Long = 0,
    val contentType: String = "",
    val sha256: String = "",
    val localPath: String? = null,
    val transferred: Int = 0,
    val status: Status = Status.RECEIVED,
) {
    enum class Status { SENDING, DELIVERED, OFFLINE_STORED, RECEIVED, FAILED }
}

sealed interface AiReplyUiState {
    data object Idle : AiReplyUiState
    data class Loading(val requestId: String, val attempt: Int) : AiReplyUiState
    data class Suggestions(val requestId: String, val items: List<String>) : AiReplyUiState
    data class Error(val message: String, val canRetry: Boolean = true) : AiReplyUiState
}

class MainViewModel : ViewModel() {

    private val client = ImClient()
    private val mediaClient = HttpMediaClient()
    private var store: ChatStore? = null
    private var storeOwnerId: Int? = null // store 是给哪个账号开的（换号登录时需要重开，不能沿用旧账号的 ChatStore）

    /** 当前登录账号 id（未登录为 0） */
    val myId: Int get() = client.myId

    private val _screen = MutableStateFlow(Screen.Login)
    val screen: StateFlow<Screen> = _screen

    private val _loginTip = MutableStateFlow("")
    val loginTip: StateFlow<String> = _loginTip

    private val _myNick = MutableStateFlow("")
    val myNick: StateFlow<String> = _myNick

    private val _myFeeling = MutableStateFlow("")
    val myFeeling: StateFlow<String> = _myFeeling

    /** 好友列表：在线的排前面 */
    private val _friends = MutableStateFlow<List<Friend>>(emptyList())
    val friends: StateFlow<List<Friend>> = _friends

    /**添加好友*/
    private val _friendRequests = MutableStateFlow<List<ImClient.Event.FriendRequestItem>>(emptyList())
    val friendRequests: StateFlow<List<ImClient.Event.FriendRequestItem>> = _friendRequests
    // 兼容尚未携带 operator_id 的旧服务端：记录本机刚发起删除的目标。
    private var pendingDeleteFriendId: Int? = null

    /** 会话消息：peerId -> 有序消息列表（登录后从 Room 装载，运行期内存驻留） */
    private val _messages = MutableStateFlow<Map<Int, List<ChatMessage>>>(emptyMap())
    val messages: StateFlow<Map<Int, List<ChatMessage>>> = _messages

    /** 会话行：peerId -> 最后消息/未读数（好友列表行展示用） */
    private val _conversations = MutableStateFlow<Map<Int, ConversationEntity>>(emptyMap())
    val conversations: StateFlow<Map<Int, ConversationEntity>> = _conversations

    private val _chatPeer = MutableStateFlow<Friend?>(null)
    val chatPeer: StateFlow<Friend?> = _chatPeer

    private val _aiReplyState = MutableStateFlow<AiReplyUiState>(AiReplyUiState.Idle)
    val aiReplyState: StateFlow<AiReplyUiState> = _aiReplyState
    private var aiTimeoutJob: kotlinx.coroutines.Job? = null
    private var aiAttempt = 0
    private var lastAiTone = "自然、简洁"

    /** 搜索结果（聊天记录） */
    private val _searchResults = MutableStateFlow<List<MessageEntity>>(emptyList())
    val searchResults: StateFlow<List<MessageEntity>> = _searchResults

    /** 当前打开的单聊内搜索结果，与主页全局搜索相互独立。 */
    private val _conversationSearchResults = MutableStateFlow<List<MessageEntity>>(emptyList())
    val conversationSearchResults: StateFlow<List<MessageEntity>> = _conversationSearchResults
    private var conversationSearchJob: kotlinx.coroutines.Job? = null

    /** 从搜索结果返回聊天页后需要定位的消息；ChatScreen 消费完成后清空。 */
    private val _chatJumpTarget = MutableStateFlow<String?>(null)
    val chatJumpTarget: StateFlow<String?> = _chatJumpTarget

    private val _toast = MutableSharedFlow<String>(extraBufferCapacity = 8)
    val toast: SharedFlow<String> = _toast

    private var expectDisconnect = false
    private var lastTel: String? = null
    private var lastHash: String? = null // 仅用于当前进程内打开本地加密 DB，不再作为网络重连凭证
    private var pendingRemember = false // 本次登录是否勾选“记住账号密码”
    private var pendingPass: String? = null // 本次登录的明文密码（记住时持久化用于回填）

    /** 断线自动重连任务与状态（防止并发重连、登录成功后清零退避） */
    private var reconnectJob: kotlinx.coroutines.Job? = null
    private var reconnecting = false
    private val refreshMutex = kotlinx.coroutines.sync.Mutex()
    private var refreshRequestInFlight = false

    /** 漫游分页状态：每会话已加载最小 seq（上拉游标）、是否还有更早、是否正在加载（防抖） */
    private val loadedMinSeq = mutableMapOf<Int, Long>()
    private val roamHasMore = mutableMapOf<Int, Boolean>()
    private val roamLoading = mutableSetOf<Int>()

    init {
        viewModelScope.launch { collectEvents() }
    }

    /**
     * 登录成功后打开（或复用）本地库：密钥由 DbKeyManager 用刚登录的密码哈希派生/解出，
     * 只有登录成功那一刻才拿得到，所以本地库不能在登录前初始化。
     * 涉及磁盘 I/O + PBKDF2，切到 IO 线程，避免卡主线程。
     */
    private suspend fun openStore(ownerId: Int): ChatStore {
        store?.let { if (storeOwnerId == ownerId) return it }
        val ctx = appContext ?: error("应用上下文未初始化")
        val passHash = lastHash ?: error("缺少登录密码哈希，无法派生本地库密钥")
        val opened = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
            val key = DbKeyManager.getOrCreateRealKey(ctx, ownerId, passHash)
            ChatStore(ctx, ownerId, key)
        }
        store = opened
        storeOwnerId = ownerId
        return opened
    }

    // 本地库（ChatStore）现在需要密码派生的密钥才能打开，改由 attachAndOpenStore()
    // 在登录成功后自行构建，不再由 MainActivity 提前注入。

    // ---------------- 登录 / 注册 ----------------

    fun login(tel: String, pass: String, remember: Boolean) {
        val normalizedTel = tel.trim()
        val validationError = validateCredentials(normalizedTel, pass)
        if (validationError != null) {
            _loginTip.value = validationError
            return
        }
        lastTel = normalizedTel
        lastHash = sha256Hex(pass) // 仅供本地 DB 密钥派生使用
        Prefs.clearTokenSession()  // 显式密码登录视为开始一个新的认证会话
        Prefs.pendingRefreshRequestId = null
        pendingRemember = remember // 登录成功后据此决定是否持久化账号密码
        pendingPass = pass         // 记住时保存的明文密码（用于登录页回填）
        viewModelScope.launch {
            _loginTip.value = "连接服务器…"
            if (!client.connect(ImClient.DEFAULT_HOST)) {
                _loginTip.value = "连接失败，请确认 im_server 已启动"
                return@launch
            }
            _loginTip.value = "登录中…"
            client.login(tel = normalizedTel, pass = pass, deviceId = Prefs.deviceId,)
        }
    }

    fun register(nick: String, tel: String, pass: String) {
        val normalizedNick = nick.trim()
        val normalizedTel = tel.trim()
        if (normalizedNick.isEmpty()) {
            _loginTip.value = "请输入昵称"
            return
        }
        if (normalizedNick.toByteArray(Charsets.UTF_8).size >= 30) {
            _loginTip.value = "昵称过长，请控制在 29 个字节以内"
            return
        }
        val validationError = validateCredentials(normalizedTel, pass)
        if (validationError != null) {
            _loginTip.value = validationError
            return
        }
        viewModelScope.launch {
            _loginTip.value = "连接服务器…"
            if (!client.connect(ImClient.DEFAULT_HOST)) {
                _loginTip.value = "连接失败，请确认 im_server 已启动"
                return@launch
            }
            _loginTip.value = "注册中…"
            client.register(normalizedNick, normalizedTel, pass)
        }
    }

    private fun validateCredentials(tel: String, pass: String): String? {
        if (!tel.matches(Regex("^1[3-9]\\d{9}$"))) return "请输入正确的 11 位手机号"
        // 兼容项目现有的 6 位演示账号；新生产系统建议提升到至少 8 位。
        if (pass.length !in 6..64) return "密码长度应为 6～64 个字符"
        if (pass.isBlank()) return "密码不能全部为空格"
        return null
    }

    // ---------------- 导航 ----------------

    fun openChat(friend: Friend) {
        clearAiReply(cancelRemote = true)
        clearConversationSearch()
        _chatJumpTarget.value = null
        _chatPeer.value = friend
        _screen.value = Screen.Chat
        // 进入会话清零未读（库 + 内存）
        viewModelScope.launch {
            store?.clearUnread(client.myId, friend.id)
            _conversations.value[friend.id]?.let {
                _conversations.value = _conversations.value + (friend.id to it.copy(unread = 0))
            }
        }
        // 漫游游标初始化：本地无历史则拉最近一页；本地已有则以本地最小 seq 为上拉起点，允许继续向上翻更早的
        val local = _messages.value[friend.id].orEmpty()
        if (local.isEmpty()) {
            requestHistory(friend.id, Long.MAX_VALUE)
        } else if (loadedMinSeq[friend.id] == null) {
            val minSeq = local.filter { it.seq > 0 }.minOfOrNull { it.seq }
            if (minSeq != null) {
                loadedMinSeq[friend.id] = minSeq
                roamHasMore[friend.id] = true // 未知，允许尝试上拉；服务端返回空批会置 false
            } else {
                // 本地全是 seq=0 的旧版/未确认消息：拉最新一页重建游标（服务端批次带回 seq）
                requestHistory(friend.id, Long.MAX_VALUE)
            }
        }
    }

    /** 添加好友 */
    fun sendAddFriendRequest(friNick: String) {
        viewModelScope.launch { client.sendAddFriendRq(myNick.value, friNick) }
    }

    fun loadFriendRequests() {
        viewModelScope.launch { client.requestFriendRequests() }
    }

    fun respondFriendRequest(request: ImClient.Event.FriendRequestItem, result: Int) {
        if (request.targetId != client.myId) return
        viewModelScope.launch {
            client.sendAddFriendRs(request.requesterId, request.requesterNick, myNick.value, result)
        }
        _friendRequests.value = _friendRequests.value.filterNot {
            it.requesterId == request.requesterId && it.targetId == request.targetId
        }
    }

    /** 删除当前聊天对象；最终是否删除成功以服务端回执为准。 */
    fun deleteCurrentFriend() {
        val friendId = _chatPeer.value?.id ?: return
        pendingDeleteFriendId = friendId
        viewModelScope.launch {
            runCatching { client.deleteFriend(friendId) }
                .onFailure {
                    pendingDeleteFriendId = null
                    notify("删除请求发送失败：${it.message ?: "连接异常"}")
                }
        }
    }


    /** 上拉加载更早历史：hasMore 且未在加载时才发请求（防抖，修复 6） */
    fun loadMoreHistory(peerId: Int) {
        if (roamHasMore[peerId] == false) return       // 已到头
        if (peerId in roamLoading) return               // 正在加载，避免并发游标错乱
        val cursor = loadedMinSeq[peerId] ?: return     // 尚无首页则不上拉（首页由 openChat 触发）
        requestHistory(peerId, cursor)
    }

    /** 发起一页历史漫游请求（首页 beforeSeq=Long.MAX，上拉传当前最小 seq） */
    private fun requestHistory(peerId: Int, beforeSeq: Long) {
        if (peerId in roamLoading) return
        roamLoading.add(peerId)
        viewModelScope.launch { client.roamMessages(peerId, beforeSeq, PAGE_SIZE) }
    }

    fun backToFriends() {
        clearAiReply(cancelRemote = true)
        clearConversationSearch()
        _chatJumpTarget.value = null
        _screen.value = Screen.FriendList
    }

    fun openChatSearch() {
        if (_chatPeer.value == null) return
        clearConversationSearch()
        _screen.value = Screen.ChatSearch
    }

    fun backToChat() {
        clearConversationSearch()
        _screen.value = Screen.Chat
    }

    fun returnToChatAt(msgId: String) {
        clearConversationSearch()
        _chatJumpTarget.value = msgId
        _screen.value = Screen.Chat
    }

    fun consumeChatJumpTarget() {
        _chatJumpTarget.value = null
    }

    fun logout() {
        expectDisconnect = true
        Prefs.clearCredentialsIfNotRemember()
        val tokenSession = Prefs.loadTokenSession()
        Prefs.clearTokenSession()
        viewModelScope.launch {
            if (tokenSession != null && client.connected) {
                runCatching { client.revokeSession(tokenSession, Prefs.deviceId) }
            }
            client.disconnect()
        }
        resetToLogin()
    }

    // ---------------- 聊天 ----------------

    fun requestAiReply(tone: String = lastAiTone) {
        val peer = _chatPeer.value ?: return
        if (!client.connected) {
            _aiReplyState.value = AiReplyUiState.Error("连接已断开，请稍后重试")
            return
        }
        if (_aiReplyState.value is AiReplyUiState.Loading) return

        // 服务端按 UTF-8 字节限制32；中文最多取10个字符，避免多字节越界。
        lastAiTone = tone.take(10).ifBlank { "自然、简洁" }
        aiAttempt += 1
        val requestId = UUID.randomUUID().toString()
        _aiReplyState.value = AiReplyUiState.Loading(requestId, aiAttempt)
        aiTimeoutJob?.cancel()
        aiTimeoutJob = viewModelScope.launch {
            kotlinx.coroutines.delay(AI_UI_TIMEOUT_MS)
            val loading = _aiReplyState.value as? AiReplyUiState.Loading
            if (loading?.requestId == requestId) {
                runCatching { client.cancelAiReply(requestId) }
                _aiReplyState.value = AiReplyUiState.Error("AI 回复超时，请重试")
            }
        }
        viewModelScope.launch {
            runCatching { client.requestAiReply(peer.id, requestId, lastAiTone, 3) }
                .onFailure {
                    aiTimeoutJob?.cancel()
                    val loading = _aiReplyState.value as? AiReplyUiState.Loading
                    if (loading?.requestId == requestId) {
                        _aiReplyState.value = AiReplyUiState.Error("AI 请求发送失败，请重试")
                    }
                }
        }
    }

    fun retryAiReply() {
        if (_aiReplyState.value is AiReplyUiState.Loading) return
        requestAiReply(lastAiTone)
    }

    fun cancelAiReply() {
        clearAiReply(cancelRemote = true)
    }

    fun dismissAiReplies() {
        clearAiReply(cancelRemote = false)
    }

    private fun clearAiReply(cancelRemote: Boolean) {
        val requestId = (_aiReplyState.value as? AiReplyUiState.Loading)?.requestId
        aiTimeoutJob?.cancel()
        aiTimeoutJob = null
        _aiReplyState.value = AiReplyUiState.Idle
        if (cancelRemote && requestId != null && client.connected) {
            viewModelScope.launch { runCatching { client.cancelAiReply(requestId) } }
        }
    }

    fun send(text: String) {
        val peer = _chatPeer.value ?: return
        if (text.isBlank()) return
        val msgId = UUID.randomUUID().toString()
        android.util.Log.d("IMSEQ", "发送消息 to=${peer.id} msgId=$msgId seq(待服务端分配)=0 text=$text")
        val m = ChatMessage(msgId, peer.id, fromMe = true, text = text, status = ChatMessage.Status.SENDING)
        append(m, incrUnread = false)
        viewModelScope.launch { client.sendChat(peer.id, text, msgId) }
    }

    /** 发送图片（压缩已由调用方完成），本地即时上屏 + 等待回执 */
    fun sendImage(bytes: ByteArray, w: Int, h: Int) {
        val peer = _chatPeer.value ?: return
        if (bytes.isEmpty()) return
        val msgId = UUID.randomUUID().toString()
        val ctx = appContext ?: run { notify("应用尚未初始化"); return }
        val local = java.io.File(ctx.filesDir, "img/outgoing/$msgId.jpg").also {
            it.parentFile?.mkdirs(); it.writeBytes(bytes)
        }
        append(
            ChatMessage(
                msgId, peer.id, fromMe = true, kind = MsgKind.IMAGE,
                imageBytes = bytes, imgW = w, imgH = h, localPath = local.absolutePath,
                fileName = "$msgId.jpg", fileSize = local.length(), contentType = "image/jpeg",
                status = ChatMessage.Status.SENDING,
            ),
            incrUnread = false,
        )
        uploadMedia(msgId, Upload(local, peer.id, "$msgId.jpg", local.length(), true, w, h))
    }

    /** 聊天记录搜索（FTS 前缀 + LIKE 子串） */
    fun search(kw: String) {
        if (kw.isBlank()) {
            _searchResults.value = emptyList()
            return
        }
        val s = store ?: return
        viewModelScope.launch {
            _searchResults.value = s.search(client.myId, kw.trim())
        }
    }

    /** 只搜索当前聊天；取消上一次任务，避免快速输入时旧查询覆盖新结果。 */
    fun searchCurrentConversation(kw: String) {
        conversationSearchJob?.cancel()
        _conversationSearchResults.value = emptyList()
        val peerId = _chatPeer.value?.id
        if (kw.isBlank() || peerId == null) {
            _conversationSearchResults.value = emptyList()
            return
        }
        val s = store ?: return
        conversationSearchJob = viewModelScope.launch {
            kotlinx.coroutines.delay(180)
            _conversationSearchResults.value =
                s.searchConversation(client.myId, peerId, kw.trim())
        }
    }

    fun clearConversationSearch() {
        conversationSearchJob?.cancel()
        conversationSearchJob = null
        _conversationSearchResults.value = emptyList()
    }

    fun notify(msg: String) {
        viewModelScope.launch { _toast.emit(msg) }
    }

    // ---------------- 文件发送（发送方状态机 + 断点续传） ----------------

    /** 进行中的上传：先将 SAF 内容复制到应用私有文件，后续哈希/分片/重试都只读稳定本地副本。 */
    private data class Upload(
        val file: java.io.File, val peerId: Int, val name: String, val size: Long,
        val isImage: Boolean = false, val width: Int = 0, val height: Int = 0,
    )
    private val uploads = java.util.concurrent.ConcurrentHashMap<String, Upload>()

    @Volatile private var appResolver: android.content.ContentResolver? = null
    fun attachResolver(r: android.content.ContentResolver) { appResolver = r }

    @Volatile private var appContext: android.content.Context? = null
    fun attachContext(ctx: android.content.Context) { appContext = ctx.applicationContext }

    fun sendFile(uri: android.net.Uri, resolver: android.content.ContentResolver) {
        val peer = _chatPeer.value ?: return
        val (name, size) = queryNameSize(resolver, uri) ?: return
        if (size <= 0) { notify("空文件无法发送"); return }
        if (size > Protocol.FILE_MAX_SIZE) { notify("文件超过 100MB"); return }
        // OpenDocument 授予的权限默认只覆盖当前进程；持久化后，失败重试和 App 重启后仍可读取。
        runCatching {
            resolver.takePersistableUriPermission(
                uri, android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION,
            )
        }
        val msgId = java.util.UUID.randomUUID().toString()
        append(ChatMessage(msgId, peer.id, fromMe = true, kind = MsgKind.FILE,
            fileName = name, fileSize = size, localPath = uri.toString(),
            status = ChatMessage.Status.SENDING), incrUnread = false)
        prepareLocalUpload(msgId, peer.id, name, uri, resolver)
    }

    /** 失败文件卡片重试：复用原 msgId，服务端据此从已有 .part 水位继续。 */
    fun retryFile(msg: ChatMessage) {
        if (!msg.fromMe || msg.kind != MsgKind.FILE ||
            msg.status != ChatMessage.Status.FAILED) return
        val resolver = appResolver ?: run { notify("文件读取器未初始化"); return }
        val path = msg.localPath ?: run { notify("原文件已不可用，请重新选择"); return }
        updateFileStatus(msg.peerId, msg.msgId, ChatMessage.Status.SENDING)
        if (path.startsWith("content://")) {
            prepareLocalUpload(msg.msgId, msg.peerId, msg.fileName,
                android.net.Uri.parse(path), resolver)
        } else {
            val file = java.io.File(path)
            if (!file.isFile) {
                updateFileStatus(msg.peerId, msg.msgId, ChatMessage.Status.FAILED)
                notify("原文件已不存在，请重新选择")
                return
            }
            val up = Upload(file, msg.peerId, msg.fileName, file.length())
            uploads[msg.msgId] = up
            uploadMedia(msg.msgId, up)
        }
    }

    private fun prepareLocalUpload(msgId: String, peerId: Int, name: String,
                                   uri: android.net.Uri, resolver: android.content.ContentResolver) {
        viewModelScope.launch(kotlinx.coroutines.Dispatchers.IO) {
            runCatching {
                val ctx = appContext ?: error("应用上下文未初始化")
                val dir = java.io.File(ctx.filesDir, "file/outgoing").apply { mkdirs() }
                val safeName = java.io.File(name).name.ifBlank { "file" }
                val local = java.io.File(dir, "${msgId}_$safeName")
                resolver.openInputStream(uri)?.use { input ->
                    java.io.FileOutputStream(local, false).use { output -> input.copyTo(output) }
                } ?: error("无法打开所选文件")
                val actualSize = local.length()
                if (actualSize <= 0L) error("文件为空")
                if (actualSize > Protocol.FILE_MAX_SIZE) error("文件超过100MB")
                val up = Upload(local, peerId, safeName, actualSize)
                uploads[msgId] = up
                updateOutgoingFile(peerId, msgId, local.absolutePath, actualSize)
                uploadMedia(msgId, up)
            }.onFailure {
                notify("文件读取失败：${it.message ?: it.javaClass.simpleName}")
                updateFileStatus(peerId, msgId, ChatMessage.Status.FAILED)
            }
        }
    }

    private fun uploadMedia(msgId: String, up: Upload) {
        uploads[msgId] = up
        viewModelScope.launch(kotlinx.coroutines.Dispatchers.IO) {
            runCatching {
                val mime = if (up.isImage) "image/jpeg" else
                    java.net.URLConnection.guessContentTypeFromName(up.name) ?: "application/octet-stream"
                val result = mediaClient.upload(up.file, up.peerId, up.name, mime) { sent, total ->
                    val percent = if (total > 0) (sent * 100 / total).toInt() else 0
                    viewModelScope.launch { updateFileProgress(up.peerId, msgId, percent) }
                }
                client.sendMediaCard(
                    up.peerId, result.fileId, up.name, result.size, result.contentType,
                    result.sha256, up.isImage, up.width, up.height, msgId,
                )
                updateMediaMetadata(up.peerId, msgId, result.fileId, result.contentType, result.sha256)
            }.onFailure {
                uploads.remove(msgId)
                updateFileStatus(up.peerId, msgId, ChatMessage.Status.FAILED)
                notify("上传失败：${it.message ?: it.javaClass.simpleName}")
            }
        }
    }

    private fun queryNameSize(resolver: android.content.ContentResolver, uri: android.net.Uri): Pair<String, Long>? {
        resolver.query(uri, null, null, null, null)?.use { c ->
            val ni = c.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME)
            val si = c.getColumnIndex(android.provider.OpenableColumns.SIZE)
            if (c.moveToFirst()) {
                val name = if (ni >= 0) c.getString(ni) else "file"
                val size = if (si >= 0) c.getLong(si) else -1L
                if (size >= 0) return name to size
            }
        }
        return null
    }

    // ---------------- 文件接收（接收方状态机 + 断点续传） ----------------

    private val downloads = java.util.concurrent.ConcurrentHashMap.newKeySet<String>()

    /** 用户点击下载：从本地已有的 .part 大小推算续传起点，向服务端请求剩余分片 */
    fun downloadFile(msg: ChatMessage) {
        val ctx = appContext ?: return
        val effectiveFileId = msg.fileId
        if (effectiveFileId.isBlank()) { notify("文件标识缺失"); return }
        if (!downloads.add(effectiveFileId)) {
            notify("文件正在下载，请勿重复点击")
            return
        }
        val dir = java.io.File(ctx.filesDir, if (msg.kind == MsgKind.IMAGE) "img" else "file").apply { mkdirs() }
        // 展示名来自网络，物理文件名必须去掉路径成分，不能直接信任。
        val safeName = java.io.File(msg.fileName).name.ifBlank { "file" }.take(255)
        val finalFile = java.io.File(dir, "${effectiveFileId}_$safeName")
        // 接收方下载完成后是 RECEIVED；发送方只是补回本地副本，不能因此丢失原发送回执状态。
        val completedStatus = if (msg.fromMe) msg.status else ChatMessage.Status.RECEIVED
        updateFileStatus(msg.peerId, msg.msgId, ChatMessage.Status.SENDING) // 复用"进行中"
        viewModelScope.launch(kotlinx.coroutines.Dispatchers.IO) {
            runCatching {
                mediaClient.download(effectiveFileId, finalFile, msg.sha256) { received, total ->
                    val percent = if (total > 0) (received * 100 / total).toInt() else 0
                    viewModelScope.launch { updateFileProgress(msg.peerId, msg.msgId, percent) }
                }
            }.onSuccess {
                downloads.remove(effectiveFileId)
                updateFileLocalPath(msg.peerId, msg.msgId, finalFile.absolutePath)
                updateFileStatus(msg.peerId, msg.msgId, completedStatus)
            }.onFailure {
                downloads.remove(effectiveFileId)
                updateFileStatus(msg.peerId, msg.msgId, completedStatus)
                notify("下载失败：${it.message ?: it.javaClass.simpleName}")
            }
        }
    }

    // ---------------- 事件归集 ----------------

    private suspend fun collectEvents() {
        client.events.collect { e ->
            when (e) {
                is ImClient.Event.RegisterResult -> {
                    _loginTip.value = when (e.result) {
                        Protocol.REGISTER_SUCC -> "注册成功，请登录"
                        Protocol.REGISTER_NICK_EXIT -> "注册失败：昵称已存在"
                        Protocol.REGISTER_TEL_EXIT -> "注册失败：手机号已存在"
                        Protocol.REGISTER_INVALID -> "注册失败：手机号或密码格式不正确"
                        else -> "注册失败：服务器错误"
                    }
                }

                is ImClient.Event.LoginResult -> when (e.result) {
                    Protocol.LOGIN_SUCCESS -> {
                        _loginTip.value = ""
                        e.tokenSession?.let(Prefs::saveTokenSession)
                        // 登录成功：结束任何进行中的重连、清零退避状态
                        reconnecting = false
                        reconnectJob?.cancel()
                        reconnectJob = null
                        // 记住账号密码：勾选则持久化明文账号密码（仅用于登录页回填，不自动登录）；
                        // 未勾选则清除。记住开关本身也持久化。
                        Prefs.remember = pendingRemember
                        if (pendingRemember) {
                            lastTel?.let { Prefs.tel = it }
                            pendingPass?.let { Prefs.pass = it }
                        } else {
                            Prefs.tel = null
                            Prefs.pass = null
                        }
                        _screen.value = Screen.FriendList
                        viewModelScope.launch {
                            val s = openStore(client.myId)
                            val (msgs, convs) = s.hydrate(client.myId)
                            android.util.Log.d("IMDBG", "hydrate done dbMsgs=${msgs.mapValues { it.value.size }} memMsgs=${_messages.value.mapValues { it.value.size }}")
                            // 合并而非覆盖：hydrate 是异步 IO，其间登录补发的离线消息
                            // 可能已经通过 append 进入内存，直接整体赋值会把这些新消息冲掉，
                            // 导致“发送方回执已翻转、接收方却看不到补发消息”。
                            // 以 DB 结果为基底，叠加当前内存里已有的消息（按 msgId 去重）。
                            _messages.value = mergeMessages(msgs, _messages.value)
                            _conversations.value = mergeConversations(convs, _conversations.value)
                            android.util.Log.d("IMDBG", "after merge msgs=${_messages.value.mapValues { it.value.size }}")
                        }
                        // 消息漫游：登录后拉每会话最后一条恢复会话列表预览（换设备/重装后本地库空时尤为关键）
                        viewModelScope.launch { client.roamConversations() }
                        loadFriendRequests()
                    }
                    Protocol.LOGIN_NOTEXIT -> _loginTip.value = "登录失败：用户不存在"
                    Protocol.LOGIN_INVALID -> _loginTip.value = "登录失败：手机号或密码格式不正确"
                    else -> _loginTip.value = "登录失败：密码错误"
                }

                is ImClient.Event.TokenLoginResult -> {
                    if (e.result == Protocol.LOGIN_SUCCESS) {
                        reconnecting = false
                        reconnectJob?.cancel()
                        reconnectJob = null
                        _loginTip.value = ""
                        _toast.emit("已重新连接")
                        viewModelScope.launch { client.roamConversations() }
                    } else {
                        Prefs.clearTokenSession()
                        _toast.emit("登录状态已失效，请重新登录")
                        resetToLogin()
                    }
                }

                is ImClient.Event.TokenRefreshResult -> {
                    refreshMutex.withLock { refreshRequestInFlight = false }
                    val refreshed = e.tokenSession
                    if (e.result == Protocol.REFRESH_TOKEN_SUCCESS && refreshed != null) {
                        Prefs.saveTokenSession(refreshed)
                        Prefs.pendingRefreshRequestId = null
                        viewModelScope.launch {
                            client.loginWithToken(refreshed, Prefs.deviceId)
                        }
                    } else {
                        Prefs.pendingRefreshRequestId = null
                        Prefs.clearTokenSession()
                        _toast.emit("登录状态已过期，请重新登录")
                        resetToLogin()
                    }
                }

                is ImClient.Event.LogoutResult -> Unit

                is ImClient.Event.AiReplyResult -> {
                    val loading = _aiReplyState.value as? AiReplyUiState.Loading
                    if (loading?.requestId == e.requestId && e.done) {
                        aiTimeoutJob?.cancel()
                        aiTimeoutJob = null
                        _aiReplyState.value = if (
                        e.status == Im.AiReplyStatus.AI_REPLY_OK &&
                            e.suggestions.isNotEmpty()
                        ) {
                            AiReplyUiState.Suggestions(e.requestId, e.suggestions.take(3))
                        } else {
                            val message = e.errorMessage.ifBlank {
                                when (e.status) {
                                    Im.AiReplyStatus.AI_REPLY_RATE_LIMITED -> "请求过于频繁，请稍后再试"
                                    Im.AiReplyStatus.AI_REPLY_BUSY -> "AI 正在处理其他请求"
                                    Im.AiReplyStatus.AI_REPLY_NOT_CONFIGURED -> "AI 功能暂不可用"
                                    else -> "AI 回复生成失败"
                                }
                            }
                            AiReplyUiState.Error(message)
                        }
                    }
                }

                is ImClient.Event.UserOrFriendInfo -> {
                    if (e.userId == client.myId) {
                        _myNick.value = e.nick
                        _myFeeling.value = e.feeling
                    } else {
                        upsertFriend(Friend(e.userId, e.nick, e.feeling, e.online))
                        _chatPeer.value?.takeIf { it.id == e.userId }?.let {
                            _chatPeer.value = it.copy(online = e.online)
                        }
                    }
                }

                is ImClient.Event.ChatReceived -> {
                    val inChat = (_screen.value == Screen.Chat || _screen.value == Screen.ChatSearch) &&
                        _chatPeer.value?.id == e.fromId
                    android.util.Log.d("IMDBG", "ChatReceived from=${e.fromId} msgId=${e.msgId} ts=${e.ts} seq=${e.seq} text=${e.text}")
                    append(
                        ChatMessage(e.msgId, e.fromId, fromMe = false, text = e.text, ts = e.ts, seq = e.seq),
                        incrUnread = !inChat,
                    )
                }

                is ImClient.Event.ChatSendResult -> {
                    android.util.Log.d("IMSEQ", "收到回执 peer=${e.peerId} msgId=${e.msgId} 服务端分配 seq=${e.seq} result=${e.result}")
                    val status = when(e.result){
                        Protocol.CHAT_RESULT_SUCC -> ChatMessage.Status.DELIVERED
                        Protocol.CHAT_RESULT_NOT_FRIEND -> {
                            _toast.emit("对方不是你的好友，消息未发送！")
                            ChatMessage.Status.FAILED
                        }
                        else -> ChatMessage.Status.OFFLINE_STORED
                    }
                    updateStatus(
                        e.peerId,
                        e.msgId,
                        status,
                        e.seq,
                    )
                    uploads.remove(e.msgId)
                }

                is ImClient.Event.AddFriendRequestReceived -> {
                    // 在线提醒只触发刷新；申请的权威状态在服务端 friend_requests 表。
                    loadFriendRequests()
                }

                is ImClient.Event.AddFriendResult -> {
                    // 收到了自己加别人后的回执：直接走全局 Toast 通道，不用单独建 SharedFlow
                    val msg = when (e.result) {
                        Protocol.ADD_FRIEND_AGREE -> "${e.peerNick} 已同意你的好友请求"
                        Protocol.ADD_FRIEND_REJECT -> "${e.peerNick} 拒绝了你的好友请求"
                        Protocol.ADD_FRIEND_OFFLINE -> "对方不在线，请求发送失败"
                        Protocol.ADD_FRIEND_NOTEXIT -> "用户 ${e.peerNick} 不存在"
                        Protocol.ADD_FRIEND_SELF -> "不能添加自己为好友"
                        Protocol.ADD_FRIEND_ALREADY -> "对方 ${e.peerNick} 已经是你好友，请勿重复添加"
                        Protocol.ADD_FRIEND_PENDING -> "好友申请已发送，等待对方验证"
                        Protocol.ADD_FRIEND_DB_ERROR -> "好友申请保存失败，请稍后重试"
                        else -> "好友请求处理失败"
                    }
                    if (e.result == Protocol.ADD_FRIEND_AGREE ||
                        e.result == Protocol.ADD_FRIEND_REJECT) {
                        _friendRequests.value = _friendRequests.value.filterNot {
                            (it.requesterId == client.myId && it.targetId == e.peerId) ||
                                (it.requesterId == e.peerId && it.targetId == client.myId)
                        }
                    }
                    notify(msg)
                }

                is ImClient.Event.FriendRequestsLoaded -> {
                    _friendRequests.value = e.requests
                }

                is ImClient.Event.DeleteFriendResult -> {
                    when (e.result) {
                        Protocol.DELETE_FRIEND_SUCCESS -> {
                            val operatorNick = e.operatorNick // 或者 e.nick
                                ?: _friends.value.firstOrNull { it.id == e.operatorId }?.nick // 如果服务端可能为空，可以保留本地查找作为备用方案
                                ?: ""
                            val deletedByMe = e.operatorId == client.myId ||
                                (e.operatorId == 0 && pendingDeleteFriendId == e.friendId)
                            if (pendingDeleteFriendId == e.friendId) pendingDeleteFriendId = null
                            android.util.Log.d(
                                "IM_FRIEND",
                                "delete result myId=${client.myId} friendId=${e.friendId} " +
                                    "operatorId=${e.operatorId} deletedByMe=$deletedByMe",
                            )
                            _friends.value = _friends.value.filterNot {
                                it.id == e.friendId
                            }
                            if (_chatPeer.value?.id == e.friendId) {
                                _chatPeer.value = null
                                _screen.value = Screen.FriendList
                            }
                            notify(
                                if (deletedByMe) {
                                    "已删除好友"
                                } else if (operatorNick.isNotBlank()) {
                                    "你已被 $operatorNick 删除"
                                } else {
                                    "你已被对方删除"
                                }
                            )
                        }
                        Protocol.DELETE_FRIEND_NOT_FRIEND -> {
                            if (pendingDeleteFriendId == e.friendId) pendingDeleteFriendId = null
                            _friends.value = _friends.value.filterNot { it.id == e.friendId }
                            if (_chatPeer.value?.id == e.friendId) {
                                _chatPeer.value = null
                                _screen.value = Screen.FriendList
                            }
                            notify(if (e.result == Protocol.DELETE_FRIEND_SUCCESS) "已删除好友" else "对方已不在好友列表")
                        }
                        Protocol.DELETE_FRIEND_DB_ERROR -> notify("删除失败，请稍后重试")
                        else -> notify("删除好友请求无效")
                    }
                }

                is ImClient.Event.RoamConversations -> {
                    // 会话列表末条：只更新会话预览行（不落消息表，避免"半条无字节图片"抢占 msgId 唯一约束，修复 3）
                    for (item in e.convs) {
                        val peerId = if (item.fromId == client.myId) item.toId else item.fromId
                        val preview = when {
                            item.isImage -> "[图片]"
                            item.isFile -> "[文件] ${item.fileName.ifBlank { "未命名文件" }}"
                            item.isText -> item.text
                            else -> "[暂不支持的消息]"
                        }
                        val old = _conversations.value[peerId]
                        // mergeConversations 语义：仅当更新（ts 更晚）才覆盖，避免冲掉刚到达的更新消息（修复 5）
                        if (old == null || item.ts >= old.lastTs) {
                            _conversations.value = _conversations.value + (
                                peerId to com.jitong.im.data.db.ConversationEntity(
                                    conversationId = 0L, ownerId = client.myId, peerId = peerId,
                                    lastMsg = preview, lastTs = item.ts, unread = old?.unread ?: 0,
                                )
                                )
                        }
                    }
                }

                is ImClient.Event.RoamMessages -> {
                    // 历史分页落地：每条走 append（msgId 幂等 + sortBySeq），但不刷新会话预览（旧消息）
                    for (item in e.msgs) {
                        // 未识别的新消息类型不伪装成文本；等 UI/协议支持后再显式落库展示。
                        if (!item.isText && !item.isImage && !item.isFile) continue
                        val peerId = if (item.fromId == client.myId) item.toId else item.fromId
                        val fromMe = item.fromId == client.myId
                        val historyMessage = ChatMessage(
                                msgId = item.msgId, peerId = peerId, fromMe = fromMe,
                                text = item.text,
                                kind = when {
                                    item.isImage -> MsgKind.IMAGE
                                    item.isFile -> MsgKind.FILE
                                    item.isText -> MsgKind.TEXT
                                    else -> error("已在上方过滤未知漫游消息类型")
                                },
                                imageBytes = item.bytes, imgW = item.w, imgH = item.h,
                                ts = item.ts, seq = item.seq,
                                fileId = item.fileId.ifBlank { item.msgId },
                                fileName = item.fileName,
                                fileSize = item.fileSize,
                                contentType = item.contentType,
                                sha256 = item.sha256,
                                status = if (fromMe) ChatMessage.Status.DELIVERED else ChatMessage.Status.RECEIVED,
                            )
                        append(
                            historyMessage,
                            incrUnread = false,
                            updateConversation = false,
                        )
                        // 漫游消息只有文件元数据，不携带图片字节。与实时 MediaCard 保持一致，
                        // 在本地没有可用缓存时通过鉴权 HTTP 文件服务下载后再交给 UI 展示。
                        if (item.isImage) {
                            val effective = _messages.value[peerId].orEmpty()
                                .firstOrNull { it.msgId == item.msgId } ?: historyMessage
                            val localAvailable = effective.imageBytes != null ||
                                effective.localPath?.let { java.io.File(it).isFile } == true
                            if (!localAvailable) downloadFile(effective)
                        }
                    }
                    // 更新分页游标：minSeq 为本批最小 seq，供下次上拉；hasMore 决定是否继续
                    if (e.minSeq > 0) {
                        val cur = loadedMinSeq[e.peerId]
                        if (cur == null || e.minSeq < cur) loadedMinSeq[e.peerId] = e.minSeq
                    }
                    roamHasMore[e.peerId] = e.hasMore
                    roamLoading.remove(e.peerId)
                }

                is ImClient.Event.FriendOffline -> {
                    _friends.value = _friends.value.map {
                        if (it.id == e.userId) it.copy(online = false) else it
                    }
                    _chatPeer.value?.takeIf { it.id == e.userId }?.let {
                        _chatPeer.value = it.copy(online = false)
                    }
                }

                ImClient.Event.KickedOffline -> {
                    expectDisconnect = true
                    Prefs.clearTokenSession()
                    Prefs.clearCredentialsIfNotRemember()
                    client.disconnect()
                    _toast.emit("你的账号已在其他设备登录")
                    resetToLogin()
                }

                ImClient.Event.Disconnected -> {
                    clearAiReply(cancelRemote = false)
                    // 连接断开意味着本次请求不可能再收到响应；允许重连后用同一 requestId 重试。
                    refreshMutex.withLock { refreshRequestInFlight = false }
                    if (!expectDisconnect && _screen.value != Screen.Login) {
                        if (Prefs.loadTokenSession() != null) {
                            startReconnect()
                        } else {
                            _toast.emit("登录状态已失效，请重新登录")
                            resetToLogin()
                        }
                    }
                    expectDisconnect = false
                }

                is ImClient.Event.MediaCard -> {
                    val inChat = (_screen.value == Screen.Chat || _screen.value == Screen.ChatSearch) &&
                        _chatPeer.value?.id == e.fromId
                    val media = ChatMessage(e.msgId, e.fromId, fromMe = false,
                        kind = if (e.isImage) MsgKind.IMAGE else MsgKind.FILE,
                        fileId = e.fileId, fileName = e.name, fileSize = e.size, ts = e.ts, seq = e.seq,
                        contentType = e.contentType, sha256 = e.sha256, imgW = e.width, imgH = e.height,
                        status = ChatMessage.Status.RECEIVED)
                    append(media, incrUnread = !inChat)
                    // 图片需要即时展示；普通文件仍由用户点击后下载。
                    if (e.isImage) downloadFile(media)
                }
            }
        }
    }

    private fun upsertFriend(f: Friend) {
        val list = _friends.value.toMutableList()
        val idx = list.indexOfFirst { it.id == f.id }
        if (idx >= 0) list[idx] = f else list.add(f)
        _friends.value = list.sortedWith(compareBy({ !it.online }, { it.id }))
    }

    /** msg_id 去重（内存 + Room UNIQUE 双保险），并按需落库。
     *  updateConversation=false：漫游历史（旧消息）只入消息列表，不刷新会话预览行，避免回退 lastTs */
    private fun append(msg: ChatMessage, incrUnread: Boolean, updateConversation: Boolean = true) {
        val conv = _messages.value[msg.peerId].orEmpty()
        val duplicateIndex = conv.indexOfFirst { it.msgId == msg.msgId }
        if (duplicateIndex >= 0) {
            // 离线补发可能撞上本地遗留的同 msgId 文件记录。补发数据是服务端权威状态，
            // 必须修正旧的 SENDING/空元数据，不能因 msgId 幂等而整条忽略。
            if ((msg.kind == MsgKind.FILE || msg.kind == MsgKind.IMAGE) && !msg.fromMe) {
                val old = conv[duplicateIndex]
                val repaired = old.copy(
                    fileId = msg.fileId.ifBlank { old.fileId },
                    fileName = msg.fileName.ifBlank { old.fileName },
                    fileSize = if (msg.fileSize > 0) msg.fileSize else old.fileSize,
                    contentType = msg.contentType.ifBlank { old.contentType },
                    sha256 = msg.sha256.ifBlank { old.sha256 },
                    imgW = if (msg.imgW > 0) msg.imgW else old.imgW,
                    imgH = if (msg.imgH > 0) msg.imgH else old.imgH,
                    ts = if (msg.ts > 0) msg.ts else old.ts,
                    seq = if (msg.seq > 0) msg.seq else old.seq,
                    status = ChatMessage.Status.RECEIVED,
                )
                val updated = conv.toMutableList().also { it[duplicateIndex] = repaired }
                _messages.value = _messages.value + (msg.peerId to sortBySeq(updated))
                viewModelScope.launch { store?.reconcileIncomingFile(client.myId, repaired) }
            }
            return
        }
        // 按会话 seq 插入并保持有序（补发/乱序到达时也能排到正确位置）
        _messages.value = _messages.value + (msg.peerId to sortBySeq(conv + msg))

        if (updateConversation) {
            // 会话行内存即时刷新
            val lastMsg = when (msg.kind) {
                MsgKind.IMAGE -> "[图片]"
                MsgKind.FILE -> "[文件] ${msg.fileName}"
                MsgKind.TEXT -> msg.text
                else -> " "
            }
            val old = _conversations.value[msg.peerId]
            val unread = (old?.unread ?: 0) + if (incrUnread && !msg.fromMe) 1 else 0
            _conversations.value = _conversations.value + (
                msg.peerId to com.jitong.im.data.db.ConversationEntity(
                    conversationId = 0L, ownerId = client.myId, peerId = msg.peerId,
                    lastMsg = lastMsg, lastTs = msg.ts, unread = unread,
                )
                )
        }

        viewModelScope.launch { store?.save(client.myId, msg, incrUnread, bumpConversation = updateConversation) }
    }

    /**
     * 合并 hydrate 的 DB 结果与内存中已到达的消息（按 msgId 去重、按会话 seq 排序）。
     * 用于登录时避免异步 hydrate 覆盖掉期间补发到内存的离线消息。
     * 排序主键为服务端分配的会话级 seq（严格顺序）；本端刚发出、尚未确认的消息 seq=0，
     * 视为最新排在末尾（用 ts 兜底其相对顺序）。
     */
    private fun mergeMessages(
        fromDb: Map<Int, List<ChatMessage>>,
        inMemory: Map<Int, List<ChatMessage>>,
    ): Map<Int, List<ChatMessage>> {
        if (inMemory.isEmpty()) return fromDb
        val result = fromDb.toMutableMap()
        for ((peerId, memList) in inMemory) {
            val base = result[peerId].orEmpty()
            val existingIds = base.mapTo(HashSet()) { it.msgId }
            val extra = memList.filter { it.msgId !in existingIds }
            if (extra.isEmpty()) continue
            result[peerId] = sortBySeq(base + extra)
        }
        return result
    }

    /** 会话内消息排序：seq>0 按 seq 升序；seq=0（本端未确认）排末尾，用 ts 兜底 */
    private fun sortBySeq(list: List<ChatMessage>): List<ChatMessage> =
        list.sortedWith(compareBy({ if (it.seq > 0) it.seq else Long.MAX_VALUE }, { it.ts }))

    /** 会话行合并：DB 为基底，内存里更新的会话行（lastTs 更新）优先保留。 */
    private fun mergeConversations(
        fromDb: Map<Int, ConversationEntity>,
        inMemory: Map<Int, ConversationEntity>,
    ): Map<Int, ConversationEntity> {
        if (inMemory.isEmpty()) return fromDb
        val result = fromDb.toMutableMap()
        for ((peerId, memConv) in inMemory) {
            val dbConv = result[peerId]
            if (dbConv == null || memConv.lastTs >= dbConv.lastTs) {
                result[peerId] = memConv
            }
        }
        return result
    }

    private fun updateStatus(peerId: Int, msgId: String, status: ChatMessage.Status, seq: Long = 0) {
        val conv = _messages.value[peerId] ?: return
        // 回执携带服务端分配的 seq：更新状态同时校正本端消息的 seq，并按 seq 重排
        val updated = conv.map {
            if (it.msgId == msgId) it.copy(status = status, seq = if (seq > 0) seq else it.seq) else it
        }
        _messages.value = _messages.value + (peerId to if (seq > 0) sortBySeq(updated) else updated)
        viewModelScope.launch {
            store?.updateStatus(client.myId, msgId, status)
            if (seq > 0) store?.updateSeq(client.myId, msgId, seq)
        }
    }

    private fun updateFileStatus(peerId: Int, msgId: String, status: ChatMessage.Status) {
        val conv = _messages.value[peerId] ?: return
        _messages.value = _messages.value + (peerId to conv.map {
            if (it.msgId == msgId) it.copy(status = status) else it })
        viewModelScope.launch { store?.updateStatus(client.myId, msgId, status) }
    }
    private fun updateFileLocalPath(peerId: Int, msgId: String, path: String) {
        val conv = _messages.value[peerId] ?: return
        _messages.value = _messages.value + (peerId to conv.map {
            if (it.msgId == msgId) it.copy(localPath = path) else it })
        viewModelScope.launch { store?.updateFileLocalPath(client.myId, msgId, path) }
    }
    private fun updateOutgoingFile(peerId: Int, msgId: String, path: String, size: Long) {
        val conv = _messages.value[peerId] ?: return
        _messages.value = _messages.value + (peerId to conv.map {
            if (it.msgId == msgId) it.copy(localPath = path, fileSize = size) else it })
        viewModelScope.launch { store?.updateOutgoingFile(client.myId, msgId, path, size) }
    }
    private fun updateFileProgress(peerId: Int, msgId: String, transferred: Int) {
        val conv = _messages.value[peerId] ?: return
        _messages.value = _messages.value + (peerId to conv.map {
            if (it.msgId == msgId) it.copy(transferred = transferred) else it })
    }

    private fun updateMediaMetadata(peerId: Int, msgId: String, fileId: String, contentType: String, sha256: String) {
        val conv = _messages.value[peerId] ?: return
        _messages.value = _messages.value + (peerId to conv.map {
            if (it.msgId == msgId) it.copy(fileId = fileId, contentType = contentType, sha256 = sha256) else it
        })
        viewModelScope.launch {
            store?.updateMediaMetadata(client.myId, msgId, fileId, contentType, sha256)
        }
    }

    /**
     * 断线自动重连（指数退避）：弱网/瞬断时自愈，不打断用户停留在会话页。
     * 每轮先建立 TLS 连接，再用短期 access token 登录；过期时先轮换 refresh token。
     * 达到最大尝试次数仍失败才回登录页。
     */
    private fun startReconnect() {
        if (reconnecting) return // 已在重连，避免并发
        if (Prefs.loadTokenSession() == null) {
            resetToLogin()
            return
        }
        reconnecting = true
        reconnectJob?.cancel()
        reconnectJob = viewModelScope.launch {
            var attempt = 0
            val maxAttempts = 10
            while (reconnecting && attempt < maxAttempts) {
                attempt++
                val backoffMs = minOf(1000L * (1L shl (attempt - 1)), 15_000L)
                _loginTip.value = "连接已断开，正在重连（第 $attempt 次）…"
                _toast.emit("连接已断开，正在重连…")
                kotlinx.coroutines.delay(backoffMs)
                if (!reconnecting) return@launch // 期间已恢复/被取消
                client.disconnect() // 清掉可能的半开连接
                val ok = runCatching { client.connect(ImClient.DEFAULT_HOST) }.getOrDefault(false)
                if (ok) {
                    val session = Prefs.loadTokenSession() ?: run {
                        resetToLogin()
                        return@launch
                    }
                    val nowSeconds = System.currentTimeMillis() / 1000L
                    if (session.accessExpiresAt > nowSeconds + 30L) {
                        client.loginWithToken(session, Prefs.deviceId)
                    } else if (session.refreshExpiresAt > nowSeconds) {
                        refreshMutex.withLock {
                            if (refreshRequestInFlight) return@withLock
                            val requestId = Prefs.pendingRefreshRequestId
                                ?: UUID.randomUUID().toString().also {
                                    Prefs.pendingRefreshRequestId = it
                                }
                            refreshRequestInFlight = true
                            try {
                                client.refreshToken(session, Prefs.deviceId, requestId)
                            } catch (error: Throwable) {
                                refreshRequestInFlight = false
                                throw error
                            }
                        }
                    } else {
                        Prefs.clearTokenSession()
                        _toast.emit("登录状态已过期，请重新登录")
                        resetToLogin()
                        return@launch
                    }
                    kotlinx.coroutines.delay(3000L)
                    if (!reconnecting) return@launch // 登录成功已清零
                    // 否则继续下一轮退避重试
                }
            }
            // 重试用尽仍未恢复：回登录页
            if (reconnecting) {
                reconnecting = false
                _toast.emit("重连失败，请重新登录")
                resetToLogin()
            }
        }
    }

    private fun resetToLogin() {
        clearAiReply(cancelRemote = false)
        reconnecting = false
        refreshRequestInFlight = false
        reconnectJob?.cancel()
        reconnectJob = null
        store?.close()
        store = null
        storeOwnerId = null
        loadedMinSeq.clear()
        roamHasMore.clear()
        roamLoading.clear()
        _friends.value = emptyList()
        _friendRequests.value = emptyList()
        pendingDeleteFriendId = null
        _messages.value = emptyMap()
        _conversations.value = emptyMap()
        _searchResults.value = emptyList()
        clearConversationSearch()
        _chatJumpTarget.value = null
        _chatPeer.value = null
        _myNick.value = ""
        _myFeeling.value = ""
        _screen.value = Screen.Login
    }

    override fun onCleared() {
        aiTimeoutJob?.cancel()
        client.disconnect()
        store?.close()
        store = null
        storeOwnerId = null
    }

    companion object {
        /** 漫游历史每页条数 */
        private const val PAGE_SIZE = 20
        private const val AI_UI_TIMEOUT_MS = 20_000L
    }
}
