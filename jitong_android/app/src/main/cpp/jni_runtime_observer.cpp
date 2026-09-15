#include "jni_runtime_observer.h"
#include <android/log.h>

namespace jt {
namespace {
struct EnvScope {
    JavaVM* vm=nullptr;JNIEnv* env=nullptr;bool attached=false;
    explicit EnvScope(JavaVM* value):vm(value){
        if(!vm)return;
        if(vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)!=JNI_OK){
            if(vm->AttachCurrentThread(&env,nullptr)==JNI_OK)attached=true;else env=nullptr;
        }
    }
    ~EnvScope(){if(attached&&vm)vm->DetachCurrentThread();}
};
void clear(JNIEnv* env){if(env&&env->ExceptionCheck()){env->ExceptionDescribe();env->ExceptionClear();}}
}

JniRuntimeObserver::JniRuntimeObserver(JavaVM* vm,jobject sink):vm_(vm)
{
    EnvScope scope(vm_);if(!scope.env||!sink)return;
    sink_=scope.env->NewGlobalRef(sink);if(!sink_)return;
    jclass cls=scope.env->GetObjectClass(sink_);if(!cls)return;
    invalidated_=scope.env->GetMethodID(cls,"onInvalidated","(IJJJ)V");
    completed_=scope.env->GetMethodID(cls,"onOperationCompleted","(Ljava/lang/String;ILjava/lang/String;JJ)V");
    scope.env->DeleteLocalRef(cls);clear(scope.env);
    valid_=invalidated_&&completed_;
}

JniRuntimeObserver::~JniRuntimeObserver()
{
    EnvScope scope(vm_);if(scope.env&&sink_)scope.env->DeleteGlobalRef(sink_);sink_=nullptr;
}

void JniRuntimeObserver::onInvalidated(im::runtime::InvalidationBus::Domain domain,
    std::int64_t version,std::int64_t generation,std::int64_t ownerId)
{
    if(!valid_)return;EnvScope scope(vm_);if(!scope.env)return;
    scope.env->CallVoidMethod(sink_,invalidated_,static_cast<jint>(domain),static_cast<jlong>(version),
                              static_cast<jlong>(generation),static_cast<jlong>(ownerId));clear(scope.env);
}

void JniRuntimeObserver::onOperationCompleted(const im::runtime::CompletionRegistry::Record& record,
    std::int64_t generation,std::int64_t ownerId)
{
    if(!valid_)return;EnvScope scope(vm_);if(!scope.env)return;
    jstring id=scope.env->NewStringUTF(record.operationId.c_str());
    jstring error=scope.env->NewStringUTF(record.errorMessage.c_str());
    scope.env->CallVoidMethod(sink_,completed_,id,static_cast<jint>(record.result),error,
                              static_cast<jlong>(generation),static_cast<jlong>(ownerId));clear(scope.env);
    if(id)scope.env->DeleteLocalRef(id);if(error)scope.env->DeleteLocalRef(error);
}
}
