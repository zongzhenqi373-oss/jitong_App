package com.jitong.im.util

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.ColorSpace
import android.net.Uri
import android.os.Build
import com.radzivon.bartoshyk.avif.coder.HeifCoder
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * 相册图片处理：原图统一转 JPEG（sRGB、不透明）并生成独立 JPEG 缩略图。
 * 缩略图采用 centerCrop：普通图片裁为 4:3；超过 3:1 的长图/宽图先截取中心区域，
 * 避免“一条线”式缩略图。原始宽高仍随消息发送，点开后展示完整图片。
 *
 * 决策：弃用 AVIF（avif-coder）编码。该库在部分 ABI（尤其 x86_64 模拟器）存在
 * G/B 通道错位，且系统 AVIF 解码器在部分设备解码失败，导致图片整图偏色。
 * JPEG 是所有 Android 系统解码器稳定支持、颜色准确的 IM 事实标准，可根除偏色。
 */
object ImageCodec {

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

                // 原图只限制极端分辨率，JPEG 高质量尽量保留视觉质量。
                val scale = minOf(1f, MAX_ORIGINAL_EDGE.toFloat() / maxOf(sampled.width, sampled.height))
                val final = if (scale < 1f) {
                    Bitmap.createScaledBitmap(
                        sampled,
                        (sampled.width * scale).toInt().coerceAtLeast(1),
                        (sampled.height * scale).toInt().coerceAtLeast(1),
                        true,
                    )
                } else sampled

                // 相册图片可能是 Display P3。统一转换到 sRGB 后再编码，避免不同设备解码
                // 时因色域解释不同出现偏色、饱和度变化（看起来像加了滤镜）。
                val encodable = toSrgb(final)
                val originalBytes = encodable.toJpeg(IMAGE_QUALITY)

                val thumb = centerCropThumbnail(encodable)
                val thumbnailBytes = thumb.toJpeg(THUMB_QUALITY)

                val largeThumb = scaleLongEdge(encodable, LARGE_THUMB_EDGE)
                val largeThumbnailBytes = largeThumb.toJpeg(LARGE_THUMB_QUALITY)
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
     * 展示端优先使用 Android 系统解码器（JPEG/PNG/WebP 稳定、颜色准确）。
     * 第三方 avif-coder 仅用于兼容历史已发出的 AVIF 旧消息（其部分 ABI 存在
     * G/B 通道错位，新消息不再由它产生）。
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
        if (systemDecoded != null) return stripHdrGainmap(systemDecoded)

        // 仅兜底历史 AVIF：isAvif 命中才走 avif-coder，其余格式交由系统解码器处理。
        return runCatching {
            val coder = HeifCoder()
            if (coder.isAvif(bytes)) stripHdrGainmap(coder.decode(bytes)) else null
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

    private fun toSrgb(source: Bitmap): Bitmap {
        val srgb = ColorSpace.get(ColorSpace.Named.SRGB)
        // 即时通讯图片统一落为不透明 SDR RGB。部分相册截图虽然所有像素均不透明，
        // Bitmap 元数据仍标记 hasAlpha=true，导致 AVIF AUTO/RGBA 编码路径发生通道错位。
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

    private fun Bitmap.toJpeg(quality: Int): ByteArray {
        val out = java.io.ByteArrayOutputStream()
        check(compress(Bitmap.CompressFormat.JPEG, quality, out)) { "JPEG 编码失败" }
        return out.toByteArray()
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

    private const val MAX_ORIGINAL_EDGE = 4096
    private const val IMAGE_QUALITY = 90
    private const val THUMB_QUALITY = 82
    private const val THUMB_W = 320
    private const val THUMB_H = 240
    private const val LARGE_THUMB_EDGE = 1280
    private const val LARGE_THUMB_QUALITY = 90
    private const val TAG = "ImageCodec"
}
