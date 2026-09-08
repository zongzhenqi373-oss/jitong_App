// Android JNI 绑定层
//
// 设计约束（来自执行规划「全局约束」）：
//   - 本文件只做类型转换、句柄生命周期与自检，不包含任何业务判断；
//   - 不打印 Token / 密码 / 密钥；
//   - C++ 异常不得穿过 JNI 边界，统一在函数入口 try/catch。
//
// P1：nativeVersion / nativeSelfTest（自检）
// P2：NativeSdkHandle 生命周期 + JniObserver 事件桥

#include <jni.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <openssl/opensslv.h>
#include <openssl/crypto.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/provider.h>
#endif

#include <google/protobuf/stubs/common.h>

#include <android/log.h>

// TcpTransport.h 位于 client_core/src/（非 public 头），由 CMake 显式加入 include 路径。
// 自检复用真实的 encodeLen32/decodeLen32，确保校验的就是链路实际使用的实现。
#include "TcpTransport.h"

#include "jni_observer.h"
#include "native_sdk_handle.h"

namespace {

// 由 build.gradle.kts 注入；未注入时用编译期兜底值，保证不为空
#ifndef JITONG_KERNEL_VERSION
#define JITONG_KERNEL_VERSION "0.1.0-dev"
#endif

constexpr const char* kKernelVersion = JITONG_KERNEL_VERSION;

// 进程内唯一的 JavaVM 指针。JNIEnv* 是线程私有的，不能跨线程保存；
// 需要回调 Java 时必须通过 JavaVM* 做 AttachCurrentThread（见 P2 JniObserver）。
JavaVM* g_vm = nullptr;

// 帧编解码自检：4 字节大端包长往返，确认与服务端/桌面端同序
bool selfTestFrameCodec()
{
    char buf[4] = {};
    const std::uint32_t kCases[] = {0u, 1u, 4u, 255u, 65535u, 10u * 1024u * 1024u, 0xDEADBEEFu};
    for (std::uint32_t v : kCases) {
        im::encodeLen32(v, buf);
        if (im::decodeLen32(buf) != v) return false;
        // 逐字节核对大端布局
        if (static_cast<unsigned char>(buf[0]) != ((v >> 24) & 0xFF)) return false;
        if (static_cast<unsigned char>(buf[3]) != (v & 0xFF)) return false;
    }
    return true;
}

// 端序自检：本机必须能正确处理大端长度字段（与字节序无关的手工编解码）
bool selfTestEndianness()
{
    const std::uint32_t one = 1u;
    const bool little = (*reinterpret_cast<const unsigned char*>(&one) == 1u);
    // 无论本机大小端，encodeLen32 都应产出大端；这里只确认不会因本机序产生歧义
    char buf[4] = {};
    im::encodeLen32(0x01020304u, buf);
    const bool bigEndianLayout =
        static_cast<unsigned char>(buf[0]) == 0x01 &&
        static_cast<unsigned char>(buf[1]) == 0x02 &&
        static_cast<unsigned char>(buf[2]) == 0x03 &&
        static_cast<unsigned char>(buf[3]) == 0x04;
    (void)little;
    return bigEndianLayout;
}

std::string buildSelfTestReport()
{
    std::string report;
    report += "kernel=";
    report += kKernelVersion;
    report += "\n";

    report += "protobuf=";
    report += google::protobuf::internal::VersionString(GOOGLE_PROTOBUF_VERSION);
    report += "\n";

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    report += "openssl=";
    report += OpenSSL_version(OPENSSL_VERSION_STRING);
    report += "\n";
#else
    report += "openssl=";
    report += OPENSSL_VERSION_TEXT;
    report += "\n";
#endif

    report += "abi=";
#if defined(__aarch64__)
    report += "arm64-v8a";
#elif defined(__x86_64__)
    report += "x86_64";
#elif defined(__i386__)
    report += "x86";
#elif defined(__arm__)
    report += "armeabi-v7a";
#else
    report += "unknown";
#endif
    report += "\n";

    report += "sizeof_long=";
    report += std::to_string(sizeof(long));
    report += "\n";

    report += "frame_codec=";
    report += selfTestFrameCodec() ? "ok" : "FAIL";
    report += "\n";

    report += "endianness=";
    report += selfTestEndianness() ? "ok" : "FAIL";
    report += "\n";

    const bool allOk = selfTestFrameCodec() && selfTestEndianness();
    report += "result=";
    report += allOk ? "ok" : "FAIL";

    return report;
}

} // namespace

extern "C" {

// 库加载时由 ART 回调；保存 JavaVM* 供后续 AttachCurrentThread 使用
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/)
{
    g_vm = vm;
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* /*vm*/, void* /*reserved*/)
{
    g_vm = nullptr;
}

// Kotlin: com.jitong.im.core.NativeBindings.nativeVersion(): String
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeVersion(JNIEnv* env, jclass /*clazz*/)
{
    return env->NewStringUTF(kKernelVersion);
}

// Kotlin: com.jitong.im.core.NativeBindings.nativeSelfTest(): String
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeSelfTest(JNIEnv* env, jclass /*clazz*/)
{
    std::string report;
    try {
        report = buildSelfTestReport();
    } catch (const std::exception& e) {
        // C++ 异常绝不能穿过 JNI 边界
        report = std::string("result=FAIL\nexception=") + e.what();
    } catch (...) {
        report = "result=FAIL\nexception=unknown";
    }
    return env->NewStringUTF(report.c_str());
}

// ---------------- P2：句柄生命周期 ----------------

namespace {

/** 在锁内为句柄挂上事件桥；句柄已销毁时返回 false。 */
bool attachObserver(jlong handleId, jobject sink)
{
    auto h = jt::lookupHandle(handleId);
    if (!h || h->isDestroying() || !sink) return false;

    auto replacement = std::make_shared<jt::JniObserver>(g_vm, sink);
    if (!replacement->valid()) return false;

    std::lock_guard<std::mutex> lk(h->mutex);
    if (h->destroying.load(std::memory_order_acquire)) return false;
    // 事件接收器与 SDK 生命周期绑定。禁止运行中替换，避免 ClientCore 的非拥有
    // IClientEvents* 与旧 observer 析构之间出现悬空窗口。
    if (h->observer) return false;
    h->observer = std::move(replacement);
    if (h->core) h->core->setEventSink(h->observer.get());
    return true;
}

} // namespace

// Kotlin: NativeBindings.nativeCreate(serverName: String?): Long
JNIEXPORT jlong JNICALL
Java_com_jitong_im_core_NativeBindings_nativeCreate(JNIEnv* env, jclass /*clazz*/,
                                                    jstring serverName)
{
    std::string name;
    if (serverName != nullptr) {
        const char* s = env->GetStringUTFChars(serverName, nullptr);
        if (s != nullptr) {
            name = s;
            env->ReleaseStringUTFChars(serverName, s);
        }
    }
    try {
        const jlong id = jt::createHandle(name);
        __android_log_print(ANDROID_LOG_INFO, "JitongKernel", "nativeCreate('%s') -> %lld",
                            name.c_str(), static_cast<long long>(id));
        return id;
    } catch (...) {
        __android_log_print(ANDROID_LOG_ERROR, "JitongKernel", "nativeCreate threw");
        return 0; // 0 保留为无效句柄
    }
}

// Kotlin: NativeBindings.nativeDestroy(handle: Long)
JNIEXPORT void JNICALL
Java_com_jitong_im_core_NativeBindings_nativeDestroy(JNIEnv* /*env*/, jclass /*clazz*/,
                                                     jlong handle)
{
    // 幂等：重复调用 / 并发调用都安全
    jt::releaseHandle(handle);
}

// Kotlin: NativeBindings.nativeSetEventSink(handle: Long, sink: NativeEventSink): Boolean
JNIEXPORT jboolean JNICALL
Java_com_jitong_im_core_NativeBindings_nativeSetEventSink(JNIEnv* /*env*/, jclass /*clazz*/,
                                                          jlong handle, jobject sink)
{
    return attachObserver(handle, sink) ? JNI_TRUE : JNI_FALSE;
}

// Kotlin: NativeBindings.nativeHandleCount(): Int
JNIEXPORT jint JNICALL
Java_com_jitong_im_core_NativeBindings_nativeHandleCount(JNIEnv* /*env*/, jclass /*clazz*/)
{
    return static_cast<jint>(jt::liveHandleCount());
}

/**
 * 生命周期压力测试（P2-T04）。
 *
 * 覆盖：
 *   1. 创建/销毁 N 次，结束后存活句柄数必须为 0；
 *   2. 重复销毁必须幂等（第二次返回 false）；
 *   3. 销毁后仍用旧句柄调用 API 必须返回「无效」而不是崩溃；
 *   4. 后台线程持续触发回调的同时销毁句柄（验证无 UAF / double free）。
 *
 * @param iterations 创建销毁轮次
 * @param sink       事件接收对象；为 null 时跳过并发回调部分
 */
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeLifecycleStressTest(JNIEnv* env, jclass /*clazz*/,
                                                                 jint iterations, jobject sink)
{
    std::string report;
    int failures = 0;

    try {
        const int rounds = iterations > 0 ? iterations : 100;
        // 基线句柄数：本轮只允许「净增 0」
        const std::size_t baseline = jt::liveHandleCount();

        // 1) 创建 / 销毁 N 次
        for (int i = 0; i < rounds; ++i) {
            const jlong id = jt::createHandle("stress.example");
            if (id == 0) { ++failures; break; }
            if (!jt::releaseHandle(id)) ++failures;
        }

        // 2) 幂等：同一句柄释放两次
        {
            const jlong id = jt::createHandle("idem.example");
            const bool first = jt::releaseHandle(id);
            const bool second = jt::releaseHandle(id);
            if (!first || second) ++failures;
        }

        // 3) 野句柄：销毁后再次使用
        {
            const jlong id = jt::createHandle("stale.example");
            jt::releaseHandle(id);
            if (jt::lookupHandle(id) != nullptr) ++failures;
            if (jt::releaseHandle(id)) ++failures;
        }

        // 4) 回调与销毁并发
        int callbackCount = 0;
        if (sink != nullptr) {
            const jlong id = jt::createHandle("concurrent.example");
            auto h = jt::lookupHandle(id);
            if (h && attachObserver(id, sink)) {
                std::atomic<bool> stop{false};
                std::atomic<int> counter{0};
                std::vector<std::thread> workers;
                for (int t = 0; t < 4; ++t) {
                    workers.emplace_back([h, &stop, &counter]() {
                        while (!stop.load(std::memory_order_acquire)) {
                            std::shared_ptr<jt::JniObserver> obs;
                            {
                                std::lock_guard<std::mutex> lk(h->mutex);
                                if (h->destroying.load(std::memory_order_acquire)) break;
                                obs = h->observer; // 取副本，保证本次回调期间存活
                            }
                            if (!obs) break;
                            obs->onLoginResult(0, 4242);
                            obs->onConnectionClosed();
                            counter.fetch_add(1, std::memory_order_relaxed);
                        }
                    });
                }
                // 故意在回调飞行的过程中销毁
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                jt::releaseHandle(id);
                stop.store(true, std::memory_order_release);
                for (auto& w : workers) w.join();
                callbackCount = counter.load();
            } else {
                ++failures;
            }
        }

        // 断言「本轮新增的句柄全部释放」，而不是全局为 0：
        // 调用方（例如已 start 的 JitongSdk）完全可能持有自己的句柄，
        // 用绝对 0 做断言会把它误判成泄漏。
        const std::size_t live = jt::liveHandleCount();
        const std::size_t leaked = live > baseline ? live - baseline : 0;
        if (leaked != 0) ++failures;

        report += "rounds=";
        report += std::to_string(rounds);
        report += "\ncallbacks=";
        report += std::to_string(callbackCount);
        report += "\nbaseline_handles=";
        report += std::to_string(baseline);
        report += "\nlive_handles=";
        report += std::to_string(live);
        report += "\nleaked=";
        report += std::to_string(leaked);
        report += "\nfailures=";
        report += std::to_string(failures);
        report += "\nresult=";
        report += (failures == 0 && leaked == 0) ? "ok" : "FAIL";
    } catch (const std::exception& e) {
        report = std::string("result=FAIL\nexception=") + e.what();
    } catch (...) {
        report = "result=FAIL\nexception=unknown";
    }

    return env->NewStringUTF(report.c_str());
}

JNIEXPORT jboolean JNICALL
Java_com_jitong_im_core_NativeBindings_nativeEmitUtf8Test(JNIEnv* /*env*/, jclass /*clazz*/,
                                                         jlong handle)
{
    auto h = jt::lookupHandle(handle);
    if (!h || h->isDestroying()) return JNI_FALSE;
    std::shared_ptr<jt::JniObserver> observer;
    {
        std::lock_guard<std::mutex> lk(h->mutex);
        if (h->destroying.load(std::memory_order_acquire)) return JNI_FALSE;
        observer = h->observer;
    }
    if (!observer) return JNI_FALSE;

    // 标准 UTF-8：中文“你好” + U+1F642 + U+10437，用来覆盖 JNI Modified UTF-8
    // 不兼容的四字节字符路径。
    const std::string text =
        std::string("\xE4\xBD\xA0\xE5\xA5\xBD") +
        std::string("\xF0\x9F\x99\x82") +
        std::string("\xF0\x90\x90\xB7");
    observer->onChatMessage(7, text);
    return JNI_TRUE;
}

} // extern "C"
