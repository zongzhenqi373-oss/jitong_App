package com.jitong.im.core.platform

/**
 * 平台原子能力契约（P2-T03）。
 *
 * 与 C++ 侧 `app/src/main/cpp/PlatformServices.h` 一一对应。
 *
 * 硬性约束：
 *   - **不含任何业务语义**：不得出现 token / 消息 / 会话 / 秒传 / 缩略图概念；
 *   - **不做策略决定**：例如「生成几档缩略图」「是否重试」全由 C++ 内核决定；
 *   - **不抛业务异常**：失败通过返回值或回调表达，由内核决定如何处理。
 */

/** 打开的文件句柄：fd 优先，退化时用本地路径。 */
data class PlatformFileHandle(val fd: Int = -1, val localPath: String? = null) {
    val isValid: Boolean get() = fd >= 0 || !localPath.isNullOrEmpty()
}

/** 图片元信息。 */
data class PlatformImageInfo(
    val width: Int,
    val height: Int,
    val orientation: Int = 1,
    val colorSpace: String = "unknown",
    val mime: String = "",
)

data class PlatformDecodeOptions(
    val targetWidth: Int = 0,
    val targetHeight: Int = 0,
    val convertToSrgb: Boolean = true,
)

data class PlatformEncodeOptions(val quality: Int = 80)

/** Keystore：只做「取公钥」与「签名」。 */
interface KeystoreProvider {
    fun publicKey(alias: String): ByteArray?
    /** 必须异步：Keystore 签名可能触发用户认证，阻塞网络线程会 ANR。 */
    fun signAsync(alias: String, payload: ByteArray, callback: (ok: Boolean, signature: ByteArray?) -> Unit)
}

/** 文件：只做 open / readRange / rename。 */
interface FileProvider {
    fun openRead(uri: String): PlatformFileHandle?
    fun openWrite(path: String): PlatformFileHandle?
    fun readRange(handle: PlatformFileHandle, offset: Long, length: Long): ByteArray?
    fun atomicRename(from: String, to: String): Boolean
    fun close(handle: PlatformFileHandle)
}

/** 图片：只做 probe / decode / encode，参数由内核给定。 */
interface ImagePlatform {
    fun probe(handle: PlatformFileHandle): PlatformImageInfo?
    fun decode(handle: PlatformFileHandle, options: PlatformDecodeOptions): ByteArray?
    /**
     * 编码 JPEG —— **发送侧主出口**。
     *
     * 与 `util/ImageCodec` 保持同一策略：客户端对外产物一律 JPEG，
     * 规避本地 AVIF 编解码偏色（见 `util/ImagePipelineConfig` 的说明）。
     *
     * @return 编码后的 JPEG 字节；失败返回 null。像素格式 RGBA8，行主序。
     */
    fun encodeJpeg(rgba8: ByteArray, width: Int, height: Int, options: PlatformEncodeOptions): ByteArray?

    /**
     * 编码 AVIF。**仅用于「传输中间格式」的转码/归档场景**，不是发送侧出口。
     *
     * ⚠️ 已知缺陷：`avif-coder` 在部分 ABI/设备上解码出现 **G/B 通道错位**，
     * `Round11ClosureTest` 实测纯白回读为 `255,172,255`（G 掉到 172）。
     * 已验证与 `surfaceMode`（RGB/AUTO）、`chromaSubsampling`(YUV444) 无关，
     * 属库本身问题。P10 若要启用 AVIF 中间格式，必须先解决该缺陷。
     *
     * @return 编码后的 AVIF 字节；失败返回 null。像素格式 RGBA8，行主序。
     */
    fun encodeAvif(rgba8: ByteArray, width: Int, height: Int, options: PlatformEncodeOptions): ByteArray?
}

/** 安全 KV：opaque 字节存取，平台不理解内容含义。 */
interface SecureKv {
    fun load(key: String): ByteArray?
    fun save(key: String, blob: ByteArray): Boolean
    fun erase(key: String)
}

/** 网络状态：只上报通断，不做重连决策。 */
interface NetworkObserver {
    enum class State { Unknown, Available, Lost }

    fun current(): State
    fun setCallback(callback: (State) -> Unit)
    fun close()
}
