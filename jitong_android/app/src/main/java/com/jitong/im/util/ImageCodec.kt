package com.jitong.im.util

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream

/**
 * 相册图片处理：读取 → 等比压缩到长边 ≤1024px → JPEG q80。
 * 对齐技术文档 4.3 图片流程：压缩后内联发送（≤500KB 量级，不阻塞信令通道）。
 */
object ImageCodec {

    data class Compressed(val bytes: ByteArray, val w: Int, val h: Int)

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
                while (longEdge / (sample * 2) >= MAX_EDGE) sample *= 2
                val opts = BitmapFactory.Options().apply { inSampleSize = sample }
                val sampled = resolver.openInputStream(uri)?.use {
                    BitmapFactory.decodeStream(it, null, opts)
                } ?: return@withContext null

                // 精确缩放到长边 1024 以内
                val scale = minOf(1f, MAX_EDGE.toFloat() / maxOf(sampled.width, sampled.height))
                val final = if (scale < 1f) {
                    Bitmap.createScaledBitmap(
                        sampled,
                        (sampled.width * scale).toInt().coerceAtLeast(1),
                        (sampled.height * scale).toInt().coerceAtLeast(1),
                        true,
                    )
                } else sampled

                val out = ByteArrayOutputStream()
                final.compress(Bitmap.CompressFormat.JPEG, JPEG_QUALITY, out)
                Compressed(out.toByteArray(), final.width, final.height)
            } catch (e: Exception) {
                null
            }
        }

    private const val MAX_EDGE = 1024
    private const val JPEG_QUALITY = 80
}
