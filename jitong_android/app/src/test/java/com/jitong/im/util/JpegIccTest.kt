package com.jitong.im.util

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

/** JPEG ICC profile 写入（APP2 / ICC_PROFILE 段）。纯 JVM，不需要 Android 环境。 */
class JpegIccTest {

    /** 最小合法形状：SOI + APP0(JFIF) + 压缩数据 + EOI */
    private fun fakeJpeg(): ByteArray = byteArrayOf(
        0xFF.toByte(), 0xD8.toByte(), // SOI
        0xFF.toByte(), 0xE0.toByte(), // APP0
        0x00, 0x10, // length = 16（含自身）
        'J'.code.toByte(), 'F'.code.toByte(), 'I'.code.toByte(), 'F'.code.toByte(), 0x00,
        0x01, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
        0x01, 0x02, 0x03, 0x04, // 假压缩数据
        0xFF.toByte(), 0xD9.toByte(), // EOI
    )

    private fun iccOf(size: Int): ByteArray = ByteArray(size) { (it % 251).toByte() }

    @Test
    fun nullIcc_returnsOriginal() {
        val jpeg = fakeJpeg()
        assertSame(jpeg, JpegIcc.attach(jpeg, null))
        assertSame(jpeg, JpegIcc.attach(jpeg, ByteArray(0)))
    }

    @Test
    fun nonJpegInput_returnsOriginal() {
        val notJpeg = byteArrayOf(0x00, 0x01, 0x02, 0x03, 0x04, 0x05)
        assertSame(notJpeg, JpegIcc.attach(notJpeg, iccOf(128)))
    }

    @Test
    fun tooShort_returnsOriginal() {
        val tiny = byteArrayOf(0xFF.toByte(), 0xD8.toByte())
        assertSame(tiny, JpegIcc.attach(tiny, iccOf(128)))
    }

    @Test
    fun attach_writesApp2RightAfterSoi() {
        val jpeg = fakeJpeg()
        val out = JpegIcc.attach(jpeg, iccOf(100))

        assertTrue("SOI 保留", out[0] == 0xFF.toByte() && out[1] == 0xD8.toByte())
        assertEquals("SOI 后紧跟 APP2", 0xE2.toByte(), out[3])
        assertEquals(0xFF.toByte(), out[2])
        assertTrue("写入后可检出 ICC", JpegIcc.hasIcc(out))
    }

    @Test
    fun attach_preservesOriginalPayload() {
        val jpeg = fakeJpeg()
        val icc = iccOf(100)
        val out = JpegIcc.attach(jpeg, icc)

        // 输出 = SOI(2) + APP2段(2+2+12+1+1+100) + 原始 SOI 之后的内容
        val app2Len = 2 + 12 + 1 + 1 + 100
        assertEquals(2 + 2 + app2Len + (jpeg.size - 2), out.size)

        val tailStart = 2 + 2 + app2Len
        assertArrayEquals(
            "SOI 之后的原始内容必须原样保留",
            jpeg.copyOfRange(2, jpeg.size),
            out.copyOfRange(tailStart, out.size),
        )
    }

    @Test
    fun attach_lengthFieldIsCorrect() {
        val icc = iccOf(300)
        val out = JpegIcc.attach(fakeJpeg(), icc)
        val len = ((out[4].toInt() and 0xFF) shl 8) or (out[5].toInt() and 0xFF)
        // length 含自身 2 字节 + 签名 12 + seq 1 + count 1 + data
        assertEquals(2 + 12 + 1 + 1 + 300, len)
    }

    @Test
    fun attach_splitsLargeProfileIntoMultipleChunks() {
        // 超过单段上限（65535-16），必须切成多段
        val icc = iccOf(70000)
        val out = JpegIcc.attach(fakeJpeg(), icc)

        var offset = 2
        var chunks = 0
        var declaredCount = -1
        var lastSeq = -1
        while (offset + 4 <= out.size) {
            val marker = out[offset + 1].toInt() and 0xFF
            if (marker == 0xDA || marker == 0xD9) break
            val len = ((out[offset + 2].toInt() and 0xFF) shl 8) or (out[offset + 3].toInt() and 0xFF)
            if (marker == 0xE2) {
                chunks++
                lastSeq = out[offset + 4 + 12].toInt() and 0xFF // seq 在签名之后
                declaredCount = out[offset + 4 + 13].toInt() and 0xFF // count
            }
            offset += 2 + len
        }
        assertEquals("应切成 2 段", 2, chunks)
        assertEquals("count 与段数一致", 2, declaredCount)
        assertEquals("seq 递增到段数", 2, lastSeq)
        assertTrue(JpegIcc.hasIcc(out))
    }

    @Test
    fun alreadyHasIcc_isNotWrittenTwice() {
        val once = JpegIcc.attach(fakeJpeg(), iccOf(100))
        val twice = JpegIcc.attach(once, iccOf(100))
        assertSame("已有 ICC 时不应重复写入", once, twice)
    }

    @Test
    fun hasIcc_falseForPlainJpeg() {
        assertFalse(JpegIcc.hasIcc(fakeJpeg()))
    }
}
