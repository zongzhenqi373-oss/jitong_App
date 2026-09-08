package com.jitong.im.core

import androidx.annotation.Keep
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.receiveAsFlow

/** 内核上抛的事件（C++ IClientEvents 的 Kotlin 映射）。 */
sealed interface CoreEvent {
    data class RegisterResult(val result: Int) : CoreEvent
    data class LoginResult(val result: Int, val userId: Int) : CoreEvent
    data class SelfInfo(val userId: Int, val nick: String, val feeling: String, val iconId: Int) : CoreEvent
    data class FriendInfo(val userId: Int, val nick: String, val status: Int, val iconId: Int) : CoreEvent
    data class ChatMessage(val fromId: Int, val text: String) : CoreEvent
    data class ChatSendResult(val peerId: Int, val result: Int) : CoreEvent
    data class AddFriendRequest(val fromId: Int, val fromNick: String) : CoreEvent
    data class AddFriendResult(val result: Int, val destNick: String) : CoreEvent
    data class FriendOffline(val userId: Int) : CoreEvent
    data class KickedOffline(val reason: Int) : CoreEvent
    data object ConnectionClosed : CoreEvent
    data class RoamMessages(val peerId: Int, val count: Int, val hasMore: Boolean, val minSeq: Long) : CoreEvent
    data class FileCard(
        val fromId: Int,
        val fileId: String,
        val name: String,
        val size: Long,
        val msgId: String,
        val contentType: String,
        val sha256: String,
        val isImage: Boolean,
        val width: Int,
        val height: Int,
    ) : CoreEvent
    data class FileProgress(val fileId: String, val received: Int, val total: Int, val status: Int) : CoreEvent
}

/**
 * C++ JniObserver 的回调目标。
 *
 * 约束（执行规划 P2-T02）：**回调只入队，不做业务**。
 * 这些方法会在 asio 网络线程/心跳线程上被直接调用，因此：
 *   - 只做可靠 Channel 入队（非阻塞、线程安全），绝不执行业务逻辑或切线程；
 *   - 不能抛异常回 C++（C++ 侧会 ExceptionCheck，但异常意味着状态丢失）；
 *   - 方法签名被 C++ 用 GetMethodID 缓存，改名/改签名必须同步 jni_observer.cpp；
 *   - `@Keep` 防止 Release 构建被 R8 混淆后 C++ 找不到方法。
 */
class NativeEventSink {

    // 第一轮先使用无丢弃的可靠队列。P5/P7 接管业务前再按语义拆成：
    // StateFlow（连接/账户）、可靠 Channel（消息/ACK/Token）和可覆盖进度流。
    private val eventChannel = Channel<CoreEvent>(Channel.UNLIMITED)

    val events: Flow<CoreEvent> = eventChannel.receiveAsFlow()

    private fun emit(event: CoreEvent) {
        eventChannel.trySend(event)
    }

    @Keep
    fun onRegisterResult(result: Int) {
        emit(CoreEvent.RegisterResult(result))
    }

    @Keep
    fun onLoginResult(result: Int, userId: Int) {
        emit(CoreEvent.LoginResult(result, userId))
    }

    @Keep
    fun onSelfInfo(userId: Int, nick: String?, feeling: String?, iconId: Int) {
        emit(CoreEvent.SelfInfo(userId, nick.orEmpty(), feeling.orEmpty(), iconId))
    }

    @Keep
    fun onFriendInfo(userId: Int, nick: String?, status: Int, iconId: Int) {
        emit(CoreEvent.FriendInfo(userId, nick.orEmpty(), status, iconId))
    }

    @Keep
    fun onChatMessage(fromId: Int, msgUtf8: String?) {
        emit(CoreEvent.ChatMessage(fromId, msgUtf8.orEmpty()))
    }

    @Keep
    fun onChatSendResult(friId: Int, result: Int) {
        emit(CoreEvent.ChatSendResult(friId, result))
    }

    @Keep
    fun onAddFriendRequest(fromId: Int, fromNick: String?) {
        emit(CoreEvent.AddFriendRequest(fromId, fromNick.orEmpty()))
    }

    @Keep
    fun onAddFriendResult(result: Int, destNick: String?) {
        emit(CoreEvent.AddFriendResult(result, destNick.orEmpty()))
    }

    @Keep
    fun onFriendOffline(userId: Int) {
        emit(CoreEvent.FriendOffline(userId))
    }

    @Keep
    fun onKickedOffline(reason: Int) {
        emit(CoreEvent.KickedOffline(reason))
    }

    @Keep
    fun onConnectionClosed() {
        emit(CoreEvent.ConnectionClosed)
    }

    @Keep
    fun onRoamMessages(peerId: Int, count: Int, hasMore: Boolean, minSeq: Long) {
        emit(CoreEvent.RoamMessages(peerId, count, hasMore, minSeq))
    }

    @Keep
    fun onFileCard(
        fromId: Int,
        fileId: String?,
        name: String?,
        size: Long,
        msgId: String?,
        contentType: String?,
        sha256: String?,
        isImage: Boolean,
        imageWidth: Int,
        imageHeight: Int,
    ) {
        emit(
            CoreEvent.FileCard(
                fromId = fromId,
                fileId = fileId.orEmpty(),
                name = name.orEmpty(),
                size = size,
                msgId = msgId.orEmpty(),
                contentType = contentType.orEmpty(),
                sha256 = sha256.orEmpty(),
                isImage = isImage,
                width = imageWidth,
                height = imageHeight,
            )
        )
    }

    @Keep
    fun onFileProgress(fileId: String?, received: Int, total: Int, status: Int) {
        emit(CoreEvent.FileProgress(fileId.orEmpty(), received, total, status))
    }
}
