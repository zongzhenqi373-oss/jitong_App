package com.jitong.im.util

import java.io.ByteArrayOutputStream

/**
 * 向 JPEG 写入 ICC 色彩配置文件（APP2 / `ICC_PROFILE` 段）。
 *
 * 存在意义：AVIF 转 JPEG 时，如果 **需要保留宽色域**（Display P3 等），
 * 输出 JPEG 必须携带正确的色彩配置文件，否则解码端只能按 sRGB 猜测，
 * 结果就是饱和度偏差 —— 这正是「转码后看起来像加了滤镜」的另一类根因。
 *
 * 当前主流程走「统一落到 sRGB」的工程规避路径，此时 **不需要写 ICC**
 * （JPEG 默认即 sRGB），所以 [attach] 在 `icc == null` 时原样返回。
 * 接口在这里，是为了宽色域保真（P10，NDK 层 libavif + libjpeg-turbo）时可直接复用。
 *
 * 布局（每个 APP2 段）：
 * ```
 * FF E2 | length(2B, 含自身) | "ICC_PROFILE\0"(12B) | seq(1B, 1-based) | count(1B) | data
 * ```
 * ICC 超过单段上限时按 [MAX_CHUNK] 切分成多段，`seq/count` 用于重组。
 */
object JpegIcc {

    private val ICC_SIGNATURE = "ICC_PROFILE\u0000".toByteArray(Charsets.ISO_8859_1)

    /** APP2 段最大数据载荷：段长度字段 65535 − 长度自身 2 − 签名 12 − seq 1 − count 1。 */
    private const val MAX_CHUNK = 65535 - 2 - 12 - 1 - 1

    /**
     * 把 [icc] 写入 [jpeg]。
     *
     * - [icc] 为 null/空 → 原样返回（不写）；
     * - 输入不是 JPEG、或已含 ICC → 原样返回（不重复写、不产出非法文件）；
     * - 任何异常 → 原样返回，宁可不写 ICC 也不能产出损坏的 JPEG。
     */
    fun attach(jpeg: ByteArray, icc: ByteArray?): ByteArray {
        if (icc == null || icc.isEmpty()) return jpeg
        if (jpeg.size < 4) return jpeg
        if (jpeg[0] != 0xFF.toByte() || jpeg[1] != 0xD8.toByte()) return jpeg // 无 SOI
        if (hasIcc(jpeg)) return jpeg

        return try {
            val chunks = chunk(icc)
            val out = ByteArrayOutputStream(jpeg.size + icc.size + chunks.size * 18)
            out.write(0xFF)
            out.write(0xD8) // SOI
            chunks.forEachIndexed { index, data ->
                val segLen = 2 + ICC_SIGNATURE.size + 1 + 1 + data.size
                out.write(0xFF)
                out.write(0xE2) // APP2
                out.write((segLen shr 8) and 0xFF)
                out.write(segLen and 0xFF)
                out.write(ICC_SIGNATURE)
                out.write(index + 1) // seq，1-based
                out.write(chunks.size) // count
                out.write(data)
            }
            // SOI 之后的原始内容原样保留（含 APP0/JFIF、量化表、扫描数据）
            out.write(jpeg, 2, jpeg.size - 2)
            out.toByteArray()
        } catch (e: Exception) {
            jpeg
        }
    }

    /** 是否已包含 ICC_PROFILE 段。 */
    fun hasIcc(jpeg: ByteArray): Boolean {
        if (jpeg.size < 4) return false
        if (jpeg[0] != 0xFF.toByte() || jpeg[1] != 0xD8.toByte()) return false
        var offset = 2
        while (offset + 4 <= jpeg.size) {
            if (jpeg[offset] != 0xFF.toByte()) { offset++; continue } // 填充字节
            val marker = jpeg[offset + 1].toInt() and 0xFF
            // SOI / EOI / RSTn / TEM 没有长度字段
            if (marker == 0xD8 || marker == 0x01 || (marker in 0xD0..0xD7)) { offset += 2; continue }
            if (marker == 0xD9 || marker == 0xDA) return false // 进入压缩数据，后面不可能再有 APP2
            if (offset + 4 > jpeg.size) return false
            val length = ((jpeg[offset + 2].toInt() and 0xFF) shl 8) or (jpeg[offset + 3].toInt() and 0xFF)
            if (length < 2) return false
            if (marker == 0xE2 && offset + 4 + ICC_SIGNATURE.size <= jpeg.size) {
                var same = true
                for (i in ICC_SIGNATURE.indices) {
                    if (jpeg[offset + 4 + i] != ICC_SIGNATURE[i]) { same = false; break }
                }
                if (same) return true
            }
            offset += 2 + length
        }
        return false
    }

    private fun chunk(icc: ByteArray): List<ByteArray> {
        if (icc.size <= MAX_CHUNK) return listOf(icc)
        val result = ArrayList<ByteArray>((icc.size / MAX_CHUNK) + 1)
        var offset = 0
        while (offset < icc.size) {
            val size = minOf(MAX_CHUNK, icc.size - offset)
            result.add(icc.copyOfRange(offset, offset + size))
            offset += size
        }
        return result
    }
}
