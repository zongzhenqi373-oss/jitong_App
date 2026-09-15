// P7-G3：账号级 Runtime 的 JNI 生命周期接口。
//
// 契约：
//   - Runtime 由 NativeSdkHandle 唯一持有；销毁在 destroyHandleLocked 中完成
//     （runtime → db → dbKeyBridge → core → observer）。
//   - 本文件只暴露**生命周期与状态**，不暴露 SQL、Socket、Token、DB key、
//     sqlite3* 或 C++ 裸对象地址——所有跨边界返回值都是基本类型或字符串。
//   - 取 runtime 指针时在 handle 锁内拷贝 shared_ptr，锁外调用，避免与 destroy 竞态。

#include <jni.h>

#include <memory>
#include <mutex>
#include <string>

#include "client_core/runtime/ClientRuntime.h"
#include "native_sdk_handle.h"
#include "jni_runtime_observer.h"

extern "C" JavaVM* jtGlobalJavaVm();

namespace {

std::string fromJString(JNIEnv* env,jstring value) {
    if(!value)return {}; const char* chars=env->GetStringUTFChars(value,nullptr);
    std::string out=chars?chars:""; if(chars)env->ReleaseStringUTFChars(value,chars); return out;
}

std::shared_ptr<im::runtime::ClientRuntime> acquireRuntime(jlong handle)
{
    auto h = jt::lookupHandle(handle);
    if (!h) return nullptr;
    std::lock_guard<std::mutex> lk(h->mutex);
    if (h->isDestroying()) return nullptr;
    return h->runtime;
}

} // namespace

extern "C" {

// boolean nativeCreateRuntime(long handle, int ownerId)
JNIEXPORT jboolean JNICALL
Java_com_jitong_im_core_NativeBindings_nativeCreateRuntime(JNIEnv*, jclass, jlong handle,
                                                           jint ownerId)
{
    auto h = jt::lookupHandle(handle);
    if (!h || h->isDestroying()) return JNI_FALSE;
    if (ownerId <= 0) return JNI_FALSE;

    im::runtime::ClientRuntime::Config cfg;
    cfg.ownerId = static_cast<std::int64_t>(ownerId);
    cfg.completionCapacity = 256;

    auto rt = std::make_shared<im::runtime::ClientRuntime>(cfg);
    {
        std::lock_guard<std::mutex> lk(h->mutex);
        if (h->isDestroying()) return JNI_FALSE;
        // Do not replace/destroy a live runtime while holding the handle mutex.
        // Account changes require a new handle and explicit teardown of the old one.
        if (h->runtime) {
            return h->runtime->ownerId() == ownerId &&
                   h->runtime->state() != im::runtime::ClientRuntime::State::Destroyed
                       ? JNI_TRUE : JNI_FALSE;
        }
        // 复用句柄唯一 ClientCore（唯一 Socket/安全通道），不在 Runtime 创建第二条连接。
        if (!h->core || !rt->attachClientCore(h->core)) return JNI_FALSE;
        // 若账号库已打开则共享所有权（NativeDatabase::close 幂等，故销毁顺序不敏感）
        if (h->db && !rt->setDatabase(h->db)) return JNI_FALSE;
        h->runtime = std::move(rt);
    }
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_jitong_im_core_NativeBindings_nativeSetRuntimeEventSink(JNIEnv*,jclass,jlong handle,
                                                                  jobject sink)
{
    if(!sink)return JNI_FALSE;
    auto rt=acquireRuntime(handle);if(!rt)return JNI_FALSE;
    auto observer=std::make_shared<jt::JniRuntimeObserver>(jtGlobalJavaVm(),sink);
    if(!observer->valid())return JNI_FALSE;
    rt->setRuntimeEventSink(observer);return JNI_TRUE;
}

// long nativeStartRuntime(long handle)
JNIEXPORT jlong JNICALL
Java_com_jitong_im_core_NativeBindings_nativeStartRuntime(JNIEnv*, jclass, jlong handle)
{
    auto rt = acquireRuntime(handle);
    if (!rt) return 0;
    std::string err;
    if (!rt->start(&err)) return 0;
    return static_cast<jlong>(rt->generation());
}

// void nativeStopRuntime(long handle)
JNIEXPORT void JNICALL
Java_com_jitong_im_core_NativeBindings_nativeStopRuntime(JNIEnv*, jclass, jlong handle)
{
    if (auto rt = acquireRuntime(handle)) rt->stop();
}

// void nativeLogoutRuntime(long handle)
JNIEXPORT void JNICALL
Java_com_jitong_im_core_NativeBindings_nativeLogoutRuntime(JNIEnv*, jclass, jlong handle)
{
    if (auto rt = acquireRuntime(handle)) rt->logout();
}

// String nativeGetRuntimeState(long handle)
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeGetRuntimeState(JNIEnv* env, jclass, jlong handle)
{
    auto rt = acquireRuntime(handle);
    const char* name = rt ? rt->stateName() : "None";
    return env->NewStringUTF(name);
}

// String nativeRuntimeSendText(handle, conversationId, peerId, text, pinyin, initials)
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRuntimeSendText(JNIEnv* env,jclass,jlong handle,
    jlong conversationId,jlong peerId,jstring text,jstring pinyin,jstring initials)
{
    auto rt=acquireRuntime(handle);
    if(!rt)return env->NewStringUTF("err|runtime_unavailable");
    const auto result=rt->sendText(conversationId,peerId,fromJString(env,text),
                                   fromJString(env,pinyin),fromJString(env,initials));
    const std::string encoded=result.accepted
        ? "ok|"+result.operationId+"|"+result.msgId+"|"+std::to_string(result.localOrder)
        : "err|"+result.operationId+"|"+result.msgId+"|"+result.error;
    return env->NewStringUTF(encoded.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRuntimeMarkRead(JNIEnv* env,jclass,jlong handle,
                                                              jlong conversationId,jlong readSeq)
{
    auto rt=acquireRuntime(handle);if(!rt)return env->NewStringUTF("");
    const auto operation=rt->markRead(conversationId,readSeq);
    return env->NewStringUTF(operation.c_str());
}

JNIEXPORT jint JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRuntimeFlushOutbox(JNIEnv*,jclass,jlong handle,
                                                                 jlong nowSeconds,jint limit)
{
    auto rt=acquireRuntime(handle); return rt?rt->flushOutbox(nowSeconds,limit):0;
}

// long nativeRuntimeConsumeInvalidation(handle, domain)
JNIEXPORT jlong JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRuntimeConsumeInvalidation(JNIEnv*,jclass,
                                                                         jlong handle,jint domain)
{
    auto rt=acquireRuntime(handle);
    if(!rt||domain<0||domain>4)return 0;
    return static_cast<jlong>(rt->consumeInvalidation(
        static_cast<im::runtime::InvalidationBus::Domain>(domain)));
}

// String nativeRuntimeConsumeOperation(handle, operationId)
// "pending" means that the operation has no terminal record yet.  The error is
// the final field, so Kotlin can split with limit=3 without corrupting messages
// that happen to contain '|'.
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRuntimeConsumeOperation(JNIEnv* env,jclass,
    jlong handle,jstring operationId)
{
    auto rt=acquireRuntime(handle);
    if(!rt)return env->NewStringUTF("unavailable");
    im::runtime::CompletionRegistry::Record record;
    if(!rt->consumeOperation(fromJString(env,operationId),&record))
        return env->NewStringUTF("pending");
    const std::string encoded="done|"+std::to_string(static_cast<int>(record.result))+"|"+
                              record.errorMessage;
    return env->NewStringUTF(encoded.c_str());
}

JNIEXPORT jboolean JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRuntimeRequestRoamConversations(JNIEnv*,jclass,
                                                                              jlong handle)
{
    auto rt=acquireRuntime(handle);
    return rt&&rt->requestRoamConversations()?JNI_TRUE:JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRuntimeRequestRoamMessages(JNIEnv*,jclass,jlong handle,
    jlong peerId,jlong beforeSeq,jint limit)
{
    auto rt=acquireRuntime(handle);
    return rt&&rt->requestRoamMessages(peerId,beforeSeq,limit)?JNI_TRUE:JNI_FALSE;
}

} // extern "C"
