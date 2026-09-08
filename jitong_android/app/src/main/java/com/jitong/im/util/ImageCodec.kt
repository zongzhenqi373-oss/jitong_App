package com.jitong.im.util

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.ColorSpace
import android.net.Uri
import android.os.Build
import android.util.Log
import com.radzivon.bartoshyk.avif.coder.HeifCoder
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import java.io.File

/**
 * 相册图片处理：原图与两档缩略图统一编码为 **JPEG**（原图质量 90）。
 * 缩略图采用 centerCrop：普通图片裁为 4:3；超过 3:1 的长图/宽图先截取中心区域，
 * 避免“一条线”式缩略图。原始宽高仍随消息发送，点开后展示完整图片。
 *
 * ## 为什么发送侧不再编码 AVIF
 *
 * 本地 AVIF 编码是偏色的根因：
 * - `avif-coder` 的 lossless chroma 在部分 ABI 上解码出现 G/B 通道错位（整图绿/洋红）；
 * - 接收端对 AVIF 色域的解释在不同设备/系统版本上不一致，同一张图在不同手机饱和度不同。
 *
 * 因此采用 QQNT 的工程规避思路：**AVIF 只作为「服务端统一压缩」的传输中间格式**，
 * 客户端对外的产物一律是 JPEG。若下载到 AVIF（历史消息或将来服务端开启压缩），
 * 通过 [transcodeAvifToJpeg] 转码成 JPEG 后再落盘，并删除 AVIF 源文件。
 *
 * 解码侧仍保留 AVIF 能力，用于兼容历史已发出的 AVIF 消息。
 */
object ImageCodec {

    /** 发送侧统一输出格式。 */
    const val OUTPUT_MIME = ImagePipelineConfig.MIME_JPEG
    const val OUTPUT_EXT = ".jpg"

    data class Compressed(
        val bytes: ByteArray,
        val w: Int,
        val h: Int,
        val thumbnailBytes: ByteArray,
        val thumbnailW: Int,
        val thumbnailH: Int,
        val largeThumbnailBytes: ByteArray,
        val largeThumbnailW: Int,
        val largeThumbnailH: Int,
    )

    suspend fun loadAndCompress(context: Context, uri: Uri): Compressed? =
        withContext(Dispatchers.IO) {
            try {
                val resolver = context.contentResolver

                // 先只解码边界拿原始尺寸
                val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                resolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it, null, bounds) }
                if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return@withContext null

                // 按目标尺寸采样解码，避免原图 OOM
                val longEdge = maxOf(bounds.outWidth, bounds.outHeight)
                var sample = 1
                while (longEdge / (sample * 2) >= MAX_ORIGINAL_EDGE) sample *= 2
                val opts = BitmapFactory.Options().apply {
                    inSampleSize = sample
                    // Photo Picker 可能返回 Display P3 / Ultra HDR 原图。明确要求软件
                    // ARGB_8888 + sRGB，避免放大解码时走设备相关的 HDR/GPU 色彩路径。
                    inPreferredConfig = Bitmap.Config.ARGB_8888
                    inPreferredColorSpace = ColorSpace.get(ColorSpace.Named.SRGB)
                }
                val decoded = resolver.openInputStream(uri)?.use {
                    BitmapFactory.decodeStream(it, null, opts)
                } ?: return@withContext null
                val sampled = stripHdrGainmap(decoded)

                // 原图只限制极端分辨率
                val scale = minOf(1f, MAX_ORIGINAL_EDGE.toFloat() / maxOf(sampled.width, sampled.height))
                val final = if (scale < 1f) {
                    Bitmap.createScaledBitmap(
                        sampled,
                        (sampled.width * scale).toInt().coerceAtLeast(1),
                        (sampled.height * scale).toInt().coerceAtLeast(1),
                        true,
                    )
                } else sampled

                // 只做「必要的」那一次色彩定点：输入已是 sRGB 且不透明时直接复用，
                // 不再额外拷贝。往返次数越多，量化误差累积越明显，这正是偏色的来源之一。
                val encodable = ensureEncodable(final)

                // 三档统一 JPEG：原图 90 / 大缩略图 85 / 小缩略图 82
                val originalBytes = encodeJpeg(encodable, ImagePipelineConfig.JPEG_QUALITY_ORIGINAL)
                    ?: return@withContext null
                val thumb = centerCropThumbnail(encodable)
                val thumbnailBytes = encodeJpeg(thumb, ImagePipelineConfig.JPEG_QUALITY_THUMB)
                    ?: return@withContext null
                val largeThumb = scaleLongEdge(encodable, LARGE_THUMB_EDGE)
                val largeThumbnailBytes = encodeJpeg(largeThumb, ImagePipelineConfig.JPEG_QUALITY_LARGE_THUMB)
                    ?: return@withContext null
                Compressed(
                    originalBytes, encodable.width, encodable.height,
                    thumbnailBytes, thumb.width, thumb.height,
                    largeThumbnailBytes, largeThumb.width, largeThumb.height,
                )
            } catch (e: Exception) {
                null
            }
        }

    /**
     * 展示端优先使用 Android 系统解码器。API 33+ 原生支持标准 AVIF，并能正确处理
     * 色彩信息；第三方 libavif 仅作为旧文件/特殊编码的兼容兜底。
     */
    fun decodeForDisplay(bytes: ByteArray): Bitmap? {
        if (bytes.isEmpty()) return null
        val options = BitmapFactory.Options().apply {
            inPreferredConfig = Bitmap.Config.ARGB_8888
            inPreferredColorSpace = ColorSpace.get(ColorSpace.Named.SRGB)
        }
        val systemDecoded = runCatching {
            BitmapFactory.decodeByteArray(bytes, 0, bytes.size, options)
        }.getOrNull()
        if (systemDecoded != null) {
            Log.d(TAG, "AVIF display decoder=system size=${systemDecoded.width}x${systemDecoded.height}")
            return stripHdrGainmap(systemDecoded)
        }

        return runCatching {
            val coder = HeifCoder()
            if (coder.isAvif(bytes)) {
                val decoded = stripHdrGainmap(coder.decode(bytes))
                Log.w(TAG, "AVIF display decoder=fallback size=${decoded.width}x${decoded.height}")
                decoded
            } else null
        }.getOrNull()
    }

    /**
     * Android 14 的 Ultra HDR JPEG 会在普通 SDR Bitmap 上附带 gain map。即时通讯图片
     * 统一发送 SDR 版本；否则部分模拟器/设备在缩放或 AVIF 转码时会错误应用增益图，
     * 出现绿色、洋红色通道异常。
     */
    private fun stripHdrGainmap(bitmap: Bitmap): Bitmap {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE && bitmap.hasGainmap()) {
            bitmap.gainmap = null
        }
        return bitmap
    }

    /**
     * 保证位图可直接喂给 JPEG 编码器：8bit sRGB、不透明、ARGB_8888。
     *
     * **只在确实需要时才转换** —— 已满足条件的输入直接返回原对象，不做任何拷贝。
     * 每多一次 RGB 往返就多一轮 8bit 量化，往返叠加正是「看起来像加了滤镜」的来源。
     */
    private fun ensureEncodable(source: Bitmap): Bitmap {
        val srgb = ColorSpace.get(ColorSpace.Named.SRGB)
        val alreadySrgb = source.colorSpace == null || source.colorSpace == srgb
        // JPEG 不支持 alpha：带 alpha 通道的输入必须合成到不透明底，
        // 否则部分解码器对 RGB 通道的解释不同，会出现通道错位。
        if (alreadySrgb && !source.hasAlpha() && source.config == Bitmap.Config.ARGB_8888) {
            return source
        }
        return toSrgb(source)
    }

    private fun toSrgb(source: Bitmap): Bitmap {
        val srgb = ColorSpace.get(ColorSpace.Named.SRGB)
        // 统一落为不透明 SDR RGB。部分相册截图虽然所有像素均不透明，
        // Bitmap 元数据仍标记 hasAlpha=true，需要显式合成到白底。
        val converted = Bitmap.createBitmap(
            source.width,
            source.height,
            Bitmap.Config.ARGB_8888,
            false,
            srgb,
        )
        Canvas(converted).apply {
            drawColor(android.graphics.Color.WHITE)
            drawBitmap(source, 0f, 0f, null)
        }
        return converted
    }

    /**
     * 编码 JPEG —— 发送侧唯一的编码出口。
     *
     * 关于「优先走无 RGB 往返的 YUV 直转」：Android 的 `Bitmap.compress(JPEG)` 由
     * libjpeg-turbo 完成，但输入是 RGBA，**Kotlin/Bitmap 层无法做到 YUV 直转**；
     * 真正的直连需要 NDK 层 libavif → libjpeg-turbo（P10 Media SDK 阶段实现，
     * 接口已预留 `preferYuvDirect`，见 AndroidImagePlatform）。
     *
     * 当前做法是**把 RGB 往返压缩到唯一一次**：解码时即定点到 sRGB（见
     * [ensureEncodable]），编码前不再做任何额外转换或拷贝。
     */
    fun encodeJpeg(bitmap: Bitmap, quality: Int): ByteArray? =
        runCatching {
            ByteArrayOutputStream().use { out ->
                if (bitmap.compress(Bitmap.CompressFormat.JPEG, quality, out)) out.toByteArray()
                else null
            }
        }.getOrNull()

    /**
     * 通过 ISOBMFF 魔数判断是否为 AVIF/HEIF：偏移 4..8 为 `ftyp`，
     * 主品牌（8..12）以 `avif` / `avis` / `mif1` 开头。
     */
    fun isAvifBytes(bytes: ByteArray): Boolean {
        if (bytes.size < 12) return false
        if (String(bytes, 4, 4, Charsets.ISO_8859_1) != "ftyp") return false
        val brand = String(bytes, 8, 4, Charsets.ISO_8859_1)
        return brand.startsWith("avif") || brand.startsWith("avis") || brand.startsWith("mif1")
    }

    /**
     * 把 AVIF **传输中间格式**转码为 JPEG 并落盘（对应服务端架平实时压 AVIF 的场景）。
     *
     * 成功后 [dest] 是合法 JPEG，调用方负责删除 AVIF 源文件。
     *
     * **全链路降级**：任何一步失败都返回 `false`，由调用方决定降级策略
     * （按普通图片处理 / 报错触发重下），绝不把半成品文件留在最终路径上
     * —— 写入走 `.tmp` + rename，失败时不会有半截文件。
     *
     * @param icc 需要写入输出 JPEG 的色彩配置文件；null 表示不写（输出为 sRGB 时无需写）
     */
    fun transcodeAvifToJpeg(
        source: File,
        dest: File,
        quality: Int = ImagePipelineConfig.JPEG_QUALITY_ORIGINAL,
        icc: ByteArray? = null,
    ): Boolean {
        return try {
            val bytes = source.readBytes()
            if (!isAvifBytes(bytes)) {
                Log.w(TAG, "transcodeAvifToJpeg: 非 AVIF 输入，跳过转码")
                return false
            }
            val decoded = decodeForDisplay(bytes) ?: run {
                Log.w(TAG, "transcodeAvifToJpeg: AVIF 解码失败，走降级")
                return false
            }
            val jpeg = encodeJpeg(decoded, quality) ?: run {
                Log.w(TAG, "transcodeAvifToJpeg: JPEG 编码失败，走降级")
                return false
            }
            val output = JpegIcc.attach(jpeg, icc)

            dest.parentFile?.mkdirs()
            val tmp = File(dest.parentFile, "${dest.name}.tmp")
            tmp.writeBytes(output)
            if (!tmp.renameTo(dest)) {
                tmp.copyTo(dest, overwrite = true)
                tmp.delete()
            }
            Log.d(TAG, "transcodeAvifToJpeg: ok ${source.length()}B -> ${dest.length()}B")
            true
        } catch (e: Exception) {
            Log.w(TAG, "transcodeAvifToJpeg: 异常，走降级", e)
            false
        }
    }

    private fun scaleLongEdge(source: Bitmap, edge: Int): Bitmap {
        val scale = minOf(1f, edge.toFloat() / maxOf(source.width, source.height))
        val width = ((source.width * scale).toInt().coerceAtLeast(2) / 2) * 2
        val height = ((source.height * scale).toInt().coerceAtLeast(2) / 2) * 2
        return if (width == source.width && height == source.height) source
        else Bitmap.createScaledBitmap(source, width, height, true)
    }

    private fun centerCropThumbnail(source: Bitmap): Bitmap {
        val ratio = source.width.toFloat() / source.height
        // 超宽/超长都截中心：一（超宽）取中间横向主体，丨（超长）取中间纵向主体。
        // 最终统一 4:3，列表布局稳定；完整内容保留在原图中。
        val targetRatio = 4f / 3f
        val cropW: Int
        val cropH: Int
        if (ratio > targetRatio) {
            cropH = source.height
            cropW = (cropH * targetRatio).toInt().coerceAtMost(source.width)
        } else {
            cropW = source.width
            cropH = (cropW / targetRatio).toInt().coerceAtMost(source.height)
        }
        val left = (source.width - cropW) / 2
        val top = (source.height - cropH) / 2
        val cropped = Bitmap.createBitmap(source, left, top, cropW, cropH)
        return Bitmap.createScaledBitmap(cropped, THUMB_W, THUMB_H, true)
    }

    // 质量统一放在 ImagePipelineConfig，避免编码参数散落多处导致两端不一致
    private const val MAX_ORIGINAL_EDGE = 4096
    private const val THUMB_W = 320
    private const val THUMB_H = 240
    private const val LARGE_THUMB_EDGE = 1280
    private const val TAG = "ImageCodec"
}
