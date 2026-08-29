package com.jitong.im.net

import com.jitong.im.data.Prefs
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.MediaType.Companion.toMediaTypeOrNull
import okhttp3.CertificatePinner
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody
import okhttp3.Response
import okio.BufferedSink
import org.json.JSONObject
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.util.concurrent.TimeUnit

data class MediaUploadResult(
    val fileId: String,
    val sha256: String,
    val size: Long,
    val contentType: String,
)

/** 图片和文件共用的 HTTPS 字节通道；IM 长连接只传 file_id 和元数据。 */
class HttpMediaClient(
    private val host: String = ImClient.DEFAULT_HOST,
    private val port: Int = Protocol.HTTPS_FILE_PORT,
) {
    private val client = OkHttpClient.Builder()
        .certificatePinner(
            CertificatePinner.Builder()
                .add(host, TlsPinning.CURRENT_PIN)
                .build(),
        )
        .connectTimeout(10, TimeUnit.SECONDS)
        .readTimeout(2, TimeUnit.MINUTES)
        .writeTimeout(2, TimeUnit.MINUTES)
        .build()

    private fun auth(builder: Request.Builder): Request.Builder {
        val token = Prefs.loadTokenSession()?.accessToken
            ?: throw IllegalStateException("登录凭证已失效")
        return builder
            .header("Authorization", "Bearer $token")
            .header("X-Device-Id", Prefs.deviceId)
    }

    suspend fun upload(
        file: File,
        receiverId: Int,
        fileName: String,
        contentType: String,
        onProgress: (Long, Long) -> Unit = { _, _ -> },
    ): MediaUploadResult = withContext(Dispatchers.IO) {
        require(file.isFile && file.length() in 1..Protocol.FILE_MAX_SIZE) { "文件大小不合法" }
        val body = object : RequestBody() {
            override fun contentType() = contentType.toMediaTypeOrNull()
            override fun contentLength() = file.length()
            override fun writeTo(sink: BufferedSink) {
                FileInputStream(file).use { input ->
                    val buffer = ByteArray(64 * 1024)
                    var sent = 0L
                    while (true) {
                        val n = input.read(buffer)
                        if (n < 0) break
                        sink.write(buffer, 0, n)
                        sent += n
                        onProgress(sent, file.length())
                    }
                }
            }
        }
        val safeName = File(fileName).name.ifBlank { "file" }.take(255)
        val request = auth(Request.Builder())
            .url("https://$host:$port/api/v1/upload")
            .header("X-File-Name", safeName)
            .header("X-Receiver-Id", receiverId.toString())
            .post(body)
            .build()
        client.newCall(request).execute().use { response ->
            checkSuccess(response)
            val json = JSONObject(response.body?.string().orEmpty())
            MediaUploadResult(
                json.getString("file_id"), json.getString("sha256"),
                json.getLong("size"), json.getString("content_type"),
            )
        }
    }

    /** Range 断点续传到 .part，完成后校验 SHA-256 再原子替换目标文件。 */
    suspend fun download(
        fileId: String,
        destination: File,
        expectedSha256: String = "",
        onProgress: (Long, Long) -> Unit = { _, _ -> },
    ): File = withContext(Dispatchers.IO) {
        require(fileId.matches(Regex("[A-Za-z0-9._-]+"))) { "file_id 非法" }
        destination.parentFile?.mkdirs()
        val part = File(destination.absolutePath + ".part")
        var offset = part.takeIf { it.exists() }?.length() ?: 0L
        val builder = auth(Request.Builder()).url("https://$host:$port/api/v1/download/$fileId")
        if (offset > 0) builder.header("Range", "bytes=$offset-")
        client.newCall(builder.get().build()).execute().use { response ->
            checkSuccess(response, allowPartial = true)
            if (offset > 0 && response.code == 200) {
                part.delete()
                offset = 0
            }
            val total = if (response.code == 206) offset + (response.body?.contentLength() ?: 0L)
                        else response.body?.contentLength() ?: 0L
            FileOutputStream(part, offset > 0).use { output ->
                val input = response.body?.byteStream() ?: error("下载响应为空")
                val buffer = ByteArray(64 * 1024)
                var received = offset
                while (true) {
                    val n = input.read(buffer)
                    if (n < 0) break
                    output.write(buffer, 0, n)
                    received += n
                    onProgress(received, total)
                }
                output.fd.sync()
            }
        }
        if (expectedSha256.isNotBlank()) {
            val actual = FileInputStream(part).use(::sha256HexOfStream)
            check(actual.equals(expectedSha256, ignoreCase = true)) { "文件摘要校验失败" }
        }
        if (destination.exists() && !destination.delete()) error("无法覆盖旧文件")
        if (!part.renameTo(destination)) {
            part.copyTo(destination, overwrite = true)
            part.delete()
        }
        destination
    }

    private fun checkSuccess(response: Response, allowPartial: Boolean = false) {
        if (response.code == 401) throw IllegalStateException("登录凭证已过期，请重试")
        if (response.code == 403) throw IllegalStateException("没有文件访问权限")
        if (response.code == 413) throw IllegalArgumentException("文件超过服务器限制")
        if (response.code == 415) throw IllegalArgumentException("图片格式不受支持")
        if (!response.isSuccessful || (!allowPartial && response.code != 200)) {
            throw IllegalStateException("文件服务错误 HTTP ${response.code}")
        }
    }
}
