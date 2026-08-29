package com.jitong.im.net

import com.jitong.im.data.Prefs
import com.jitong.im.data.crypto.TokenVault.TokenSession
import im.proto.Im
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.DataInputStream
import java.net.InetSocketAddress
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSocket

/**
 * 即通 Android 纯 Kotlin 协议栈客户端（设计决策 D2：本期不引入 djinni/JNI）。
 * 职责：TCP 连接管理、二段式帧收发、心跳保活、协议分发。
 * 事件通过 [events] 流出，UI 层在 ViewModel 中收集（自动切主线程）。
 */
class ImClient {

    sealed interface Event {
        data class RegisterResult(val result: Int) : Event
        data class LoginResult(val result: Int, val userId: Int, val tokenSession: TokenSession?,) : Event
        data class TokenLoginResult(val result: Int, val userId: Int, val accessExpiresAt: Long) : Event
        data class TokenRefreshResult(val result: Int, val tokenSession: TokenSession?) : Event
        data class LogoutResult(val result: Int) : Event

        data class AiReplyResult(
            val requestId: String,
            val status: Im.AiReplyStatus,
            val suggestions: List<String>,
            val errorMessage: String,
            val partial: Boolean,
            val chunkIndex: Int,
            val done: Boolean,
        ) : Event

        /** 本人资料（userId == myId）或好友资料/上下线状态刷新 */
        data class UserOrFriendInfo(
            val userId: Int,
            val nick: String,
            val feeling: String,
            val online: Boolean,
        ) : Event

        /** 收到文本消息（在线转发或离线补发），ts 为毫秒（服务端权威时间换算），seq 为会话级序列号 */
        data class ChatReceived(val fromId: Int, val text: String, val msgId: String, val ts: Long, val seq: Long) : Event

        /** 发送回执：peerId=接收方好友，result=CHAT_RESULT_SUCC(已送达)/FAIL(已转存离线)，seq=服务端分配的会话序列号 */
        data class ChatSendResult(val peerId: Int, val result: Int, val msgId: String, val seq: Long) : Event

        /** 收到添加好友请求：fromId=发起者id，fromNick=发起者的nick，同意或者拒绝应该让服务器知道向谁发送添加好友回执*/
        data class AddFriendRequestReceived(val fromId: Int, val fromNick: String) : Event

        /** 好友添加回执：result=添加结果，peerId=被添加人的id，peerNick=被添加人的nick*/
        data class AddFriendResult(val result: Int, val peerId: Int, val peerNick: String) : Event

        data class FriendRequestItem(
            val requesterId: Int,
            val targetId: Int,
            val requesterNick: String,
            val targetNick: String,
            val createdAt: Long,
        )
        data class FriendRequestsLoaded(val requests: List<FriendRequestItem>) : Event

        data class DeleteFriendResult(val result: Int, val friendId: Int, val operatorId: Int, val operatorNick: String) : Event


        /** 漫游消息条目（会话列表末条 / 历史分页共用）。图片 bytes 为空表示预览占位（不落消息表）。 */
        data class RoamItem(
            val fromId: Int,
            val toId: Int,
            val isText: Boolean,
            val isImage: Boolean,
            val isFile: Boolean,
            val text: String,
            val bytes: ByteArray?,
            val w: Int,
            val h: Int,
            val msgId: String,
            val ts: Long,
            val seq: Long,
            val fileId: String,
            val fileName: String,
            val fileSize: Long,
            val contentType: String,
            val sha256: String,
        )

        /** 漫游会话列表结果：每会话最后一条（仅用于会话预览行，不落消息表） */
        data class RoamConversations(val convs: List<RoamItem>) : Event

        /** 漫游历史分页结果：hasMore/minSeq 供上拉翻页 */
        data class RoamMessages(
            val peerId: Int,
            val msgs: List<RoamItem>,
            val hasMore: Boolean,
            val minSeq: Long,
        ) : Event

        data class MediaCard(
            val fromId: Int, val fileId: String, val name: String, val size: Long,
            val msgId: String, val ts: Long, val seq: Long, val isImage: Boolean,
            val contentType: String, val sha256: String, val width: Int, val height: Int,
        ) : Event

        data class FriendOffline(val userId: Int) : Event

        /** 账号在别处登录，被服务端踢下线 */
        data object KickedOffline : Event

        /** 连接断开（含主动 logout 之外的意外断开） */
        data object Disconnected : Event
    }

    private val _events = MutableSharedFlow<Event>(extraBufferCapacity = 64)
    val events: SharedFlow<Event> = _events

    @Volatile
    var myId: Int = 0
        private set

    @Volatile
    var connected: Boolean = false
        private set

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var socket: SSLSocket? = null
    private var secureChannel: SecureChannel? = null
    private val writeLock = Mutex()
    private var heartbeatJob: kotlinx.coroutines.Job? = null

    /** 建立 TCP 连接并启动读循环 + 心跳；成功返回 true */
    suspend fun connect(host: String, port: Int = Protocol.TCP_PORT): Boolean =
        withContext(Dispatchers.IO) {
            if (connected) return@withContext true
            try {
                val factory = SSLContext
                    .getDefault()
                    .socketFactory

                val sslSocket = factory
                    .createSocket() as SSLSocket

                sslSocket.connect(
                    InetSocketAddress(host,port),
                    5000,
                )

                sslSocket.enabledProtocols = arrayOf("TLSv1.3")

                val parameters = sslSocket.sslParameters
                parameters.endpointIdentificationAlgorithm = "HTTPS"
                sslSocket.sslParameters = parameters

                sslSocket.tcpNoDelay = true
                sslSocket.keepAlive = true
                sslSocket.soTimeout = 0

                sslSocket.startHandshake()

                val tlsSession = sslSocket.session
                // 必须在任何登录凭证或业务帧发出之前完成固定校验。
                // 即使设备额外信任了恶意 CA，公钥不匹配也会在这里断开。
                TlsPinning.verify(tlsSession)

                /**打印日志，握手成功*/
                android.util.Log.i(
                    "IM_TLS",
                    "握手及 SPKI 固定校验成功 " +
                            "protocol=${tlsSession.protocol} " +
                            "cipher=${tlsSession.cipherSuite} " +
                            "peer=${tlsSession.peerHost}",
                )
                /**打印服务端证书*/
                val certificate =
                    tlsSession.peerCertificates.firstOrNull()
                            as? java.security.cert.X509Certificate
                android.util.Log.i(
                    "IM_TLS",
                    "服务端证书 " +
                            "subject=${certificate?.subjectX500Principal?.name} " +
                            "issuer=${certificate?.issuerX500Principal?.name} " +
                            "expires=${certificate?.notAfter}",
                )

                val establishedChannel = SecureChannel.establish(sslSocket)
                android.util.Log.i(
                    "IM_APP_SEC",
                    "应用层安全握手成功 version=${Protocol.APP_SECURITY_VERSION} " +
                        "sessionBytes=${establishedChannel.sessionId.size}",
                )

                socket = sslSocket
                secureChannel = establishedChannel
                connected = true

                startHeartbeat()
                scope.launch {
                    readLoop(sslSocket)
                }

                true
            } catch (e: Exception) {
                android.util.Log.e(
                    "IM_APP_SEC",
                    "TLS/SPKI/应用层握手失败",
                    e,
                )
                closeQuietly()
                false
            }
        }

    suspend fun register(nick: String, tel: String, pass: String) {
        val rq = Im.RegisterRq.newBuilder()
            .setNick(nick)
            .setTel(tel)
            .setPass(sha256Hex(pass)) // 与 C++ 端一致：哈希上链路
            .build()
        send(Protocol.REGISTER_RQ, rq.toByteArray())
    }

    suspend fun login(
        tel: String,
        pass: String,
        deviceId: String,
    ) {
        val rq = Im.LoginRq.newBuilder()
            .setTel(tel)
            .setPass(sha256Hex(pass))
            .setDeviceId(deviceId)
            .setDeviceName("${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL}")
            .setClientVersion("android-0.5.0")
            .build()

        send(Protocol.LOGIN_RQ, rq.toByteArray())
    }

    suspend fun loginWithToken(session: TokenSession, deviceId: String) {
        val rq = Im.TokenLoginRq.newBuilder()
            .setAccessToken(session.accessToken)
            .setDeviceId(deviceId)
            .setRequestId(java.util.UUID.randomUUID().toString())
            .build()
        send(Protocol.TOKEN_LOGIN_RQ, rq.toByteArray())
    }

    suspend fun refreshToken(session: TokenSession, deviceId: String, requestId: String) {
        val rq = Im.RefreshTokenRq.newBuilder()
            .setRefreshToken(session.refreshToken)
            .setDeviceId(deviceId)
            .setRequestId(requestId)
            .build()
        send(Protocol.TOKEN_REFRESH_RQ, rq.toByteArray())
    }

    suspend fun revokeSession(session: TokenSession, deviceId: String, allDevices: Boolean = false) {
        val rq = Im.LogoutRq.newBuilder()
            .setRefreshToken(session.refreshToken)
            .setDeviceId(deviceId)
            .setLogoutAllDevices(allDevices)
            .build()
        send(Protocol.LOGOUT_RQ, rq.toByteArray())
    }

    suspend fun sendChat(friId: Int, text: String, msgId: String) {
        val rq = Im.ChatInfoRq.newBuilder()
            .setMyid(myId)
            .setFriid(friId)
            .setMsg(text)
            .setType(Im.MsgType.TEXT)
            .setMsgId(msgId) // 客户端生成 UUID，漫游/去重幂等
            .build()
        send(Protocol.CHAT_INFO_RQ, rq.toByteArray())
    }

    /**发送添加好友申请*/
    suspend fun sendAddFriendRq(myNick: String, friNick: String){
        val rq = Im.AddFriendRq.newBuilder()
            .setMyid(myId)
            .setMynick(myNick)
            .setFrinick(friNick)
            .build()
        send(Protocol.ADD_FRIEND_RQ,rq.toByteArray())
    }

    /**发送添加好友回执，回应一条添加好友申请*/
    suspend fun sendAddFriendRs(destId: Int, destNick: String, myNick: String, result: Int){
        val rs = Im.AddFriendRs.newBuilder()
            .setMyid(myId)
            .setMynick(myNick)
            .setDestid(destId)
            .setDestnick(destNick)
            .setResult(result)
            .build()
        send(Protocol.ADD_FRIEND_RS, rs.toByteArray())
    }

    suspend fun requestFriendRequests() {
        send(
            Protocol.FRIEND_REQUEST_LIST_RQ,
            Im.FriendRequestListRq.getDefaultInstance().toByteArray(),
        )
    }

    /**发送删除好友请求*/
    suspend fun deleteFriend(friendId: Int){
        val rq = Im.DeleteFriendRq.newBuilder()
            .setFriendId(friendId)
            .build()

        send(Protocol.DELETE_FRIEND_RQ,rq.toByteArray())
    }

    /** HTTPS 上传完成后，经 IM 长连接发送媒体卡片元数据。 */
    suspend fun sendMediaCard(
        friId: Int, fileId: String, fileName: String, size: Long,
        contentType: String, sha256: String, isImage: Boolean,
        w: Int, h: Int, msgId: String,
    ) {
        val rq = Im.ChatInfoRq.newBuilder()
            .setMyid(myId)
            .setFriid(friId)
            .setType(if (isImage) Im.MsgType.IMAGE else Im.MsgType.FILE)
            .setImageWidth(w)
            .setImageHeight(h)
            .setMsgId(msgId)
            .setFileId(fileId)
            .setFileName(fileName)
            .setFileSize(size)
            .setContentType(contentType)
            .setSha256(sha256)
            .build()
        send(Protocol.CHAT_INFO_RQ, rq.toByteArray())
    }

    /** 漫游：登录后拉每会话最后一条（会话列表预览）。myid 占位，服务端以登录态为准 */
    suspend fun roamConversations() {
        val rq = Im.RoamConvRq.newBuilder().setMyid(myId).build()
        send(Protocol.ROAM_CONV_RQ, rq.toByteArray())
    }

    /** 漫游：拉某会话比 beforeSeq 更早的 limit 条（首次传 Long.MAX 拉最新，上拉传已加载最小 seq） */
    suspend fun roamMessages(peerId: Int, beforeSeq: Long, limit: Int) {
        val rq = Im.RoamMsgRq.newBuilder()
            .setMyid(myId)
            .setPeerId(peerId)
            .setBeforeSeq(beforeSeq)
            .setLimit(limit)
            .build()
        send(Protocol.ROAM_MSG_RQ, rq.toByteArray())
    }

    /** 请求服务端基于当前单聊最近消息生成候选回复；聊天正文由服务端从数据库读取。 */
    suspend fun requestAiReply(
        peerId: Int,
        requestId: String,
        tone: String = "自然、简洁",
        maxSuggestions: Int = 3,
    ) {
        val request = Im.AiReplyRq.newBuilder()
            .setRequestId(requestId)
            .setPeerId(peerId)
            .setTone(tone)
            .setMaxSuggestions(maxSuggestions.coerceIn(1, 3))
            .build()
        send(Protocol.AI_REPLY_RQ, request.toByteArray())
    }

    /** 尽力取消：服务端尚未调用模型时跳过；调用进行中时丢弃响应。 */
    suspend fun cancelAiReply(requestId: String) {
        val request = Im.AiCancelRq.newBuilder().setRequestId(requestId).build()
        send(Protocol.AI_CANCEL_RQ, request.toByteArray())
    }

    /** 主动下线：通知服务端（其会广播好友）后关闭连接 */
    suspend fun logout() {
        if (connected && myId > 0) {
            val pkt = Im.FriendOffline.newBuilder().setOfflineid(myId).build()
            try {
                send(Protocol.FRIEND_OFFLINE, pkt.toByteArray())
            } catch (_: Exception) {
            }
        }
        disconnect()
    }

    fun disconnect() {
        heartbeatJob?.cancel()
        closeQuietly()
    }

    // ---------------- 内部实现 ----------------

    /** pb ChatInfoRq → RoamItem。ts 秒转毫秒，图片字节为空表示预览占位 */
    private fun Im.ChatInfoRq.toRoamItem(): Event.RoamItem {
        val isText = type == Im.MsgType.TEXT
        val isImage = type == Im.MsgType.IMAGE
        val isFile = type == Im.MsgType.FILE
        val tsMs = if (ts > 0) ts * 1000L else System.currentTimeMillis()
        return Event.RoamItem(
            fromId = myid, toId = friid, isText = isText, isImage = isImage, isFile = isFile,
            text = msg, bytes = null, w = imageWidth, h = imageHeight,
            msgId = msgId, ts = tsMs, seq = seq,
            fileId = fileId, fileName = fileName, fileSize = fileSize,
            contentType = contentType, sha256 = sha256,
        )
    }

    private suspend fun readLoop(s: SSLSocket) {
        try {
            val input = DataInputStream(s.getInputStream())
            while (currentCoroutineContext().isActive) {
                val outerFrame = Frame.readFrom(input) ?: break
                val channel = secureChannel ?: throw java.io.IOException("安全通道未建立")
                val frame = channel.decrypt(outerFrame)
                android.util.Log.d(
                    "IM_APP_SEC",
                    "已认证解密业务帧 innerType=${frame.type} payloadBytes=${frame.payload.size}",
                )
                dispatch(frame)
            }
        } catch (e: Exception) {
            android.util.Log.e("IM_APP_SEC", "安全通道读取失败，连接已关闭", e)
        } finally {
            closeQuietly()
            _events.emit(Event.Disconnected)
        }
    }

    private suspend fun dispatch(f: Frame) {
        when (f.type) {
            Protocol.REGISTER_RS ->
                _events.emit(Event.RegisterResult(Im.RegisterRs.parseFrom(f.payload).result))

            Protocol.LOGIN_RS -> {
                val rs = Im.LoginRs.parseFrom(f.payload)
                if (rs.result == Protocol.LOGIN_SUCCESS) myId = rs.userid
                val tokenSession = if (rs.result == Protocol.LOGIN_SUCCESS) {
                    TokenSession(
                        rs.userid, rs.sessionId, rs.accessToken, rs.refreshToken,
                        rs.accessTokenExpireAt, rs.refreshTokenExpireAt,
                    )
                } else null
                _events.emit(Event.LoginResult(rs.result, rs.userid, tokenSession))
            }

            Protocol.TOKEN_LOGIN_RS -> {
                val rs = Im.TokenLoginRs.parseFrom(f.payload)
                if (rs.result == Protocol.LOGIN_SUCCESS) myId = rs.userid
                _events.emit(Event.TokenLoginResult(rs.result, rs.userid, rs.accessTokenExpireAt))
            }

            Protocol.TOKEN_REFRESH_RS -> {
                val rs = Im.RefreshTokenRs.parseFrom(f.payload)
                val old = Prefs.loadTokenSession()
                val tokenSession = if (rs.result == Protocol.REFRESH_TOKEN_SUCCESS && old != null) {
                    TokenSession(
                        old.userId, rs.sessionId, rs.accessToken, rs.refreshToken,
                        rs.accessTokenExpireAt, rs.refreshTokenExpireAt,
                    )
                } else null
                _events.emit(Event.TokenRefreshResult(rs.result, tokenSession))
            }

            Protocol.LOGOUT_RS ->
                _events.emit(Event.LogoutResult(Im.LogoutRs.parseFrom(f.payload).result))

            Protocol.AI_REPLY_RS -> {
                val response = Im.AiReplyRs.parseFrom(f.payload)
                _events.emit(
                    Event.AiReplyResult(
                        response.requestId,
                        response.status,
                        response.suggestionsList,
                        response.errorMessage,
                        response.partial,
                        response.chunkIndex,
                        response.done,
                    )
                )
            }

            Protocol.FRIEND_INFO -> {
                val info = Im.FriendInfo.parseFrom(f.payload)
                _events.emit(
                    Event.UserOrFriendInfo(
                        info.userid, info.nick, info.feeling,
                        info.status == Protocol.STATUS_ONLINE,
                    )
                )
            }

            Protocol.CHAT_INFO_RQ -> {
                val rq = Im.ChatInfoRq.parseFrom(f.payload)
                // 服务端 ts 为秒级权威时间；换算成毫秒统一口径，0 则兜底本地时间（兼容老服务端）
                val tsMs = if (rq.ts > 0) rq.ts * 1000L else System.currentTimeMillis()
                android.util.Log.d("IMDBG", "dispatch CHAT_INFO_RQ from=${rq.myid} type=${rq.type} msgId=${rq.msgId} ts=${rq.ts} seq=${rq.seq} msg=${rq.msg}")
                when (rq.type) {
                    Im.MsgType.TEXT ->
                        _events.emit(Event.ChatReceived(rq.myid, rq.msg, rq.msgId, tsMs, rq.seq))
                    Im.MsgType.IMAGE, Im.MsgType.FILE ->
                        _events.emit(Event.MediaCard(
                            rq.myid, rq.fileId, rq.fileName, rq.fileSize, rq.msgId, tsMs, rq.seq,
                            rq.type == Im.MsgType.IMAGE, rq.contentType, rq.sha256,
                            rq.imageWidth, rq.imageHeight,
                        ))
                    else -> Unit
                }
            }

            Protocol.CHAT_INFO_RS -> {
                val rs = Im.ChatInfoRs.parseFrom(f.payload)
                // 回执里 myid=接收方好友，friid=自己
                _events.emit(Event.ChatSendResult(rs.myid, rs.result, rs.msgId, rs.seq))
            }

            /**添加好友请求与回复*/
            Protocol.ADD_FRIEND_RQ -> {
                val rq = Im.AddFriendRq.parseFrom(f.payload)
                //请求中需要传请求方id和请求方nick
                _events.emit(Event.AddFriendRequestReceived(rq.myid, rq.mynick))
            }

            Protocol.ADD_FRIEND_RS -> {
                val rs = Im.AddFriendRs.parseFrom(f.payload)
                //回执中应该传的是被添加方id和被添加方nick，以及结果，回执中myid就是被添加方的id，mynick就是被添加方的nick
                _events.emit(Event.AddFriendResult(rs.result,rs.myid,rs.mynick))
            }

            Protocol.FRIEND_REQUEST_LIST_RS -> {
                val rs = Im.FriendRequestListRs.parseFrom(f.payload)
                _events.emit(Event.FriendRequestsLoaded(rs.requestsList.map {
                    Event.FriendRequestItem(
                        requesterId = it.requesterId,
                        targetId = it.targetId,
                        requesterNick = it.requesterNick,
                        targetNick = it.targetNick,
                        createdAt = it.createdAt,
                    )
                }))
            }

            /**删除好友*/
            Protocol.DELETE_FRIEND_RS -> {
                val rs = Im.DeleteFriendRs.parseFrom(f.payload)

                _events.emit(Event.DeleteFriendResult(rs.result, rs.friendId, rs.operatorId, rs.operatorNick))
            }

            Protocol.ROAM_CONV_RS -> {
                val rs = Im.RoamConvRs.parseFrom(f.payload)
                _events.emit(Event.RoamConversations(rs.convsList.map { it.toRoamItem() }))
            }

            Protocol.ROAM_MSG_RS -> {
                val rs = Im.RoamMsgRs.parseFrom(f.payload)
                _events.emit(
                    Event.RoamMessages(
                        rs.peerId, rs.msgsList.map { it.toRoamItem() }, rs.hasMore, rs.minSeq,
                    )
                )
            }

            Protocol.FRIEND_OFFLINE ->
                _events.emit(Event.FriendOffline(Im.FriendOffline.parseFrom(f.payload).offlineid))

            Protocol.KICKED_OFFLINE -> _events.emit(Event.KickedOffline)

            Protocol.HEARTBEAT_RS -> Unit // 心跳应答，无需处理
        }
    }

    private fun startHeartbeat() {
        heartbeatJob?.cancel()
        heartbeatJob = scope.launch {
            while (isActive) {
                delay(HEARTBEAT_INTERVAL_MS)
                try {
                    send(Protocol.HEARTBEAT_RQ, ByteArray(0))
                } catch (_: Exception) {
                    break // 发送失败说明连接已坏，交由读循环收尾
                }
            }
        }
    }

    // 所有发送集中走 IO 线程：UI 层在主线程直接调用也安全（NetworkOnMainThreadException 防护）
    private suspend fun send(type: Int, payload: ByteArray) = withContext(Dispatchers.IO) {
        val s = socket ?: throw java.io.IOException("未连接")
        writeLock.withLock {
            val channel = secureChannel ?: throw java.io.IOException("安全通道未建立")
            val encryptedFrame = channel.encrypt(type, payload)
            val bytes = Frame.encode(encryptedFrame.type, encryptedFrame.payload)
            s.getOutputStream().write(bytes)
            s.getOutputStream().flush()
            android.util.Log.d(
                "IM_APP_SEC",
                "业务帧已加密 innerType=$type outerType=${encryptedFrame.type} " +
                    "plaintextBytes=${payload.size} encryptedBytes=${encryptedFrame.payload.size}",
            )
        }
    }

    private fun closeQuietly() {
        connected = false
        heartbeatJob?.cancel()
        try {
            socket?.close()
        } catch (_: Exception) {
        }
        socket = null
        secureChannel?.destroy()
        secureChannel = null
    }

    companion object {
        /** 30s 一次心跳，与服务端 90s 超时空窗对齐 client_core */
        const val HEARTBEAT_INTERVAL_MS = 30_000L

        /** 默认服务端地址：模拟器经 10.0.2.2 访问宿主 Mac */
        const val DEFAULT_HOST = "10.0.2.2"
    }
}
