package com.jitong.im.util

import android.os.Build

/**
 * 图片流水线配置：**用工程规避解决 AVIF 偏色**，而不是做全链路色彩管理。
 *
 * ## 策略
 *
 * 把 AVIF 限定为「小尺寸、sRGB、服务端统一压缩」的**传输中间格式**：
 *
 * - **发送侧不产 AVIF**：本地统一编码 JPEG（质量 90）。偏色根因正是客户端本地
 *   编 AVIF —— `avif-coder` 的 lossless chroma 在部分 ABI 上解码会出现 G/B 通道
 *   错位（整图发绿/发洋红），且接收端对 AVIF 色域的解释在不同设备上不一致。
 *   JPEG 走 Android 系统编码器，行为稳定、跨端一致。
 * - **AVIF 只作传输中间格式**：若将来服务端支持实时压 AVIF（省流量），客户端
 *   下载到 AVIF 后**立即转码成 JPEG** 再落盘展示，并删除 AVIF 源文件。
 *   用户看到的永远是 JPEG。
 *
 * ## 能防什么 / 不能防什么
 *
 * - ✅ 防「量化损失 + 色彩空间往返」这类**可控**偏色：统一落到 8bit sRGB SDR，
 *   编码前只做一次必要的色彩空间定点转换，杜绝多次 RGB 往返累积误差。
 * - ❌ **不做**宽色域（Display P3）或 HDR/10bit 的保真：这类输入统一转换到 sRGB，
 *   属于有意的信息损失。要覆盖那类场景见 [ColorTransferParams] —— 需要把
 *   nclx/ICC 作为出参透传，并让输出 JPEG 携带正确的色彩配置文件（NDK 层
 *   libavif + libjpeg-turbo 实现，P10 Media SDK 阶段）。
 */
object ImagePipelineConfig {

    // ---------------- 发送侧编码质量 ----------------

    /** 原图 JPEG 质量：90 视觉与 AVIF q100 基本无差，但彻底规避 AVIF 解码色偏。 */
    const val JPEG_QUALITY_ORIGINAL = 90

    /** 大缩略图（长边 1280）质量。 */
    const val JPEG_QUALITY_LARGE_THUMB = 85

    /** 小缩略图（320×240）质量。 */
    const val JPEG_QUALITY_THUMB = 82

    /** 发送侧统一 MIME。 */
    const val MIME_JPEG = "image/jpeg"

    // ---------------- AVIF 作为传输中间格式（下载侧） ----------------

    /**
     * 是否允许把 AVIF 当作传输中间格式下载。
     *
     * 默认 `false`：当前服务端不具备实时压 AVIF 的能力，下载到的就是 JPEG，
     * 没有开启的必要。待服务端支持后由远程配置打开。
     */
    @Volatile
    var avifTransportEnabled: Boolean = false
        private set

    /** 被远程关闭 AVIF 的机型（命中即不走 AVIF，直接按常规格式下载）。 */
    private val blockedDevices = mutableSetOf<String>()

    /**
     * 当前设备是否允许走 AVIF 传输。
     * 机型黑名单优先于总开关：即使总开关打开，命中黑名单的机型也强制走常规下载。
     */
    @Synchronized
    fun isAvifTransportAllowed(): Boolean {
        if (!avifTransportEnabled) return false
        val model = Build.MODEL ?: return true
        val device = Build.DEVICE ?: ""
        val fingerprint = "$device/$model"
        return model !in blockedDevices && fingerprint !in blockedDevices
    }

    /** 远程配置下发入口；黑名单为空表示只由总开关决定。 */
    @Synchronized
    fun updateFromRemote(enabled: Boolean, blocked: Collection<String> = emptySet()) {
        avifTransportEnabled = enabled
        blockedDevices.clear()
        blocked.forEach { if (it.isNotBlank()) blockedDevices.add(it.trim()) }
    }

    /** 测试/本地调试用：直接设置状态。 */
    @Synchronized
    internal fun resetForTest(enabled: Boolean = false, blocked: Collection<String> = emptySet()) {
        updateFromRemote(enabled, blocked)
    }
}

/**
 * 转码色彩参数（nclx / ICC 出入参）。
 *
 * 当前 Kotlin 实现走「统一落到 sRGB」的工程规避路径，因此：
 * - **入参**：由 AVIF 解码结果填充，用于判断输入是否宽色域/HDR，并据此决定是否
 *   需要额外转换（转换本身会记录到 [convertedToSrgb]）；
 * - **出参**：[iccProfile] 允许调用方指定要写入输出 JPEG 的色彩配置文件。
 *   当输出已是 sRGB 时无需写入（JPEG 默认即 sRGB），故默认 `null`。
 *
 * 真正保留 Display P3 / HDR 需要 NDK 层直转（P10），届时这里就是透传通道。
 */
data class ColorTransferParams(
    /** NCLX colour_primaries：1=BT.709/sRGB，12=Display P3。 */
    val colorPrimaries: Int = 1,
    /** NCLX transfer_characteristics：13=sRGB，16=PQ(HDR10)，18=HLG。 */
    val transferCharacteristics: Int = 13,
    /** NCLX matrix_coefficients：6=BT.601，9=BT.2020。 */
    val matrixCoefficients: Int = 6,
    /** 是否 full-range（AVIF 常用 full range）。 */
    val fullRange: Boolean = true,
    /** 是否 10bit 及以上位深（HDR 常见）。 */
    val highBitDepth: Boolean = false,
    /** 需要写入输出 JPEG 的 ICC profile；null 表示不写。 */
    val iccProfile: ByteArray? = null,
    /** 实际是否发生了到 sRGB 的转换（出参）。 */
    val convertedToSrgb: Boolean = false,
) {
    /** 输入是否为宽色域（非 sRGB/BT.709）。 */
    val isWideGamut: Boolean get() = colorPrimaries != 1

    /** 输入是否为 HDR（PQ/HLG 传输特性）。 */
    val isHdr: Boolean get() = transferCharacteristics == 16 || transferCharacteristics == 18

    /** 是否需要「非平凡」的色彩处理（宽色域或 HDR）。 */
    val needsColorManagement: Boolean get() = isWideGamut || isHdr || highBitDepth
}
