// 认证会话 JNI（P5-T06）。
//
// 暴露"瘦 UI / 厚内核"的账号认证入口：UI 只表达意图（setup/密码登录/token 登录/登出/取消），
// 所有连接/握手/设备签名/Token 判断/刷新/重连/被踢处理都在 C++ 的 AccountSession 里编排。
// 账号事件通过 JniAccountObserver 拍平成 onAccountEvent 回调投递到 Kotlin。
//
// 复用 jt::NativeSdkHandle（同一 id 空间）：认证会话组件挂在句柄上，随句柄销毁按依赖逆序释放。

#include <jni.h>

#include <map>
#include <memory>
#include <string>

#include "native_sdk_handle.h"
#include "jni_account_observer.h"

#include "client_core/AccountSession.h"
#include "client_core/ClientCoreAuthTransport.h"

extern "C" JavaVM* jtGlobalJavaVm(); // 定义于 im_core_jni.cpp（g_vm 为内部链接，经此访问）

namespace {

std::string jstr(JNIEnv* env, jstring s)
{
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return r;
}

} // namespace

extern "C" {

// long nativeAccountSetup(long handle, String serverIp, int port, Object accountSink)
JNIEXPORT jboolean JNICALL
Java_com_jitong_im_core_NativeBindings_nativeAccountSetup(JNIEnv* env, jclass, jlong handle,
                                                          jstring serverIp, jint port,
                                                          jobject accountSink, jobject platformBridge)
{
    auto h = jt::lookupHandle(handle);
    if (!h) return JNI_FALSE;
    std::lock_guard<std::mutex> lk(h->mutex);
    if (h->isDestroying() || !h->core) return JNI_FALSE;
    if (h->account) return JNI_TRUE; // 已建立

    const std::string ip = jstr(env, serverIp);

    auto observer = std::make_shared<jt::JniAccountObserver>(jtGlobalJavaVm(), accountSink);
    if (!observer->valid()) return JNI_FALSE;
    auto platform = std::make_shared<jt::JniAuthPlatform>(jtGlobalJavaVm(), env, platformBridge);
    if (!platform->valid()) return JNI_FALSE;
    const std::string deviceId = platform->deviceId();
    if (deviceId.empty() || platform->publicKey().empty()) return JNI_FALSE;

    h->clock = std::make_shared<im::account::SystemClock>();
    h->authPlatform = platform;
    h->tokenStore = std::make_shared<jt::JniTokenStore>(platform);
    h->signer = std::make_shared<jt::JniP256Signer>(platform);
    h->authTransport = std::make_shared<im::account::ClientCoreAuthTransport>(
        *h->core, ip, static_cast<std::uint16_t>(port));

    h->account = std::make_shared<im::account::AccountSession>(
        *h->authTransport, h->clock, h->tokenStore, *h->signer, deviceId);
    h->authTransport->setSession(h->account);

    auto obs = observer;
    h->account->setEventSink([obs](const im::account::AccountEvent& e) { obs->emit(e); });
    h->accountObserver = observer; // 持有，随句柄销毁释放
    return JNI_TRUE;
}

// long nativeAccountLoginWithPassword(long handle, String account, String password)
JNIEXPORT jlong JNICALL
Java_com_jitong_im_core_NativeBindings_nativeAccountLoginWithPassword(JNIEnv* env, jclass,
                                                                      jlong handle, jstring account,
                                                                      jstring password)
{
    auto h = jt::lookupHandle(handle);
    if (!h) return 0;
    std::shared_ptr<im::account::AccountSession> session;
    {
        std::lock_guard<std::mutex> lk(h->mutex);
        if (h->isDestroying() || !h->account) return 0;
        session = h->account;
    }
    return static_cast<jlong>(session->startWithPassword(jstr(env, account), jstr(env, password)));
}

// long nativeAccountStartWithSavedToken(long handle, String account)
JNIEXPORT jlong JNICALL
Java_com_jitong_im_core_NativeBindings_nativeAccountStartWithSavedToken(JNIEnv* env, jclass,
                                                                        jlong handle,
                                                                        jstring account)
{
    auto h = jt::lookupHandle(handle);
    if (!h) return 0;
    std::shared_ptr<im::account::AccountSession> session;
    {
        std::lock_guard<std::mutex> lk(h->mutex);
        if (h->isDestroying() || !h->account) return 0;
        session = h->account;
    }
    return static_cast<jlong>(session->startWithSavedToken(jstr(env, account)));
}

// void nativeAccountLogout(long handle, boolean allDevices)
JNIEXPORT void JNICALL
Java_com_jitong_im_core_NativeBindings_nativeAccountLogout(JNIEnv*, jclass, jlong handle,
                                                           jboolean allDevices)
{
    auto h = jt::lookupHandle(handle);
    if (!h) return;
    std::shared_ptr<im::account::AccountSession> session;
    {
        std::lock_guard<std::mutex> lk(h->mutex);
        if (h->isDestroying() || !h->account) return;
        session = h->account;
    }
    session->logout(allDevices == JNI_TRUE);
}

// void nativeAccountCancel(long handle)
JNIEXPORT void JNICALL
Java_com_jitong_im_core_NativeBindings_nativeAccountCancel(JNIEnv*, jclass, jlong handle)
{
    auto h = jt::lookupHandle(handle);
    if (!h) return;
    std::shared_ptr<im::account::AccountSession> session;
    {
        std::lock_guard<std::mutex> lk(h->mutex);
        if (h->isDestroying() || !h->account) return;
        session = h->account;
    }
    session->cancel();
}

// int nativeAccountGetState(long handle) —— 返回 AccountState 序号，-1 表示无会话
JNIEXPORT jint JNICALL
Java_com_jitong_im_core_NativeBindings_nativeAccountGetState(JNIEnv*, jclass, jlong handle)
{
    auto h = jt::lookupHandle(handle);
    if (!h) return -1;
    std::shared_ptr<im::account::AccountSession> session;
    {
        std::lock_guard<std::mutex> lk(h->mutex);
        if (h->isDestroying() || !h->account) return -1;
        session = h->account;
    }
    return static_cast<jint>(session->accountState());
}

} // extern "C"
