package com.jitong.im.util

import android.graphics.Bitmap
import android.graphics.Color
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.radzivon.bartoshyk.avif.coder.AvifChromaSubsampling
import com.radzivon.bartoshyk.avif.coder.AvifSurfaceMode
import com.radzivon.bartoshyk.avif.coder.HeifCoder
import com.radzivon.bartoshyk.avif.coder.PreciseMode
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * 图片流水线测试：验证「发送侧输出 JPEG、AVIF 仅作传输中间格式」的规避策略。
 *
 * ```bash
 * ./gradlew :app:connectedAndroidTest \
 *   -Pandroid.testInstrumentationRunnerArguments.class=com.jitong.im.util.ImagePipelineTest
 * ```
 */
@RunWith(AndroidJUnit4::class)
class ImagePipelineTest {

    private lateinit var workDir: File

    @Before
    fun setUp() {
        val ctx = InstrumentationRegistry.getInstrumentation().targetContext
        workDir = File(ctx.cacheDir, "img_pipeline_test").apply { mkdirs() }
        workDir.listFiles()?.forEach { it.delete() }
        ImagePipelineConfig.resetForTest(enabled = false, blocked = emptySet())
    }

    private fun testBitmap(w: Int = 64, h: Int = 48): Bitmap =
        Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888).apply {
            eraseColor(Color.rgb(200, 60, 40)) // 明显偏红，便于肉眼/像素比对
        }

    // ---------------- 发送侧：产物必须是 JPEG ----------------

    @Test
    fun encodeJpeg_producesJpegNotAvif() {
        val jpeg = ImageCodec.encodeJpeg(testBitmap(), ImagePipelineConfig.JPEG_QUALITY_ORIGINAL)

        assertNotNull("JPEG 编码不应失败", jpeg)
        assertTrue("必须以 SOI(FFD8) 开头", jpeg!![0] == 0xFF.toByte() && jpeg[1] == 0xD8.toByte())
        assertFalse("产物不能是 AVIF", ImageCodec.isAvifBytes(jpeg))
    }

    @Test
    fun encodeJpeg_honorsQuality() {
        val bmp = testBitmap(512, 384)
        val high = ImageCodec.encodeJpeg(bmp, 95)!!
        val low = ImageCodec.encodeJpeg(bmp, 30)!!
        assertTrue("高质量应产生更大体积", high.size > low.size)
    }

    // ---------------- AVIF 魔数检测 ----------------

    @Test
    fun isAvifBytes_detectsAvifAndRejectsJpeg() {
        val avif = HeifCoder().encodeAvif(
            testBitmap(),
            quality = 80,
            preciseMode = PreciseMode.LOSSY,
            surfaceMode = AvifSurfaceMode.RGB,
            avifChromaSubsampling = AvifChromaSubsampling.YUV444,
        )
        assertTrue("libavif 产物应被识别为 AVIF", ImageCodec.isAvifBytes(avif))

        val jpeg = ImageCodec.encodeJpeg(testBitmap(), 90)!!
        assertFalse("JPEG 不应被识别为 AVIF", ImageCodec.isAvifBytes(jpeg))
        assertFalse("过短数据不应被识别", ImageCodec.isAvifBytes(ByteArray(8)))
    }

    // ---------------- 下载侧：AVIF → JPEG 转码 ----------------

    @Test
    fun transcodeAvifToJpeg_convertsAndDeletesSource() {
        val avifFile = File(workDir, "in.avif")
        avifFile.writeBytes(
            HeifCoder().encodeAvif(
                testBitmap(128, 96),
                quality = 80,
                preciseMode = PreciseMode.LOSSY,
                surfaceMode = AvifSurfaceMode.RGB,
                avifChromaSubsampling = AvifChromaSubsampling.YUV444,
            )
        )
        val jpegFile = File(workDir, "out.jpg")

        assertTrue("转码应成功", ImageCodec.transcodeAvifToJpeg(avifFile, jpegFile))
        assertTrue("应产出目标文件", jpegFile.isFile)

        val out = jpegFile.readBytes()
        assertTrue("输出必须是 JPEG", out[0] == 0xFF.toByte() && out[1] == 0xD8.toByte())
        assertFalse("输出不能是 AVIF", ImageCodec.isAvifBytes(out))
        assertFalse("不得残留 .tmp", File(workDir, "out.jpg.tmp").exists())
    }

    @Test
    fun transcode_rejectsNonAvifInput() {
        // 降级分支：输入不是 AVIF 时应返回 false，且不产出目标文件
        val fake = File(workDir, "fake.avif").apply { writeBytes(ByteArray(64) { 0x41 }) }
        val dest = File(workDir, "should_not_exist.jpg")

        assertFalse(ImageCodec.transcodeAvifToJpeg(fake, dest))
        assertFalse("非 AVIF 输入不应产出目标文件", dest.exists())
    }

    @Test
    fun transcode_samePathDoesNotDeleteOutput() {
        // 下载路径就是 .jpg 时（内容是 AVIF），转码后不能把刚写好的文件删掉
        val avif = HeifCoder().encodeAvif(
            testBitmap(),
            quality = 80,
            preciseMode = PreciseMode.LOSSY,
            surfaceMode = AvifSurfaceMode.RGB,
            avifChromaSubsampling = AvifChromaSubsampling.YUV444,
        )
        val same = File(workDir, "same.jpg").apply { writeBytes(avif) }

        assertTrue(ImageCodec.transcodeAvifToJpeg(same, same))
        assertTrue("同名转码后文件必须仍在", same.isFile)
        val out = same.readBytes()
        assertTrue("同名转码后内容必须是 JPEG", out[0] == 0xFF.toByte() && out[1] == 0xD8.toByte())
    }

    // ---------------- 远程开关与机型黑名单 ----------------

    @Test
    fun avifTransport_defaultDisabled() {
        assertFalse("默认应关闭 AVIF 传输", ImagePipelineConfig.isAvifTransportAllowed())
    }

    @Test
    fun avifTransport_enabledThenBlockedByModel() {
        ImagePipelineConfig.updateFromRemote(enabled = true)
        assertTrue(ImagePipelineConfig.isAvifTransportAllowed())

        // 命中当前机型 → 强制走常规下载
        val model = android.os.Build.MODEL
        ImagePipelineConfig.updateFromRemote(enabled = true, blocked = setOf(model))
        assertFalse("命中黑名单机型应被禁用", ImagePipelineConfig.isAvifTransportAllowed())

        // 黑名单为空且开启 → 恢复
        ImagePipelineConfig.updateFromRemote(enabled = true, blocked = emptySet())
        assertTrue(ImagePipelineConfig.isAvifTransportAllowed())
    }

    @Test
    fun avifTransport_blockedEvenIfModelUnknown() {
        ImagePipelineConfig.updateFromRemote(enabled = false)
        assertFalse("总开关关闭时不走 AVIF", ImagePipelineConfig.isAvifTransportAllowed())
    }

    // ---------------- 输出格式常量 ----------------

    @Test
    fun outputConstants_areJpeg() {
        assertEquals("image/jpeg", ImageCodec.OUTPUT_MIME)
        assertEquals(".jpg", ImageCodec.OUTPUT_EXT)
        assertEquals("image/jpeg", ImagePipelineConfig.MIME_JPEG)
        assertEquals(90, ImagePipelineConfig.JPEG_QUALITY_ORIGINAL)
    }
}
