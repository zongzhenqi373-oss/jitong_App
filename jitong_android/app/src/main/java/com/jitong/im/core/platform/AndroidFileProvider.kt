package com.jitong.im.core.platform

import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.system.Os
import java.io.File
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.util.concurrent.ConcurrentHashMap

/**
 * [FileProvider] 的 Android 实现。
 *
 * 只提供 open / readRange / atomicRename 这类原子能力：
 * 「读哪一段」「是否分片」「怎么续传」由 C++ TransferManager 决定。
 */
class AndroidFileProvider(private val context: Context) : FileProvider {

    // PlatformFileHandle 跨越 JNI 调用存活，必须保留 PFD 所有权，不能只返回一个
    // 随时可能被原对象关闭的裸 fd。close(handle) 是唯一释放入口。
    private val descriptors = ConcurrentHashMap<Int, ParcelFileDescriptor>()

    private fun retain(pfd: ParcelFileDescriptor, localPath: String? = null): PlatformFileHandle {
        descriptors[pfd.fd] = pfd
        return PlatformFileHandle(fd = pfd.fd, localPath = localPath)
    }

    override fun openRead(uri: String): PlatformFileHandle? {
        // content:// 走 ContentResolver；file:// 或裸路径直接用 fd
        if (uri.startsWith("content://")) {
            return runCatching {
                val pfd = context.contentResolver.openFileDescriptor(Uri.parse(uri), "r")
                    ?: return null
                retain(pfd)
            }.getOrNull()
        }
        val path = uri.removePrefix("file://")
        val file = File(path)
        if (!file.exists()) return null
        return runCatching {
            val pfd = ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY)
            retain(pfd, file.absolutePath)
        }.getOrNull()
    }

    override fun openWrite(path: String): PlatformFileHandle? = runCatching {
        val file = File(path)
        file.parentFile?.mkdirs()
        val pfd = ParcelFileDescriptor.open(
            file,
            ParcelFileDescriptor.MODE_WRITE_ONLY or
                ParcelFileDescriptor.MODE_CREATE or
                ParcelFileDescriptor.MODE_TRUNCATE,
        )
        retain(pfd, file.absolutePath)
    }.getOrNull()

    /**
     * 读取指定区间。秒传 PoP 需要按服务端 challenge 读 [offset, offset+length)，
     * 因此必须支持随机访问，而不是只能顺序读。
     */
    override fun readRange(handle: PlatformFileHandle, offset: Long, length: Long): ByteArray? {
        if (length <= 0) return ByteArray(0)
        if (offset < 0 || length > Int.MAX_VALUE) return null
        return runCatching {
            val pfd = descriptors[handle.fd] ?: return null
            val buffer = ByteArray(length.toInt())
            var total = 0
            while (total < buffer.size) {
                val count = Os.pread(
                    pfd.fileDescriptor,
                    buffer,
                    total,
                    buffer.size - total,
                    offset + total,
                )
                if (count <= 0) break
                total += count
            }
            if (total == buffer.size) buffer else buffer.copyOf(total)
        }.getOrNull()
    }

    /** 临时文件与正式文件必须位于同一文件系统；不支持原子移动时明确失败。 */
    override fun atomicRename(from: String, to: String): Boolean {
        val src = File(from)
        val dst = File(to)
        dst.parentFile?.mkdirs()
        return runCatching {
            Files.move(
                src.toPath(),
                dst.toPath(),
                StandardCopyOption.ATOMIC_MOVE,
                StandardCopyOption.REPLACE_EXISTING,
            )
            true
        }.recoverCatching { error ->
            if (error is AtomicMoveNotSupportedException) false else throw error
        }.getOrDefault(false)
    }

    override fun close(handle: PlatformFileHandle) {
        descriptors.remove(handle.fd)?.close()
    }
}
