package com.jitong.im.core.platform

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.ColorSpace
import com.radzivon.bartoshyk.avif.coder.AvifChromaSubsampling
import com.radzivon.bartoshyk.avif.coder.AvifSurfaceMode
import com.radzivon.bartoshyk.avif.coder.HeifCoder
import com.radzivon.bartoshyk.avif.coder.PreciseMode
import java.io.ByteArrayOutputStream
import java.io.FileInputStream
import java.nio.ByteBuffer

/**
 * [ImagePlatform] 的 Android 实现。
 *
 * 只按**内核给定的参数**执行解码/编码：
 * 目标尺寸、质量、是否生成缩略图都由 C++ ThumbnailPipeline 决定（P10）。
 *
 * 统一落到 sRGB + RGBA8：跨端（Android/iOS/桌面）解码同一份 AVIF 时
 * 不因色域解释差异出现偏色。
 */
class AndroidImagePlatform : ImagePlatform {

    override fun probe(handle: PlatformFileHandle): PlatformImageInfo? {
        val path = handle.localPath ?: return null
        return runCatching {
            val options = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            FileInputStream(path).use { BitmapFactory.decodeStream(it, null, options) }
            if (options.outWidth <= 0 || options.outHeight <= 0) return null
            PlatformImageInfo(
                width = options.outWidth,
                height = options.outHeight,
                mime = options.outMimeType.orEmpty(),
            )
        }.getOrNull()
    }

    override fun decode(handle: PlatformFileHandle, options: PlatformDecodeOptions): ByteArray? {
        val path = handle.localPath ?: return null
        return runCatching {
            val bitmap = BitmapFactory.decodeFile(
                path,
                BitmapFactory.Options().apply {
                    inPreferredConfig = Bitmap.Config.ARGB_8888
                    inPreferredColorSpace = ColorSpace.get(ColorSpace.Named.SRGB)
                },
            ) ?: return null
            val scaled = scaleIfNeeded(bitmap, options)
            toRgba8(toSrgb(scaled))
        }.getOrNull()
    }

    override fun encodeJpeg(
        rgba8: ByteArray,
        width: Int,
        height: Int,
        options: PlatformEncodeOptions,
    ): ByteArray? = runCatching {
        val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888).apply {
            copyPixelsFromBuffer(ByteBuffer.wrap(rgba8))
        }
        ByteArrayOutputStream().use { out ->
            if (bitmap.compress(Bitmap.CompressFormat.JPEG, options.quality.coerceIn(0, 100), out)) {
                out.toByteArray()
            } else {
                null
            }
        }
    }.getOrNull()

    // 仅用于「传输中间格式」，不是发送侧出口。
    // 已知缺陷：部分 ABI/设备解码出现 G/B 通道错位（纯白回读 255,172,255），
    // 与 surfaceMode/chromaSubsampling 无关，属 avif-coder 库问题。详见接口注释。
    override fun encodeAvif(
        rgba8: ByteArray,
        width: Int,
        height: Int,
        options: PlatformEncodeOptions,
    ): ByteArray? = runCatching {
        require(rgba8.size == width * height * 4) { "RGBA8 byte count does not match dimensions" }
        val argb = IntArray(width * height)
        for (i in argb.indices) {
            val p = i * 4
            val r = rgba8[p].toInt() and 0xff
            val g = rgba8[p + 1].toInt() and 0xff
            val b = rgba8[p + 2].toInt() and 0xff
            val a = rgba8[p + 3].toInt() and 0xff
            argb[i] = (a shl 24) or (r shl 16) or (g shl 8) or b
        }
        val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888).apply {
            setPixels(argb, 0, width, 0, 0, width, height)
            // 即通图片流水线当前统一输出不透明 SDR；同步 Bitmap 元数据，避免编码器
            // 为实际全不透明的图片额外生成 alpha plane。
            setHasAlpha(false)
        }
        // 与 ImageCodec 保持一致：q100/YUV444 走标准 YUV 路径，
        // 避免部分 ABI 上 lossless chroma 解码出现 G/B 通道错位（整图绿/洋红）。
        // 用命名参数，speed 保持默认值。
        //
        // surfaceMode 必须是 AUTO 而不是 RGB：`AvifSurfaceMode.RGB` 会让编码器按 RGB
        // 平面而非 YUV 平面组织数据，配合 YUV444 在部分 ABI 上解码出现 G/B 通道错位
        // —— Round11ClosureTest 实测纯白回读为 255,172,255（G 通道掉到 172）。
        // AUTO 走标准 YUV 路径，回读 255,255,255。
        HeifCoder().encodeAvif(
            bitmap,
            quality = options.quality,
            preciseMode = PreciseMode.LOSSY,
            surfaceMode = AvifSurfaceMode.AUTO,
            avifChromaSubsampling = AvifChromaSubsampling.YUV444,
        )
    }.getOrNull()

    private fun scaleIfNeeded(source: Bitmap, options: PlatformDecodeOptions): Bitmap {
        if (options.targetWidth <= 0 || options.targetHeight <= 0) return source
        if (source.width == options.targetWidth && source.height == options.targetHeight) return source
        return Bitmap.createScaledBitmap(source, options.targetWidth, options.targetHeight, true)
    }

    /** 统一落到不透明 sRGB，作为跨平台像素契约。 */
    private fun toSrgb(source: Bitmap): Bitmap {
        val converted = Bitmap.createBitmap(
            source.width,
            source.height,
            Bitmap.Config.ARGB_8888,
            false,
            ColorSpace.get(ColorSpace.Named.SRGB),
        )
        Canvas(converted).apply {
            drawColor(android.graphics.Color.WHITE)
            drawBitmap(source, 0f, 0f, null)
        }
        return converted
    }

    private fun toRgba8(bitmap: Bitmap): ByteArray {
        val pixels = IntArray(bitmap.width * bitmap.height)
        bitmap.getPixels(pixels, 0, bitmap.width, 0, 0, bitmap.width, bitmap.height)
        val rgba = ByteArray(pixels.size * 4)
        for (i in pixels.indices) {
            val color = pixels[i]
            val p = i * 4
            rgba[p] = ((color ushr 16) and 0xff).toByte()
            rgba[p + 1] = ((color ushr 8) and 0xff).toByte()
            rgba[p + 2] = (color and 0xff).toByte()
            rgba[p + 3] = ((color ushr 24) and 0xff).toByte()
        }
        return rgba
    }
}
