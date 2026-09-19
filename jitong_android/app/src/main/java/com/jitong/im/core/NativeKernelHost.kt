package com.jitong.im.core

import android.content.Context
import com.jitong.im.core.platform.NativeDbKeyPlatform
import com.jitong.im.util.sha256Hex
import java.io.File
import java.util.concurrent.ConcurrentHashMap
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

/**
 * Android 平台的 Native 内核宿主。
 *
 * 它只负责 Android 特有的生命周期组装：账号事件到达后解锁账号库、创建同一账号 Runtime，
 * 再向页面暴露薄控制器。Token 判断、刷新、重连、消息 ACK/重试、漫游与好友状态机仍在 C++。
 * 页面不能自行分别创建这些组件，否则容易出现“登录成功但 Runtime 未启动”的半初始化状态。
 */
class NativeKernelHost(
    context: Context,
    private val scope: CoroutineScope,
    private val sdk: JitongSdk = JitongSdk(),
) {
    enum class Phase { Stopped, AccountReady, Authenticating, Activating, Ready, Failed, Kicked }

    data class State(
        val phase: Phase = Phase.Stopped,
        val ownerId: Int = 0,
        val error: String? = null,
    )

    data class Controllers(
        val messages: NativeMessageController,
        val friends: NativeFriendController,
        val media: NativeMediaController,
    )

    private val appContext = context.applicationContext
    private val databaseRoot = File(appContext.filesDir, "native_kernel")
    private val pendingPasswordHashes = ConcurrentHashMap<Long, String>()
    private val _state = MutableStateFlow(State())
    val state: StateFlow<State> = _state
    private val _controllers = MutableStateFlow<Controllers?>(null)
    val controllers: StateFlow<Controllers?> = _controllers

    @Volatile private var configured = false
    @Volatile private var shuttingDown = false
    private var eventJob: Job
    private var coreEventJob: Job

    /** 注册结果（1=成功，2=昵称占用，3=手机号已注册，0=本地连接失败）；UI 只读。 */
    val registerResults = kotlinx.coroutines.flow.MutableSharedFlow<Int>(extraBufferCapacity = 4)

    init {
        // UNDISPATCHED 保证构造返回前已订阅 SharedFlow，避免极快的登录回执落在订阅窗口之前。
        eventJob = scope.launch(Dispatchers.IO, start = CoroutineStart.UNDISPATCHED) {
            sdk.accountEvents.collect(::handleAccountEvent)
        }
        coreEventJob = scope.launch(Dispatchers.IO, start = CoroutineStart.UNDISPATCHED) {
            sdk.events.collect { event ->
                if (event is CoreEvent.RegisterResult) registerResults.tryEmit(event.result)
            }
        }
    }

    @Synchronized
    fun setup(serverName: String, serverIp: String, port: Int): Boolean {
        if (configured) return true
        if (KernelBackendSelector.effective != KernelBackend.CPP_NATIVE ||
            !sdk.start(serverName) || !sdk.setupAccount(serverIp, port)) {
            sdk.stop()
            _state.value = State(Phase.Failed, error = "Native 账号内核初始化失败")
            return false
        }
        configured = true
        shuttingDown = false
        _state.value = State(Phase.AccountReady)
        return true
    }

    /** 密码仅交给 Native 认证；Kotlin 只短暂保存其摘要，供登录成功后解锁本账号数据库。 */
    fun loginWithPassword(account: String, password: String): Long {
        if (!configured) return 0
        val hash = sha256Hex(password)
        val operationId = sdk.loginWithPassword(account, password)
        if (operationId != 0L) {
            pendingPasswordHashes[operationId] = hash
            _state.value = State(Phase.Authenticating)
        }
        return operationId
    }

    fun startWithSavedToken(account: String): Long {
        if (!configured) return 0
        return sdk.startWithSavedToken(account).also {
            if (it != 0L) _state.value = State(Phase.Authenticating)
        }
    }

    /** 一次性注册（先于认证）；结果经 [registerResults] 异步到达。 */
    fun register(nick: String, tel: String, pass: String): Boolean {
        if (!configured) return false
        return sdk.register(nick, tel, pass)
    }

    fun logout(allDevices: Boolean = false) {
        if (configured) sdk.logout(allDevices)
    }

    fun cancelAuthentication() {
        if (configured) sdk.cancelAuthentication()
    }

    @Synchronized
    fun shutdown() {
        shuttingDown = true
        configured = false
        pendingPasswordHashes.clear()
        _controllers.value?.messages?.close()
        _controllers.value = null
        sdk.stop()
        _state.value = State(Phase.Stopped)
        eventJob.cancel()
        coreEventJob.cancel()
    }

    private fun handleAccountEvent(event: JitongSdk.AccountEvent) {
        when (event.type) {
            AccountEventType.LoginSucceeded -> activate(event)
            AccountEventType.LoginFailed -> {
                pendingPasswordHashes.remove(event.operationId)
                _state.value = State(Phase.AccountReady, error = event.message ?: event.error.name)
            }
            AccountEventType.Kicked -> teardownBusiness(Phase.Kicked, event.message ?: "账号已被踢下线")
            AccountEventType.LoggedOut -> teardownBusiness(Phase.AccountReady, null)
            else -> Unit
        }
    }

    @Synchronized
    private fun activate(event: JitongSdk.AccountEvent) {
        if (shuttingDown || !configured || event.userId <= 0) return
        _state.value = State(Phase.Activating, event.userId)
        val bridge = NativeDbKeyPlatform(appContext)
        bridge.setPassHash(pendingPasswordHashes.remove(event.operationId))
        pendingPasswordHashes.clear()
        databaseRoot.mkdirs()

        val opened = sdk.openAccountDatabase(event.userId, databaseRoot.absolutePath, bridge)
        // passHash 不跨越开库窗口；后续冷启动走已落地的设备包装密钥。
        bridge.setPassHash(null)
        val created = opened && sdk.createRuntime(event.userId)
        val generation = if (created) sdk.startRuntime() else 0L
        if (!opened || !created || generation == 0L) {
            // createRuntime 后的对象不能换 owner；失败时销毁整个句柄，禁止半初始化实例被复用。
            sdk.stop()
            configured = false
            _controllers.value = null
            _state.value = State(Phase.Failed, event.userId, "账号数据库或 Native Runtime 启动失败")
            return
        }

        val messageController = NativeMessageController(sdk, scope)
        val friendController = NativeFriendController(sdk, scope)
        _controllers.value = Controllers(messageController, friendController,
            NativeMediaController(appContext,sdk,scope))
        _state.value = State(Phase.Ready, event.userId)
        // 登录后的同步意图只发给同一个 Native ClientCore；离线时由后续重连/补洞继续收敛。
        sdk.requestRoamConversations()
        // 断点续传：登录就绪后恢复未终态上传草稿（以服务端会话为准，只补缺失分片）
        scope.launch(Dispatchers.IO) { sdk.resumeUploads() }
        friendController.refresh()
    }

    @Synchronized
    private fun teardownBusiness(next: Phase, error: String?) {
        pendingPasswordHashes.clear()
        _controllers.value?.messages?.close()
        _controllers.value = null
        sdk.destroyRuntime()
        sdk.closeAccountDatabase()
        _state.value = State(next, error = error)
    }
}
