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
    external fun nativeCreate(serverName: String?): Long

    /** 销毁句柄。幂等且并发安全：重复/并发调用只有一次真正生效。 */
    external fun nativeDestroy(handle: Long)

    /** 挂上事件接收器；句柄无效时返回 false。 */
    external fun nativeSetEventSink(handle: Long, sink: NativeEventSink): Boolean

    /** 当前存活句柄数，用于压力测试断言无泄漏。 */
    external fun nativeHandleCount(): Int

    /**
     * 生命周期压力测试（创建销毁 N 次 + 幂等 + 野句柄 + 回调销毁并发）。
     * 返回 key=value 报告，末行 `result=ok|FAIL`。
     */
    external fun nativeLifecycleStressTest(iterations: Int, sink: NativeEventSink?): String

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
}
