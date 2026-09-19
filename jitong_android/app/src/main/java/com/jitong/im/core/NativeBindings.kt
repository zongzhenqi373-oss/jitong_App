package com.jitong.im.core

/**
 * Native 内核的底层绑定（P1-T03）。
 *
 * 这一层只做「加载 .so + 调用 native 函数 + 转成 Kotlin 结果」，
 * 不持有任何业务状态，也不做线程切换（由上层 [JitongSdk] 决定）。
 *
 * P2 会在此增加 nativeCreate/nativeDestroy/nativeSetEventSink 等生命周期方法。
 */
object NativeBindings {

    private const val LIBRARY_NAME = "jitong_kernel"

    /** 内核是否可用；加载失败时不抛异常，由调用方降级到 Legacy 实现。 */
    val isLoaded: Boolean by lazy {
        runCatching {
            System.loadLibrary(LIBRARY_NAME)
            true
        }.getOrDefault(false)
    }

    /** 加载过程中的异常，便于灰度期定位；成功时为 null。 */
    val loadError: Throwable? by lazy {
        runCatching {
            System.loadLibrary(LIBRARY_NAME)
            null
        }.getOrElse { it }
    }

    /**
     * 内核版本号。与 `.so` 编译期注入的 JITONG_KERNEL_VERSION 一致，
     * 用于灰度期确认「Java 代码与 Native 产物版本匹配」。
     */
    external fun nativeVersion(): String

    /**
     * Native 自检报告，换行分隔的 key=value：
     *
     * ```text
     * kernel=...
     * protobuf=...
     * openssl=...
     * abi=...
     * sizeof_long=...
     * frame_codec=ok|FAIL
     * endianness=ok|FAIL
     * result=ok|FAIL
     * ```
     *
     * 真机与模拟器（arm64-v8a / x86_64）除 `abi` 外必须完全一致。
     */
    external fun nativeSelfTest(): String

    /** 把自检报告解析成 Map，便于断言与日志。 */
    fun selfTestMap(): Map<String, String> =
        nativeSelfTest().lineSequence()
            .mapNotNull { line ->
                val idx = line.indexOf('=')
                if (idx > 0) line.substring(0, idx) to line.substring(idx + 1) else null
            }
            .toMap()

    // ---------------- P2：句柄生命周期 ----------------

    /**
     * 创建内核句柄。
     * @return 不透明句柄 id；0 表示创建失败（0 被保留为无效句柄）
     */
    fun nativeCreate(serverName: String?): Long {
        val key = com.jitong.im.net.AppIdentityPins.publicKeyBase64(1) ?: return 0L
        return nativeCreateConfigured(serverName, 1, key)
    }

    /** 真实 JNI 创建入口；信任根必须显式注入，Native 侧仍执行长度校验并 fail-close。 */
    private external fun nativeCreateConfigured(
        serverName: String?,
        identityKeyId: Int,
        identityPublicKeyBase64: String,
    ): Long

    /** 销毁句柄。幂等且并发安全：重复/并发调用只有一次真正生效。 */
    external fun nativeDestroy(handle: Long)

    /** 挂上事件接收器；句柄无效时返回 false。 */
    external fun nativeSetEventSink(handle: Long, sink: NativeEventSink): Boolean

    // ---------------- P7-G3：账号级 Runtime 生命周期 ----------------
    // Runtime 唯一拥有业务服务、数据库与 completion executor。
    // JNI 层不暴露 SQL、Socket、Token、DB key、sqlite3* 或 C++ 裸对象地址。

    /** 创建并挂载账号级 Runtime（同账号唯一）。 */
    external fun nativeCreateRuntime(handle: Long, ownerId: Int): Boolean
    /** 绑定 Runtime 的合并失效通知和可靠命令终态回调。 */
    external fun nativeSetRuntimeEventSink(handle: Long, sink: NativeRuntimeSink): Boolean

    /** 启动 Runtime；幂等。返回 runtimeGeneration（0 表示失败）。 */
    external fun nativeStartRuntime(handle: Long): Long

    /** 停止 Runtime（保留对象，可再次 start）。 */
    external fun nativeStopRuntime(handle: Long)

    /** 登出：停止并递增 generation，使在途旧 generation 事件失效。 */
    external fun nativeLogoutRuntime(handle: Long)

    /** 销毁并摘除 Runtime，但保留同一句柄上的账号会话，允许登出后换账号。 */
    external fun nativeDestroyRuntime(handle: Long)

    /** Runtime 状态名：Idle/Starting/Running/Stopping/Stopped/Destroyed。 */
    external fun nativeGetRuntimeState(handle: Long): String
    external fun nativeRuntimeSendText(
        handle: Long, conversationId: Long, peerId: Long, text: String,
        pinyin: String, initials: String,
    ): String
    external fun nativeBeginMediaOperation(handle:Long):Long
    external fun nativeCancelMediaOperation(handle:Long,operation:Long)
    external fun nativeEndMediaOperation(handle:Long,operation:Long)
    external fun nativeRuntimeUploadMedia(handle:Long,operation:Long,path:String,receiverId:Long,isImage:Boolean):String
    external fun nativeRuntimeDownloadMedia(handle:Long,operation:Long,fileId:String,destination:String):Boolean

    // ---------------- 分片上传（断点续传） ----------------
    /** 登记上传草稿（幂等，同 msgId+variant 保留进度）。返回 "ok" 或 "err|..."。 */
    external fun nativeMediaEnqueueUpload(handle:Long,msgId:String,conversationId:Long,peerId:Long,
        variant:Int,localPath:String,fileName:String,contentType:String,
        imageWidth:Int,imageHeight:Int):String
    /** 推进该消息上传直到 Sent/Failed（阻塞，含网络 IO；成功后唤醒 Outbox 发卡片）。 */
    external fun nativeMediaPumpUpload(handle:Long,msgId:String):String
    /** 启动/网络恢复扫描：恢复全部活跃草稿，返回推进到终态的消息数（阻塞）。 */
    external fun nativeMediaResumeUploads(handle:Long):Int
    /** 用户主动取消（终态，不自动恢复；与网络异常严格区分）。 */
    external fun nativeMediaCancelUpload(handle:Long,msgId:String):Boolean
    /** 上传进度查询："minState,doneChunks,totalChunks,originFileId"；无草稿返回空串。 */
    external fun nativeMediaUploadState(handle:Long,msgId:String):String
    external fun nativeRuntimeSendMedia(handle:Long,conversationId:Long,peerId:Long,type:Int,
        fileId:String,fileName:String,fileSize:Long,contentType:String,sha256:String,width:Int,height:Int,
        localPath:String,thumbnailFileId:String,thumbnailSize:Long,thumbnailSha256:String,
        thumbnailWidth:Int,thumbnailHeight:Int,largeThumbnailFileId:String,largeThumbnailSize:Long,
        largeThumbnailSha256:String,largeThumbnailWidth:Int,largeThumbnailHeight:Int):String
    external fun nativeRuntimeMarkRead(handle:Long,conversationId:Long,readSeq:Long):String
    external fun nativeRuntimeFlushOutbox(handle: Long, nowSeconds: Long, limit: Int): Int
    external fun nativeRuntimeConsumeInvalidation(handle: Long, domain: Int): Long
    external fun nativeRuntimeConsumeOperation(handle: Long, operationId: String): String
    external fun nativeRuntimeRequestRoamConversations(handle: Long): Boolean
    external fun nativeRuntimeRequestRoamMessages(
        handle: Long, peerId: Long, beforeSeq: Long, limit: Int,
    ): Boolean
    external fun nativeRuntimeLoadFriends(handle: Long): ByteArray?
    external fun nativeRuntimeLoadFriendRequests(handle: Long): ByteArray?
    external fun nativeRuntimeRequestFriendRequests(handle: Long): Boolean
    external fun nativeRuntimeSendAddFriend(handle: Long, nick: String): Boolean
    external fun nativeRuntimeAnswerFriend(
        handle: Long, requesterId: Long, requesterNick: String, agree: Boolean,
    ): Boolean
    external fun nativeRuntimeDeleteFriend(handle: Long, friendId: Long): Boolean

    /** 当前存活句柄数，用于压力测试断言无泄漏。 */
    external fun nativeHandleCount(): Int

    /**
     * 生命周期压力测试（创建销毁 N 次 + 幂等 + 野句柄 + 回调销毁并发）。
     * 返回 key=value 报告，末行 `result=ok|FAIL`。
     */
    fun nativeLifecycleStressTest(iterations: Int, sink: NativeEventSink?): String {
        val key = com.jitong.im.net.AppIdentityPins.publicKeyBase64(1)
            ?: return "result=FAIL\nerror=identity_key_missing"
        return nativeLifecycleStressTestConfigured(iterations, sink, 1, key)
    }
    private external fun nativeLifecycleStressTestConfigured(iterations: Int, sink: NativeEventSink?,
                                                              keyId: Int, publicKeyBase64: String): String

    /** 测试专用：由 C++ 构造包含中文和非 BMP emoji 的标准 UTF-8，再走真实 JNI 回调。 */
    external fun nativeEmitUtf8Test(handle: Long): Boolean

    // ---------------- P4：应用层安全通道进程内握手自检 ----------------

    /**
     * 在 native 侧跑一遍完整的应用层安全通道握手（进程内环回，不依赖服务端/TLS）：
     * ClientHello → ServerHello(Ed25519 验签) → ClientFinished → ServerFinished
     * → 加密业务帧双向 AES-256-GCM 往返。全程使用与真实链路相同的加密原语。
     *
     * 用于在 Android arm64（真机/模拟器）上验证 X25519/Ed25519/HKDF/AES-GCM 与
     * 四步握手在目标架构上的正确性。返回换行分隔的分步诊断，末行 `result=ok|FAIL`。
     */
    external fun nativeSecureChannelHandshakeTest(): String

    /** 把握手自检报告解析成 Map。 */
    fun handshakeTestMap(): Map<String, String> =
        nativeSecureChannelHandshakeTest().lineSequence()
            .mapNotNull { line ->
                val idx = line.indexOf('=')
                if (idx > 0) line.substring(0, idx) to line.substring(idx + 1) else null
            }
            .toMap()

    /**
     * 设备证明（P-256）自检：生成 P-256 密钥、导出 X.509 SPKI DER 公钥、对规范
     * message 做 SHA256withECDSA 签名并本地验签往返。返回 `result=ok|FAIL`。
     */
    external fun nativeDeviceProofSelfTest(): String

    /**
     * P4 验收专用真实网络入口。它直接复用生产 ClientCore，依次完成：
     * TCP → TLS 1.3/CA/hostname/SPKI → 四步应用握手 → AES-GCM Heartbeat 往返。
     * 仅供 androidTest 调用，不接入 UI 或业务登录流程。
     */
    external fun nativeSocketHeartbeatTest(
        host: String,
        port: Int,
        serverName: String,
        caFile: String,
        identityPublicKeyBase64: String,
        spkiPinBase64: String,
        plaintextMarker: String,
    ): String

    // ---------------- P5-T06：认证会话（厚内核编排） ----------------

    /**
     * 在已创建的句柄上建立认证会话：绑定服务端地址与账号事件回调。
     * 之后所有连接/握手/设备签名/Token/刷新/重连/被踢均由 C++ AccountSession 编排。
     * @return 是否建立成功（句柄无效/事件桥无效返回 false）
     */
    external fun nativeAccountSetup(
        handle: Long,
        serverIp: String,
        port: Int,
        accountSink: NativeAccountSink,
        platform: com.jitong.im.core.platform.NativeAuthPlatform,
    ): Boolean

    /** 密码登录（首次/换账号）。返回 operationId（0 表示被拒/无会话）。 */
    external fun nativeAccountLoginWithPassword(handle: Long, account: String, password: String): Long

    /**
     * 一次性注册（先于认证，不进入 AccountSession 会话生命周期）。
     * 仅在账号空闲时可发起；结果经 [NativeEventSink.onRegisterResult] 异步上抛，
     * 本地连接不可用上报 result=0（非协议结果码）。
     */
    external fun nativeAccountRegister(handle: Long, nick: String, tel: String, pass: String): Boolean

    /** 冷启动自动登录：有未过期凭据则 Token 登录，否则返回 0（需密码登录）。 */
    external fun nativeAccountStartWithSavedToken(handle: Long, account: String): Long

    /** 登出（allDevices=true 登出全部设备）。 */
    external fun nativeAccountLogout(handle: Long, allDevices: Boolean)

    /** 取消进行中的认证（连点/放弃）。 */
    external fun nativeAccountCancel(handle: Long)

    /** 查询当前 AccountState 序号；-1 表示无会话。 */
    external fun nativeAccountGetState(handle: Long): Int

    // ---------------- P6-T01：加密 fixture 双向验证（仅测试使用） ----------------
    // 这两个接口只接受"路径 + 32 字节 key"，只能操作固定结构的 native_probe 表，
    // 不接受任意 SQL，也不返回数据库句柄或 key；仅用于证明 Room 与 Native 可互开
    // 同一份加密库（S1-3）。返回形如 "ok|cipher=4.6.1 community|..." 或 "err|<code>|<msg>"。

    /** 创建（覆盖）一个加密 fixture 库，内含 native_probe 表与固定 marker。 */
    external fun nativeCipherCreateFixtureForTest(path: String, key: ByteArray): String

    /** 只读打开既有加密库，回报 cipher 版本、表数量与 native_probe 的 marker。 */
    external fun nativeCipherVerifyFixtureForTest(path: String, key: ByteArray): String

    // ---------------- P6-T05/S6：正式数据库生命周期与影子迁移 ----------------
    // key 经平台密钥桥（NativeDbKeyPlatform）从 DbKeyManager 解出，不再由调用方直接传。
    // 数据库由 NativeSdkHandle 持有；不暴露任意 SQL / 裸句柄 / key。

    /**
     * 打开（或创建）账号影子库，数据库挂到句柄上。
     * @param bridge [com.jitong.im.core.platform.NativeDbKeyPlatform]（当前登录态 passHash 的密钥桥）
     * @return 是否打开成功；无 passHash（Token-only 冷启动）时返回 false 且保持锁定
     */
    external fun nativeOpenAccountDatabase(
        handle: Long,
        ownerId: Int,
        filesDir: String,
        bridge: com.jitong.im.core.platform.NativeDbKeyPlatform,
    ): Boolean

    /** 关闭当前账号数据库（普通 logout 只关库，不删数据）。 */
    external fun nativeCloseAccountDatabase(handle: Long)

    /** 开始影子导入：Disabled → ShadowImport；已 Verified 返回 false 拒绝重复迁移。 */
    external fun nativeBeginMigration(handle: Long): Boolean

    /** 回滚到 ShadowImport：清完成标记与 checkpoint（不删生产数据）。 */
    external fun nativeResetMigration(handle: Long): Boolean

    /**
     * 提交一个迁移批次（消息 + FTS + 会话摘要 + checkpoint 在**一个事务**内提交）。
     * @param batch 由 [com.jitong.im.data.MigrationExporter] 编码的长度前缀二进制
     * @return "ok|imported=N" 或 "err|<code>|<msg>"
     */
    external fun nativeSubmitMigrationBatch(handle: Long, batch: ByteArray,
                                            checkpoint: String): String

    /** 提交一个会话元数据批次（unread/lastMsg/lastTs，权威覆盖，独立 checkpoint）。 */
    external fun nativeSubmitConversationBatch(handle: Long, batch: ByteArray,
                                               checkpoint: String): String

    /** Room v10 delta：数据修改与 legacy_delta checkpoint 在 Native Writer 同一事务提交。 */
    external fun nativeSubmitLegacyDeltaBatch(
        handle: Long, epoch: Long, expectedAfter: Long, batch: ByteArray,
    ): String
    /** 冷启动恢复用：返回 ok|checkpoint=N。 */
    external fun nativeGetLegacyDeltaCheckpoint(handle: Long, epoch: Long): String
    /** snapshot 前首次建立 delta baseline；返回 ok|checkpoint=N。 */
    external fun nativeSeedLegacyDeltaCheckpoint(
        handle: Long, epoch: Long, baseline: Long, updatedAt: Long,
    ): String

    /** 查询 Native DB 最新 cutover journal。 */
    external fun nativeGetCutoverJournal(handle: Long): String

    /**
     * cutover DIRTY 镜像写入委托，由 Application 在 onCreate 注入。
     * Native transaction hook 在首次业务事务中原子推进 DIRTY 后回调；
     * 写入失败/进程被杀导致镜像滞后时，冷启动双证据校验 fail-close 为 Repair，绝不回退 Room。
     */
    @Volatile
    var cutoverDirtyHandler: ((CutoverMirrorStore.Evidence) -> Unit)? = null

    /** JNI 回调入口（可能发生在任意 native 线程，禁止在此做阻塞/UI 操作）。 */
    @JvmStatic
    fun onCutoverDirtyFromNative(
        ownerId: Long, epoch: Long, state: Int, highWater: Long, schemaVersion: Int,
        keyId: String, summary: String, updatedAt: Long,
    ) {
        val evidence = CutoverMirrorStore.Evidence(
            ownerId, epoch, state, highWater, schemaVersion, keyId, summary, updatedAt,
        )
        runCatching { cutoverDirtyHandler?.invoke(evidence) }
    }

    /** 严格单向推进 cutover；expectedState=-1 仅允许创建 LEGACY_ACTIVE。 */
    external fun nativeAdvanceCutoverJournal(
        handle: Long, epoch: Long, expectedState: Int, targetState: Int,
        highWater: Long, schemaVersion: Int, keyId: String, summary: String, updatedAt: Long,
    ): String


    /** 全量对账并写完成标记；对账不一致返回 err|VerifyFailed|... 且不写标记。 */
    external fun nativeFinishMigration(handle: Long, expectedMessages: Long,
                                       expectedConversations: Long, expectedMinSeq: Long,
                                       expectedMaxSeq: Long, expectedFts: Long): String

    /** 查询迁移状态：completed / checkpoint / 当前摘要。 */
    external fun nativeGetMigrationState(handle: Long): String

    /** 数据库自检：cipher 版本 / schema 版本 / 迁移完成状态 / 摘要。 */
    external fun nativeRunDatabaseSelfTest(handle: Long): String

    /** 搜索 Native 消息库；返回版本化长度前缀二进制，失败返回 null。 */
    external fun nativeSearchMessages(
        handle: Long,
        conversationId: Long,
        keyword: String,
        limit: Int,
    ): ByteArray?

    /** 按稳定 keyset 游标读取会话历史；返回 JTHP v1 二进制。 */
    external fun nativeLoadHistory(
        handle: Long,
        conversationId: Long,
        limit: Int,
        hasCursor: Boolean,
        cursorTime: Long,
        cursorSeq: Long,
        cursorOrder: Long,
        cursorMsgId: String,
    ): ByteArray?
    /** 读取账号会话快照；返回 JTCL v1 二进制。 */
    external fun nativeLoadConversations(handle: Long): ByteArray?
    external fun nativeBeginDownloadTask(handle:Long,taskId:String,msgId:String,fileId:String,
                                         localPath:String,generation:Long):Boolean
    external fun nativeFinishDownloadTask(handle:Long,taskId:String,generation:Long,
                                          state:Int,transferred:Long):Boolean
    external fun nativeListRecoverableDownloads(handle:Long):ByteArray?
}
