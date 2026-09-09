#include "jni_account_observer.h"

#include <android/log.h>

namespace jt {

namespace {
constexpr const char* kSinkClass = "com/jitong/im/core/NativeAccountSink";
// void onAccountEvent(int type, long operationId, int accountState, int connectionState,
//                     int error, int userId, String message)
constexpr const char* kMethod = "onAccountEvent";
constexpr const char* kSig = "(IJIIIILjava/lang/String;)V";
} // namespace

JniAccountObserver::JniAccountObserver(JavaVM* vm, jobject sink) : vm_(vm)
{
    JNIEnv* env = nullptr;
    if (!vm_ || vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || !env) {
        return;
    }
    sink_ = env->NewGlobalRef(sink);
    if (!sink_) return;
    jclass cls = env->GetObjectClass(sink_);
    if (!cls) return;
    m_onAccountEvent = env->GetMethodID(cls, kMethod, kSig);
    env->DeleteLocalRef(cls);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    valid_ = (m_onAccountEvent != nullptr);
}

JniAccountObserver::~JniAccountObserver()
{
    if (!vm_ || !sink_) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (vm_->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    }
    if (env) env->DeleteGlobalRef(sink_);
    sink_ = nullptr;
    if (attached) vm_->DetachCurrentThread();
}

void JniAccountObserver::emit(const im::account::AccountEvent& e)
{
    if (!valid_ || !vm_) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (vm_->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    }
    if (env) {
        jstring msg = env->NewStringUTF(e.message.c_str());
        env->CallVoidMethod(sink_, m_onAccountEvent, static_cast<jint>(e.type),
                            static_cast<jlong>(e.operationId),
                            static_cast<jint>(e.accountState),
                            static_cast<jint>(e.connectionState), static_cast<jint>(e.error),
                            static_cast<jint>(e.userId), msg);
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
        if (msg) env->DeleteLocalRef(msg);
    }
    if (attached) vm_->DetachCurrentThread();
}

} // namespace jt
