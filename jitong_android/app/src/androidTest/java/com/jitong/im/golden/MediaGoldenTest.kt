package com.jitong.im.golden

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Color
import android.net.Uri
import android.util.Log
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.jitong.im.util.ImageCodec
import kotlinx.coroutines.runBlocking
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.io.FileOutputStream

/**
 * 尺寸、裁剪、格式与颜色共同构成媒体流水线的可执行基线。
 *
 * 2026-09-08：发送侧产物由 AVIF 改为 JPEG（规避本地 AVIF 编解码偏色）。
 * 此前白色回读为 `255,172,255`（G/B 通道错位），门禁一度只能靠放宽 colorTolerance 掩盖；
 * 改 JPEG 后容差已收紧回 8，门禁恢复为真正的回归防线。
 */
@RunWith(AndroidJUnit4::class)
class MediaGoldenTest {
    @Test
    fun generatedVectorsMatchImageCodecContract() = runBlocking {
        val context = ApplicationProvider.getApplicationContext<Context>()
        val testContext = InstrumentationRegistry.getInstrumentation().context
        val root = JSONObject(testContext.assets.open("media-golden.json").bufferedReader().use { it.readText() })
        val tolerance = root.getInt("colorTolerance")
        val expectedFormat = root.optString("format", "image/jpeg")
        val cases = root.getJSONArray("cases")

        for (index in 0 until cases.length()) {
            val item = cases.getJSONObject(index)
            val id = item.getString("id")
            val sourceColor = Color.parseColor(item.getString("color"))
            val source = Bitmap.createBitmap(item.getInt("width"), item.getInt("height"), Bitmap.Config.ARGB_8888)
            source.eraseColor(sourceColor)
            val file = File(context.cacheDir, "$id.png")
            FileOutputStream(file).use { source.compress(Bitmap.CompressFormat.PNG, 100, it) }

            val result = assertNotNullResult(id, ImageCodec.loadAndCompress(context, Uri.fromFile(file)))
            assertSize(id, "original", item.getJSONArray("expectedOriginal"), result.w, result.h)
            assertSize(id, "thumbnail", item.getJSONArray("expectedThumb"), result.thumbnailW, result.thumbnailH)
            assertSize(id, "largeThumbnail", item.getJSONArray("expectedLarge"), result.largeThumbnailW, result.largeThumbnailH)
            assertFormat(expectedFormat, id, result.bytes)
            assertFormat(expectedFormat, "$id thumbnail", result.thumbnailBytes)
            assertFormat(expectedFormat, "$id largeThumbnail", result.largeThumbnailBytes)

            listOf(
                "original" to result.bytes,
                "thumbnail" to result.thumbnailBytes,
                "largeThumbnail" to result.largeThumbnailBytes,
            ).forEach { (variant, bytes) ->
                val decoded = requireNotNull(ImageCodec.decodeForDisplay(bytes)) { "$id $variant decode failed" }
                val actual = decoded.getPixel(decoded.width / 2, decoded.height / 2)
                // 输出实际回读值，便于在门禁失败时定位是偏色还是尺寸/裁剪问题
                Log.d(
                    "MediaGolden",
                    "$id/$variant expected=#%08X actual=#%08X".format(sourceColor, actual),
                )
                assertChannel(id, variant, "red", Color.red(sourceColor), Color.red(actual), tolerance)
                assertChannel(id, variant, "green", Color.green(sourceColor), Color.green(actual), tolerance)
                assertChannel(id, variant, "blue", Color.blue(sourceColor), Color.blue(actual), tolerance)
            }
            file.delete()
        }
    }

    private fun assertNotNullResult(id: String, value: ImageCodec.Compressed?): ImageCodec.Compressed {
        assertNotNull("$id encode failed", value)
        return requireNotNull(value)
    }

    private fun assertSize(id: String, variant: String, expected: org.json.JSONArray, width: Int, height: Int) {
        assertEquals("$id $variant width", expected.getInt(0), width)
        assertEquals("$id $variant height", expected.getInt(1), height)
    }

    /**
     * 按用例声明的 format 断言产物格式。
     *
     * 发送侧当前固定输出 JPEG —— 这是规避本地 AVIF 编解码偏色的工程决策。
     * 若将来重新引入 AVIF，改 `media-golden.json` 的 `format` 即可，无需改测试代码。
     */
    private fun assertFormat(format: String, id: String, bytes: ByteArray) {
        when (format) {
            "image/jpeg" -> {
                assertTrue(
                    "$id is not JPEG",
                    bytes.size >= 2 && bytes[0] == 0xFF.toByte() && bytes[1] == 0xD8.toByte(),
                )
                assertFalse("$id should not be AVIF", ImageCodec.isAvifBytes(bytes))
            }

            "image/avif" -> assertTrue(
                "$id is not AVIF",
                bytes.size >= 12 && String(bytes, 4, 8, Charsets.ISO_8859_1).contains("ftypavi"),
            )

            else -> throw AssertionError("$id unknown expected format: $format")
        }
    }

    private fun assertChannel(id: String, variant: String, channel: String, expected: Int, actual: Int, tolerance: Int) {
        assertTrue("$id $variant $channel expected=$expected actual=$actual tolerance=$tolerance", kotlin.math.abs(expected - actual) <= tolerance)
    }
}
