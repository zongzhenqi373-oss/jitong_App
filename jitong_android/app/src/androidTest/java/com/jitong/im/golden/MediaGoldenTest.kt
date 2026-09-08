package com.jitong.im.golden

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Color
import android.net.Uri
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.jitong.im.util.ImageCodec
import kotlinx.coroutines.runBlocking
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.io.FileOutputStream

/** 尺寸、裁剪、AVIF 格式与颜色共同构成媒体流水线的可执行基线。 */
@RunWith(AndroidJUnit4::class)
class MediaGoldenTest {
    @Test
    fun generatedVectorsMatchImageCodecContract() = runBlocking {
        val context = ApplicationProvider.getApplicationContext<Context>()
        val testContext = InstrumentationRegistry.getInstrumentation().context
        val root = JSONObject(testContext.assets.open("media-golden.json").bufferedReader().use { it.readText() })
        val tolerance = root.getInt("colorTolerance")
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
            assertAvif(id, result.bytes)
            assertAvif("$id thumbnail", result.thumbnailBytes)
            assertAvif("$id largeThumbnail", result.largeThumbnailBytes)

            listOf(
                "original" to result.bytes,
                "thumbnail" to result.thumbnailBytes,
                "largeThumbnail" to result.largeThumbnailBytes,
            ).forEach { (variant, bytes) ->
                val decoded = requireNotNull(ImageCodec.decodeForDisplay(bytes)) { "$id $variant decode failed" }
                val actual = decoded.getPixel(decoded.width / 2, decoded.height / 2)
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

    private fun assertAvif(id: String, bytes: ByteArray) {
        assertTrue("$id is not AVIF", bytes.size >= 12 && String(bytes, 4, 8, Charsets.ISO_8859_1).contains("ftypavi"))
    }

    private fun assertChannel(id: String, variant: String, channel: String, expected: Int, actual: Int, tolerance: Int) {
        assertTrue("$id $variant $channel expected=$expected actual=$actual tolerance=$tolerance", kotlin.math.abs(expected - actual) <= tolerance)
    }
}
