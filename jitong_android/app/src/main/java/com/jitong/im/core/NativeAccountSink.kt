package com.jitong.im.core

/**
 * 账号事件接收器（P5-T06）。C++ AccountSession 通过 JNI 把 [im::account::AccountEvent]
 * 拍平成一次 [onAccountEvent] 回调投递上来。序号必须与 C++ 枚举严格一致。
 *
 * 回调可能来自任意 native 线程；实现方需自行切回主线程后再更新 UI。
 */
interface NativeAccountSink {
    fun onAccountEvent(
        type: Int,
        operationId: Long,
        accountState: Int,
        connectionState: Int,
        error: Int,
        userId: Int,
        message: String?,
    )
}

/** 与 C++ im::account::AccountState 序号一致。 */
enum class AccountState {
    LoggedOut,
    Authenticating,
    Authenticated,
    Refreshing,
    LoggedOutKicked;

    companion object {
        fun fromOrdinal(v: Int): AccountState =
            values().getOrElse(v) { LoggedOut }
    }
}

/** 与 C++ im::account::ConnectionState 序号一致。 */
enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting;

    companion object {
        fun fromOrdinal(v: Int): ConnectionState =
            values().getOrElse(v) { Disconnected }
    }
}

/** 与 C++ im::account::AuthError 序号一致。 */
enum class AuthError {
    None,
    NetworkUnreachable,
    InvalidCredentials,
    TokenExpired,
    TokenRevoked,
    DeviceProofFailed,
    KickedByOtherDevice,
    OperationInProgress,
    OperationSuperseded,
    ServerRejected,
    Internal;

    companion object {
        fun fromOrdinal(v: Int): AuthError = values().getOrElse(v) { None }
    }
}

/** 与 C++ im::account::AccountEventType 序号一致。 */
enum class AccountEventType {
    StateChanged,
    ConnectionChanged,
    LoginSucceeded,
    LoginFailed,
    RefreshSucceeded,
    RefreshFailed,
    Kicked,
    LoggedOut;

    companion object {
        fun fromOrdinal(v: Int): AccountEventType = values().getOrElse(v) { StateChanged }
    }
}
