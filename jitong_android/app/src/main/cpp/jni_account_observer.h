#pragma once
// 账号事件桥（P5-T06）：把 C++ AccountEvent 投递到 Kotlin。
//
// 与 JniObserver 同样的约束：缓存 JavaVM*/methodID、正确管理 GlobalRef、按需 Attach/Detach、
// 每次调用后清理 JNI 异常、只投递事件不做业务判断。为降低 JNI 边界复杂度，AccountEvent 被
// 拍平成一次 onAccountEvent(int,long,int,int,int,int,String) 调用。

#include <jni.h>

#include "client_core/AccountTypes.h"

namespace jt {

class JniAccountObserver {
public:
    JniAccountObserver(JavaVM* vm, jobject sink);
    ~JniAccountObserver();

    JniAccountObserver(const JniAccountObserver&) = delete;
    JniAccountObserver& operator=(const JniAccountObserver&) = delete;

    bool valid() const { return valid_; }

    // 供 AccountSession 的 EventSink 调用（可能在任意线程）。
    void emit(const im::account::AccountEvent& e);

private:
    JavaVM* vm_ = nullptr;
    jobject sink_ = nullptr; // GlobalRef
    bool valid_ = false;
    jmethodID m_onAccountEvent = nullptr;
};

} // namespace jt
