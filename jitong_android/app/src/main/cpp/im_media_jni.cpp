// 分片上传草稿 JNI（P7 媒体断点续传 Android 接线）。
//
// 线程模型：所有函数都是阻塞式（含网络 IO），Kotlin 必须从 Dispatchers.IO 调用。
// 生命周期：MediaService 懒创建并挂在句柄上，随句柄销毁；销毁期间新调用被拒绝。

#include <jni.h>

#include <fstream>
#include <string>
#include <vector>

#include <android/log.h>

#include "native_sdk_handle.h"
#include "client_core/media/ClientCoreUploadTransport.h"
#include "client_core/dto/Dtos.h"

#include <openssl/evp.h>

namespace {

constexpr const char* kTag = "JitongKernelMedia";

std::string jstrToUtf8(JNIEnv* env, jstring s)
{
    if (!s) return {};
    const jsize len = env->GetStringUTFLength(s);
    const char* chars = env->GetStringUTFChars(s, nullptr);
    std::string out(chars, len);
    env->ReleaseStringUTFChars(s, chars);
    return out;
}

jstring toJ(JNIEnv* env, const std::string& s)
{
    return env->NewStringUTF(s.c_str());
}

// 懒创建 MediaService：core + db 就绪后绑定（同一 NativeDatabase，独立仓储实例）。
// 返回 nullptr 表示未就绪或句柄销毁中。调用方持 shared_ptr 使用，锁外安全。
std::shared_ptr<im::media::MediaService> ensureMediaService(
    const std::shared_ptr<jt::NativeSdkHandle>& h)
{
    std::lock_guard<std::mutex> lk(h->mutex);
    if (h->isDestroying() || !h->core || !h->db || h->dbOwnerId <= 0) return nullptr;
    if (!h->mediaService) {
#if defined(CLIENT_CORE_WITH_MEDIA)
        h->uploadTransport = std::make_shared<im::media::ClientCoreUploadTransport>(*h->core);
        h->mediaRepo = std::make_shared<im::storage::NativeRepository>(h->db.get());
        h->mediaService = std::make_shared<im::media::MediaService>(
            h->dbOwnerId, *h->mediaRepo, *h->uploadTransport);
#else
        return nullptr;
#endif
    }
    return h->mediaService;
}

void wakeOutbox(const std::shared_ptr<jt::NativeSdkHandle>& h)
{
    std::shared_ptr<im::runtime::ClientRuntime> runtime;
    {
        std::lock_guard<std::mutex> lk(h->mutex);
        if (h->isDestroying()) return;
        runtime = h->runtime;
    }
    if (runtime) runtime->flushOutbox(
        static_cast<std::int64_t>(std::time(nullptr)), 32);
}

// 流式计算文件大小与整体 SHA-256（不整体入内存）
bool statFile(const std::string& path, std::int64_t& size, std::string& sha)
{
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) return false;
    const auto end = ifs.tellg();
    if (end <= 0) return false;
    size = static_cast<std::int64_t>(end);
    ifs.seekg(0, std::ios::beg);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return false;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) { EVP_MD_CTX_free(ctx); return false; }
    std::vector<char> buf(64 * 1024);
    while (ifs) {
        ifs.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        if (ifs.gcount() > 0)
            EVP_DigestUpdate(ctx, buf.data(), static_cast<std::size_t>(ifs.gcount()));
    }
    if (ifs.bad()) { EVP_MD_CTX_free(ctx); return false; }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;
    const bool ok = EVP_DigestFinal_ex(ctx, digest, &digestLen) == 1 && digestLen == 32;
    EVP_MD_CTX_free(ctx);
    if (!ok) return false;
    static constexpr char hex[] = "0123456789abcdef";
    sha.clear();
    sha.reserve(64);
    for (unsigned int i = 0; i < digestLen; ++i) { sha += hex[digest[i] >> 4]; sha += hex[digest[i] & 15]; }
    return true;
}

} // namespace

extern "C" {

// String nativeMediaEnqueueUpload(handle, msgId, conversationId, peerId, variant,
//                                 localPath, fileName, contentType)
// 幂等：同 msgId+variant 重复登记保留进度。返回 "ok" / "err|<原因>"。
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeMediaEnqueueUpload(JNIEnv* env, jclass,
    jlong handle, jstring msgId, jlong conversationId, jlong peerId, jint variant,
    jstring localPath, jstring fileName, jstring contentType, jint imageWidth, jint imageHeight)
{
    auto h = jt::lookupHandle(handle);
    if (!h) return toJ(env, "err|句柄无效");
    auto svc = ensureMediaService(h);
    if (!svc) return toJ(env, "err|内核未就绪");

    im::dto::UploadDraftDto d;
    d.msgId = jstrToUtf8(env, msgId);
    d.variant = static_cast<im::dto::MediaVariant>(variant);
    d.conversationId = conversationId;
    d.peerId = peerId;
    d.localPath = jstrToUtf8(env, localPath);
    d.fileName = jstrToUtf8(env, fileName);
    d.contentType = jstrToUtf8(env, contentType);
    d.imageWidth = imageWidth;
    d.imageHeight = imageHeight;
    if (d.msgId.empty() || d.conversationId <= 0 || d.peerId <= 0 || d.localPath.empty())
        return toJ(env, "err|参数非法");
    if (!statFile(d.localPath, d.fileSize, d.sha256))
        return toJ(env, "err|本地文件不可读");
    if (d.fileName.empty()) d.fileName = "file";

    std::string err;
    if (!svc->enqueue({d}, &err)) return toJ(env, "err|" + err);
    return toJ(env, "ok");
}

// String nativeMediaPumpUpload(handle, msgId)：推进该消息的上传直到 Sent/Failed。
// 阻塞（含网络 IO）；成功后唤醒 Outbox 泵立即发送消息卡片。
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeMediaPumpUpload(JNIEnv* env, jclass,
                                                             jlong handle, jstring msgId)
{
    auto h = jt::lookupHandle(handle);
    if (!h) return toJ(env, "err|句柄无效");
    auto svc = ensureMediaService(h);
    if (!svc) return toJ(env, "err|内核未就绪");
    std::string err;
    const bool ok = svc->pumpMessage(jstrToUtf8(env, msgId), &err);
    if (ok) wakeOutbox(h); // 卡片已入 Outbox，立即触发真实网络发送
    return toJ(env, ok ? "ok" : "err|" + err);
}

// int nativeMediaResumeUploads(handle)：启动/网络恢复时扫描恢复全部活跃草稿。
// 返回成功推进到终态的消息数（阻塞）。
JNIEXPORT jint JNICALL
Java_com_jitong_im_core_NativeBindings_nativeMediaResumeUploads(JNIEnv*, jclass, jlong handle)
{
    auto h = jt::lookupHandle(handle);
    if (!h) return 0;
    auto svc = ensureMediaService(h);
    if (!svc) return 0;
    std::string err;
    const auto n = svc->resumeAll(&err);
    if (n > 0) wakeOutbox(h);
    return static_cast<jint>(n);
}

// boolean nativeMediaCancelUpload(handle, msgId)：用户主动取消（终态，不自动恢复）。
JNIEXPORT jboolean JNICALL
Java_com_jitong_im_core_NativeBindings_nativeMediaCancelUpload(JNIEnv* env, jclass,
                                                               jlong handle, jstring msgId)
{
    auto h = jt::lookupHandle(handle);
    if (!h) return JNI_FALSE;
    auto svc = ensureMediaService(h);
    if (!svc) return JNI_FALSE;
    std::string err;
    return svc->cancel(jstrToUtf8(env, msgId), &err) ? JNI_TRUE : JNI_FALSE;
}

// String nativeMediaUploadState(handle, msgId)：
// 返回 "minState,doneChunks,totalChunks,originFileId"（多变体聚合）；无草稿返回空串。
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeMediaUploadState(JNIEnv* env, jclass,
                                                              jlong handle, jstring msgId)
{
    auto h = jt::lookupHandle(handle);
    if (!h) return toJ(env, "");
    std::shared_ptr<im::storage::NativeDatabase> db;
    std::int64_t ownerId = 0;
    {
        std::lock_guard<std::mutex> lk(h->mutex);
        if (h->isDestroying()) return toJ(env, "");
        db = h->db;
        ownerId = h->dbOwnerId;
    }
    if (!db || ownerId <= 0) return toJ(env, "");
    im::storage::NativeRepository repo(db.get());
    const std::string id = jstrToUtf8(env, msgId);

    int minState = 99;
    int done = 0;
    int total = 0;
    std::string fileId;
    bool found = false;
    for (auto v : {im::dto::MediaVariant::Origin, im::dto::MediaVariant::LargeThumbnail,
                   im::dto::MediaVariant::SmallThumbnail}) {
        im::dto::UploadDraftDto d;
        std::string e;
        if (!repo.getUploadDraft(ownerId, id, v, &d, &e)) continue;
        found = true;
        minState = std::min(minState, static_cast<int>(d.state));
        done += static_cast<int>(d.chunksDone.size());
        total += d.chunkCount;
        if (v == im::dto::MediaVariant::Origin) fileId = d.fileId;
    }
    if (!found) return toJ(env, "");
    return toJ(env, std::to_string(minState) + "," + std::to_string(done) + "," +
                    std::to_string(total) + "," + fileId);
}

} // extern "C"
