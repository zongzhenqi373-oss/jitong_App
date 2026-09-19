package com.jitong.im.core

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import com.jitong.im.util.ImageCodec
import java.io.File
import java.io.FileInputStream
import java.security.MessageDigest
import java.util.concurrent.ConcurrentHashMap
import java.util.UUID
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlin.coroutines.coroutineContext

/** Android 平台媒体桥：SAF/Codec/文件原子操作留在平台层，上传、消息和 ACK 由 Native 处理。 */
class NativeMediaController(
    context:Context,private val sdk:JitongSdk,private val scope:CoroutineScope,
) {
    enum class Status { Idle, Working, Ready, Failed, Canceled }
    data class Local(val path:String="",val status:Status=Status.Idle,val error:String?=null)
    private val app=context.applicationContext
    private val root=File(app.filesDir,"native_media").apply { mkdirs() }
    private val _local=MutableStateFlow<Map<String,Local>>(emptyMap())
    val local:StateFlow<Map<String,Local>> = _local
    private val operations=ConcurrentHashMap<String,Long>()
    private val jobs=ConcurrentHashMap<String,Job>()
    /** 上传任务：token=msgId（上传草稿主键），取消时直接终止草稿。 */
    private val uploads=ConcurrentHashMap<String,String>()
    private data class DownloadLease(val taskId:String,val generation:Long,val part:File)
    private val downloads=ConcurrentHashMap<String,DownloadLease>()

    init { scope.launch(Dispatchers.IO) { recoverDownloads() } }

    /** UI 只提交取消意图；上传草稿写终态（服务端会话 best-effort 取消），下载走 Native flag。 */
    fun cancel(token:String) {
        uploads.remove(token)?.let(sdk::cancelUpload) // 终态 Cancelled，不自动恢复
        operations[token]?.let(sdk::cancelMediaOperation)
        jobs[token]?.cancel()
        downloads[token]?.let { lease -> scope.launch(Dispatchers.IO) {
            sdk.finishDownloadTask(lease.taskId,lease.generation,4,lease.part.length())
        } }
        _local.update { current -> current[token]?.let { current+(token to it.copy(status=Status.Canceled)) } ?: current }
    }

    /**
     * 分片上传（断点续传）：图片的原图/大图/小图各有独立上传草稿，共享固定 msgId；
     * 上传全部完成后由 Native MediaService 以固定 msgId 落消息卡片+Outbox 并发送。
     * 中断（断网/杀进程）后草稿保留，由 resumeUploads/pumpUpload 恢复。
     */
    fun sendImage(conversationId:Long,peerId:Long,image:ImageCodec.Compressed) = scope.launch(Dispatchers.IO) {
        val msgId="m-"+UUID.randomUUID().toString()
        uploads[msgId]=msgId;jobs[msgId]=coroutineContext[Job]!!
        _local.update { it+(msgId to Local(status=Status.Working)) }
        try { runCatching {
            val original=write(msgId+ImageCodec.OUTPUT_EXT,image.bytes)
            val small=write("${msgId}_small${ImageCodec.OUTPUT_EXT}",image.thumbnailBytes)
            val large=write("${msgId}_large${ImageCodec.OUTPUT_EXT}",image.largeThumbnailBytes)
            coroutineContext.ensureActive()
            require(sdk.enqueueUpload(msgId,conversationId,peerId,2,small.absolutePath,small.name,
                ImageCodec.OUTPUT_MIME,image.thumbnailW,image.thumbnailH)){"小图草稿登记失败"}
            require(sdk.enqueueUpload(msgId,conversationId,peerId,1,large.absolutePath,large.name,
                ImageCodec.OUTPUT_MIME,image.largeThumbnailW,image.largeThumbnailH)){"大图草稿登记失败"}
            require(sdk.enqueueUpload(msgId,conversationId,peerId,0,original.absolutePath,original.name,
                ImageCodec.OUTPUT_MIME,image.w,image.h)){"原图草稿登记失败"}
            pumpWithRetry(msgId)
            _local.update { it+(msgId to Local(original.absolutePath,Status.Ready)) }
        }.onFailure { error -> _local.update { it+(msgId to Local(status=if(error is CancellationException)Status.Canceled else Status.Failed,error=error.message)) } }
        } finally { uploads.remove(msgId);jobs.remove(msgId) }
    }

    fun sendFile(conversationId:Long,peerId:Long,uri:Uri) = scope.launch(Dispatchers.IO) {
        val msgId="m-"+UUID.randomUUID().toString()
        uploads[msgId]=msgId;jobs[msgId]=coroutineContext[Job]!!
        _local.update { it+(msgId to Local(status=Status.Working)) }
        try { runCatching {
            val (name,size)=query(uri);require(size in 1..MAX_FILE){"文件大小必须在 1B–100MB"}
            val dest=File(root,"${safeId(msgId)}_${sanitize(name)}")
            var copied=false
            try {
                app.contentResolver.openInputStream(uri)!!.use { input -> dest.outputStream().use { output ->
                    val buffer=ByteArray(64*1024)
                    while(true){
                        coroutineContext.ensureActive()
                        val n=input.read(buffer);if(n<0)break
                        output.write(buffer,0,n)
                    }
                    output.fd.sync()
                } }
                copied=true
            } finally { if(!copied)dest.delete() }
            require(dest.length()==size){"文件读取不完整"}
            coroutineContext.ensureActive()
            require(sdk.enqueueUpload(msgId,conversationId,peerId,0,dest.absolutePath,name,
                app.contentResolver.getType(uri)?:"application/octet-stream")){"文件草稿登记失败"}
            pumpWithRetry(msgId)
            _local.update { it+(msgId to Local(dest.absolutePath,Status.Ready)) }
        }.onFailure { error -> _local.update { it+(msgId to Local(status=if(error is CancellationException)Status.Canceled else Status.Failed,error=error.message)) } }
        } finally { uploads.remove(msgId);jobs.remove(msgId) }
    }

    /** 推进上传：网络失败按 1s/2s/4s 退避重试；终态（取消/已发）立即停止；仍失败则保留草稿待恢复。 */
    private suspend fun pumpWithRetry(msgId:String) {
        var delayMs=1000L
        repeat(4) { attempt ->
            coroutineContext.ensureActive()
            if(sdk.pumpUpload(msgId))return
            val state=sdk.uploadState(msgId)?.substringBefore(',')
            if(state=="4")throw CancellationException("用户取消") // Cancelled 终态不重试
            if(state=="3")return // Sent
            if(attempt<3){kotlinx.coroutines.delay(delayMs);delayMs*=2}
        }
        require(sdk.uploadState(msgId)?.substringBefore(',')=="3"){
            "网络异常，上传草稿已保留，恢复网络/重启后自动续传"}
    }

    /** 列表小图：本地原图 > 小缩略图 > 大缩略图 > 原图。 */
    fun ensurePreview(message:JitongSdk.NativeMessage)=ensure(message,false)
    /** 点开：本地原图 > 原图下载 > 大缩略图 > 小缩略图。 */
    fun ensureOriginal(message:JitongSdk.NativeMessage)=ensure(message,true)

    private fun ensure(message:JitongSdk.NativeMessage,origin:Boolean) = scope.launch(Dispatchers.IO) {
        // 同一条消息的预览与原图请求串行，避免两个下载同时追加同一 .part。
        val thisJob=coroutineContext[Job]!!
        while(true){
            val previous=jobs.putIfAbsent(message.msgId,thisJob)
            if(previous==null)break
            previous.join();coroutineContext.ensureActive()
        }
        try {
        val existing=listOf(message.media.localPath,message.media.largeThumbnailPath,
            message.media.thumbnailPath).firstOrNull { it.isNotBlank()&&File(it).isFile }
        if(existing!=null&&(!origin||existing==message.media.localPath)){
            _local.update { it+(message.msgId to Local(existing,Status.Ready)) };return@launch
        }
        _local.update { it+(message.msgId to Local(existing.orEmpty(),Status.Working)) }
        val ids=if(origin) listOf(message.media.fileId,message.media.largeThumbnailFileId,message.media.thumbnailFileId)
            else listOf(message.media.thumbnailFileId,message.media.largeThumbnailFileId,message.media.fileId)
        val hashes=if(origin) listOf(message.media.sha256,message.media.largeThumbnailSha256,message.media.thumbnailSha256)
            else listOf(message.media.thumbnailSha256,message.media.largeThumbnailSha256,message.media.sha256)
        var failure="无可用媒体索引"
        ids.zip(hashes).filter { it.first.isNotBlank() }.forEach { (id,hash) ->
            val final=File(root,"${safeId(id)}.bin");val part=File(root,"${safeId(id)}.part")
            if(hash.isNotBlank()&&final.isFile&&verify(final,hash)){
                _local.update { it+(message.msgId to Local(final.absolutePath,Status.Ready)) };return@launch
            }
            // 崩溃可能发生在下载完成与 rename 之间；已完整的 .part 不再发 Range。
            if(hash.isNotBlank()&&part.isFile&&verify(part,hash)&&part.renameTo(final)){
                _local.update { it+(message.msgId to Local(final.absolutePath,Status.Ready)) };return@launch
            }
            val taskId=UUID.randomUUID().toString()
            val durable=hash.matches(Regex("[0-9a-fA-F]{64}"))
            val generation=if(durable)
                sdk.beginDownloadTask(taskId,message.msgId,id,part.absolutePath) else 0L
            if(durable&&generation<=0){failure="下载任务未能持久化";return@forEach}
            val operation=sdk.beginMediaOperation()
            if(operation<=0){
                if(generation>0)sdk.finishDownloadTask(taskId,generation,2,part.length())
                failure="Native 媒体任务不可用";return@forEach
            }
            if(generation>0)downloads[message.msgId]=DownloadLease(taskId,generation,part)
            operations[message.msgId]=operation
            val downloaded=try { sdk.downloadMedia(id,part.absolutePath,operation) }
                finally { operations.remove(message.msgId,operation);sdk.endMediaOperation(operation) }
            coroutineContext.ensureActive()
            if(downloaded&&verify(part,hash)&&part.renameTo(final)){
                if(generation>0&&!sdk.finishDownloadTask(taskId,generation,3,final.length())){
                    downloads.remove(message.msgId)
                    failure="下载已完成但任务终态提交失败";return@forEach
                }
                downloads.remove(message.msgId)
                _local.update { it+(message.msgId to Local(final.absolutePath,Status.Ready)) };return@launch
            }
            // 网络失败保留 .part 供 Range 续传；完成但摘要不符时清掉坏数据。
            if(downloaded)part.delete()
            if(generation>0)sdk.finishDownloadTask(taskId,generation,2,part.length())
            downloads.remove(message.msgId)
            failure="媒体下载或 SHA-256 校验失败"
        }
        _local.update { it+(message.msgId to Local(existing.orEmpty(),Status.Failed,failure)) }
        } catch(cancelled:CancellationException) {
            downloads.remove(message.msgId)?.let { lease ->
                sdk.finishDownloadTask(lease.taskId,lease.generation,4,lease.part.length())
            }
            _local.update { it+(message.msgId to Local(status=Status.Canceled)) }
            throw cancelled
        } catch(error:Exception) {
            downloads.remove(message.msgId)?.let { lease ->
                sdk.finishDownloadTask(lease.taskId,lease.generation,2,lease.part.length())
            }
            _local.update { it+(message.msgId to Local(status=Status.Failed,error=error.message)) }
        } finally { jobs.remove(message.msgId,thisJob) }
    }

    private suspend fun recoverDownloads() {
        var scheduled=0
        val recoveryJob=coroutineContext[Job]!!
        for(row in sdk.recoverableDownloads()) {
            if(jobs.putIfAbsent(row.msgId,recoveryJob)!=null)continue
            try {
            val part=File(row.localPath)
            val expected=File(root,"${safeId(row.fileId)}.part")
            if(!row.sha256.matches(Regex("[0-9a-fA-F]{64}"))||
                part.canonicalPath!=expected.canonicalPath){
                sdk.finishDownloadTask(row.taskId,row.generation,5,0);continue
            }
            val final=File(root,"${safeId(row.fileId)}.bin")
            if(final.isFile&&verify(final,row.sha256)){
                if(!sdk.finishDownloadTask(row.taskId,row.generation,3,final.length()))continue
                _local.update { it+(row.msgId to Local(final.absolutePath,Status.Ready)) }
                continue
            }
            if(part.isFile&&verify(part,row.sha256)&&part.renameTo(final)){
                if(!sdk.finishDownloadTask(row.taskId,row.generation,3,final.length()))continue
                _local.update { it+(row.msgId to Local(final.absolutePath,Status.Ready)) }
                continue
            }
            // 冷启动仅主动续传两个已有部分文件；其余任务由可见消息的 ensure 按需领取。
            if(!part.isFile||part.length()==0L){
                sdk.finishDownloadTask(row.taskId,row.generation,5,0);continue
            }
            if(scheduled++>=2)continue
            val taskId=UUID.randomUUID().toString()
            val generation=sdk.beginDownloadTask(taskId,row.msgId,row.fileId,part.absolutePath)
            if(generation<=0)continue
            val operation=sdk.beginMediaOperation()
            if(operation<=0){sdk.finishDownloadTask(taskId,generation,2,part.length());continue}
            downloads[row.msgId]=DownloadLease(taskId,generation,part)
            operations[row.msgId]=operation
            try {
                val ok=sdk.downloadMedia(row.fileId,part.absolutePath,operation)
                if(ok&&verify(part,row.sha256)&&part.renameTo(final)){
                    if(sdk.finishDownloadTask(taskId,generation,3,final.length()))
                        _local.update { it+(row.msgId to Local(final.absolutePath,Status.Ready)) }
                } else {
                    if(ok)part.delete()
                    sdk.finishDownloadTask(taskId,generation,2,part.length())
                }
            } catch(cancelled:CancellationException) {
                sdk.finishDownloadTask(taskId,generation,4,part.length())
                throw cancelled
            } catch(error:Exception) {
                sdk.finishDownloadTask(taskId,generation,2,part.length())
                _local.update { it+(row.msgId to Local(status=Status.Failed,error=error.message)) }
            } finally {
                operations.remove(row.msgId,operation);downloads.remove(row.msgId)
                sdk.endMediaOperation(operation)
            }
            } finally { jobs.remove(row.msgId,recoveryJob) }
        }
    }

    private fun write(name:String,bytes:ByteArray)=File(root,sanitize(name)).also { target ->
        val temp=File(root,".${target.name}.tmp");temp.outputStream().use { it.write(bytes);it.fd.sync() }
        require(temp.renameTo(target)){"local media rename failed"}
    }
    private fun query(uri:Uri):Pair<String,Long>{
        var name="file";var size=-1L
        app.contentResolver.query(uri,arrayOf(OpenableColumns.DISPLAY_NAME,OpenableColumns.SIZE),null,null,null)?.use {
            if(it.moveToFirst()){name=it.getString(0)?:name;size=if(it.isNull(1))-1 else it.getLong(1)}
        }
        if(size<0)size=app.contentResolver.openAssetFileDescriptor(uri,"r")?.use{it.length}?:-1
        return sanitize(name) to size
    }
    private fun verify(file:File,expected:String)=expected.isBlank()||sha256(file).equals(expected,true)
    private fun sha256(file:File):String{val d=MessageDigest.getInstance("SHA-256");FileInputStream(file).use{
        input->val b=ByteArray(64*1024);while(true){val n=input.read(b);if(n<0)break;d.update(b,0,n)}}
        return d.digest().joinToString(""){"%02x".format(it)}}
    private fun safeId(value:String)=MessageDigest.getInstance("SHA-256").digest(value.toByteArray())
        .take(16).joinToString(""){"%02x".format(it)}
    private fun sanitize(value:String)=value.substringAfterLast('/').substringAfterLast('\\')
        .replace(Regex("[^A-Za-z0-9._-]"),"_").take(120).ifBlank{"file"}
    private companion object { const val MAX_FILE=100L*1024*1024 }
}
