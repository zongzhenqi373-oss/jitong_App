package com.jitong.im.ui

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch

/** 认证流程阶段，供 UI 展示与按钮禁用判断。 */
enum class AuthPhase { Idle, Connecting, Authenticating, Authenticated, Failed }

/** 一次认证请求的意图；所有登录入口统一收口成这三类。 */
sealed interface AuthRequest {
    data class Password(val tel: String, val pass: String) : AuthRequest
    data class Register(val nick: String, val tel: String, val pass: String) : AuthRequest
    /** 冷启动/重连的 token 登录；需轮换 refresh 时先置 true。 */
    data class Token(val needRefresh: Boolean) : AuthRequest
}

/**
 * 认证入口统一收口：同一时刻只允许一个认证流程在飞（单飞）。
 *
 * 只有这一层是纯客户端防护，服务端仍有三层兜底：
 * 1) 同一 Session 内业务请求走固定 business strand 串行，bindAuth 后重复登录被拒绝；
 * 2) 不同连接登录同一账号时 Presence.replace 原子替换并踢旧连接；
 * 3) refresh 由 requestId 缓存 + 数据库轮换事务保证重试幂等。
 *
 * 这里解决的是“客户端入口没有统一收口”导致的重复连接、本地凭证被并发清写、
 * 以及 UI 状态竞争。流程结果通过 [onCompleted] 由调用方（ViewModel）收口。
 */
class AuthCoordinator(private val scope: CoroutineScope) {
    private var job: Job? = null

    private val _phase = kotlinx.coroutines.flow.MutableStateFlow(AuthPhase.Idle)
    val phase: kotlinx.coroutines.flow.StateFlow<AuthPhase> = _phase

    val isRunning: Boolean get() = _phase.value == AuthPhase.Connecting || _phase.value == AuthPhase.Authenticating

    /**
     * 提交一次认证请求。若已有流程在飞则直接返回，不新建连接。
     * 认证步骤（连接、发协议、副作用）由 [body] 提供，结果通过 [onCompleted] 收口。
     */
    @Synchronized
    fun submit(body: suspend () -> Unit) {
        // 在启动协程前同步切换阶段，关闭“已提交但协程尚未调度”的重复提交窗口；
        // 同时以阶段覆盖“请求已经发出、发送 Job 已结束、响应尚未返回”的等待窗口。
        if (isRunning || job?.isActive == true) return
        _phase.value = AuthPhase.Connecting
        job = scope.launch {
            runCatching {
                body()
            }.onFailure {
                onCompleted(false)
            }
        }
    }

    /** 进入“已连接、等待服务端认证响应”阶段。 */
    fun markAuthenticating() {
        _phase.value = AuthPhase.Authenticating
    }

    /** 认证流程结束（成功或失败）；成功后若被挤下线会回到 Idle。 */
    @Synchronized
    fun onCompleted(success: Boolean) {
        _phase.value = if (success) AuthPhase.Authenticated else AuthPhase.Failed
        job = null
    }

    /** 注册只创建账号，不代表当前 Session 已认证；成功后回到空闲态等待用户登录。 */
    @Synchronized
    fun onRegistrationCompleted(success: Boolean) {
        _phase.value = if (success) AuthPhase.Idle else AuthPhase.Failed
        job = null
    }

    /** 主动取消（用户返回、退出登录、重置到登录页）。 */
    @Synchronized
    fun cancel() {
        job?.cancel()
        job = null
        _phase.value = AuthPhase.Idle
    }
}
