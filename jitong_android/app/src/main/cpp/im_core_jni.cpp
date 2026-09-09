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
#include <openssl/evp.h>
#include <openssl/x509.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/provider.h>
#endif

#include <google/protobuf/stubs/common.h>

#include <android/log.h>

// TcpTransport.h 位于 client_core/src/（非 public 头），由 CMake 显式加入 include 路径。
// 自检复用 FrameCodec（全工程唯一的线格式实现），确保校验的就是链路实际使用的代码。
#include "transport/FrameCodec.h"
#include "transport/ClientSecureChannel.h"
#include "transport/DeviceProof.h"

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
        im::transport::FrameCodec::encodeLength(v, buf);
        if (im::transport::FrameCodec::decodeLength(buf) != v) return false;
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
    // 无论本机大小端，encodeLength 都应产出大端；这里只确认不会因本机序产生歧义
    char buf[4] = {};
    im::transport::FrameCodec::encodeLength(0x01020304u, buf);
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

// P5-T06：为其它 TU（im_auth_jni.cpp）提供进程 JavaVM* 的外部链接访问器。
// g_vm 在匿名 namespace 内（内部链接），跨 TU 需通过本函数取得。
JavaVM* jtGlobalJavaVm()
{
    return g_vm;
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

// Kotlin: NativeBindings.nativeCreateConfigured(serverName, keyId, publicKeyBase64): Long
JNIEXPORT jlong JNICALL
Java_com_jitong_im_core_NativeBindings_nativeCreateConfigured(JNIEnv* env, jclass /*clazz*/,
                                                              jstring serverName, jint keyId,
                                                              jstring publicKeyBase64)
{
    std::string name;
    if (serverName != nullptr) {
        const char* s = env->GetStringUTFChars(serverName, nullptr);
        if (s != nullptr) {
            name = s;
            env->ReleaseStringUTFChars(serverName, s);
        }
    }
    std::string identityKey;
    if (publicKeyBase64 != nullptr) {
        const char* key = env->GetStringUTFChars(publicKeyBase64, nullptr);
        if (key != nullptr) {
            identityKey = key;
            env->ReleaseStringUTFChars(publicKeyBase64, key);
        }
    }
    try {
        if (keyId <= 0 || identityKey.empty()) return 0;
        const jlong id = jt::createHandle(
            name, {{static_cast<std::uint32_t>(keyId), identityKey}});
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
            const jlong id = jt::createHandle("stress.example", {});
            if (id == 0) { ++failures; break; }
            if (!jt::releaseHandle(id)) ++failures;
        }

        // 2) 幂等：同一句柄释放两次
        {
            const jlong id = jt::createHandle("idem.example", {});
            const bool first = jt::releaseHandle(id);
            const bool second = jt::releaseHandle(id);
            if (!first || second) ++failures;
        }

        // 3) 野句柄：销毁后再次使用
        {
            const jlong id = jt::createHandle("stale.example", {});
            jt::releaseHandle(id);
            if (jt::lookupHandle(id) != nullptr) ++failures;
            if (jt::releaseHandle(id)) ++failures;
        }

        // 4) 回调与销毁并发
        int callbackCount = 0;
        if (sink != nullptr) {
            const jlong id = jt::createHandle("concurrent.example", {});
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

// ---------------- P4：应用层安全通道 / 设备证明 进程内自检 ----------------

// Kotlin: NativeBindings.nativeSecureChannelHandshakeTest(): String
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeSecureChannelHandshakeTest(JNIEnv* env, jclass /*clazz*/)
{
    std::string report;
    report += "abi=";
#if defined(__aarch64__)
    report += "arm64-v8a\n";
#elif defined(__x86_64__)
    report += "x86_64\n";
#else
    report += "other\n";
#endif
    try {
        std::string diag;
        const bool ok = im::transport::ClientSecureChannel::runLoopbackHandshakeSelfTest(&diag);
        report += diag;
        if (report.find("result=") == std::string::npos) {
            report += ok ? "result=ok\n" : "result=FAIL\n";
        }
    } catch (const std::exception& e) {
        report += std::string("result=FAIL\nexception=") + e.what() + "\n";
    } catch (...) {
        report += "result=FAIL\nexception=unknown\n";
    }
    return env->NewStringUTF(report.c_str());
}

// Kotlin: NativeBindings.nativeDeviceProofSelfTest(): String
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeDeviceProofSelfTest(JNIEnv* env, jclass /*clazz*/)
{
    std::string report;
    report += "abi=";
#if defined(__aarch64__)
    report += "arm64-v8a\n";
#elif defined(__x86_64__)
    report += "x86_64\n";
#else
    report += "other\n";
#endif
    bool ok = false;
    try {
        im::transport::DeviceProofKey key;
        const bool generated = key.generate();
        report += std::string("generate=") + (generated ? "ok\n" : "FAIL\n");

        const auto pub = generated ? key.publicKeyDer() : im::transport::DeviceProofKey::Bytes{};
        report += std::string("public_key=") + (!pub.empty() ? "ok\n" : "FAIL\n");

        // 用与服务端逐字节一致的规范 message 构造并签名
        im::transport::DeviceProofKey::Bytes sessionId(16, 0x11);
        const auto msg = im::transport::buildDeviceProofMessage(
            "password-login", sessionId, "arm64-selftest-device",
            std::string("13800000000") + std::string(1, '\0') + std::string(64, 'a'), pub);
        const auto sig = key.sign(msg);
        report += std::string("sign=") + (!sig.empty() ? "ok\n" : "FAIL\n");

        // 本地用导出的 SPKI 公钥验签，证明公私钥自洽（P-256 / SHA256withECDSA）
        bool verified = false;
        if (!pub.empty() && !sig.empty()) {
            const unsigned char* p = pub.data();
            EVP_PKEY* pkey = d2i_PUBKEY(nullptr, &p, static_cast<long>(pub.size()));
            if (pkey) {
                EVP_MD_CTX* ctx = EVP_MD_CTX_new();
                if (ctx) {
                    verified = EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
                               EVP_DigestVerifyUpdate(ctx, msg.data(), msg.size()) == 1 &&
                               EVP_DigestVerifyFinal(ctx, sig.data(), sig.size()) == 1;
                    EVP_MD_CTX_free(ctx);
                }
                EVP_PKEY_free(pkey);
            }
        }
        report += std::string("verify=") + (verified ? "ok\n" : "FAIL\n");
        ok = generated && !pub.empty() && !sig.empty() && verified;
    } catch (const std::exception& e) {
        report += std::string("exception=") + e.what() + "\n";
    } catch (...) {
        report += "exception=unknown\n";
    }
    report += ok ? "result=ok\n" : "result=FAIL\n";
    return env->NewStringUTF(report.c_str());
}

// P4 test-only：Android App 进程通过真实 socket 连接宿主机 im_server，并用生产
// ClientCore 完成 TLS、应用握手和加密 Heartbeat 往返。参数由 androidTest 注入，
// 不在二进制中固化测试证书、pin 或身份公钥。
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeSocketHeartbeatTest(
    JNIEnv* env, jclass /*clazz*/, jstring host, jint port, jstring serverName,
    jstring caFile, jstring identityPublicKeyBase64, jstring spkiPinBase64,
    jstring plaintextMarker)
{
    auto toString = [env](jstring value) -> std::string {
        if (!value) return {};
        const char* chars = env->GetStringUTFChars(value, nullptr);
        if (!chars) return {};
        std::string result(chars);
        env->ReleaseStringUTFChars(value, chars);
        return result;
    };

    std::string report;
#if defined(__aarch64__)
    report += "abi=arm64-v8a\n";
#elif defined(__x86_64__)
    report += "abi=x86_64\n";
#else
    report += "abi=other\n";
#endif

    try {
        const std::string hostValue = toString(host);
        const std::string serverNameValue = toString(serverName);
        const std::string caFileValue = toString(caFile);
        const std::string identityValue = toString(identityPublicKeyBase64);
        const std::string pinValue = toString(spkiPinBase64);
        const std::string markerValue = toString(plaintextMarker);

        std::vector<unsigned char> identityPublicKey;
        const bool configOk = !hostValue.empty() && port > 0 && port <= 65535 &&
            !serverNameValue.empty() && !caFileValue.empty() && !pinValue.empty() &&
            !markerValue.empty() &&
            jt::base64Decode(identityValue, identityPublicKey) && identityPublicKey.size() == 32;
        report += std::string("config=") + (configOk ? "ok\n" : "FAIL\n");
        if (!configOk) {
            report += "result=FAIL\n";
            return env->NewStringUTF(report.c_str());
        }

        im::ClientConfig config;
        config.tlsServerName = serverNameValue;
        config.caFile = caFileValue;
        config.identityKeys.emplace(1U, std::move(identityPublicKey));
        config.spkiPins.push_back(pinValue);

        im::ClientCore core(std::move(config));
        core.setHeartbeatIntervalMs(150);
        const bool connected = core.connectToServer(
            hostValue, static_cast<std::uint16_t>(port));
        report += std::string("tls_and_app_handshake=") + (connected ? "ok\n" : "FAIL\n");

        if (connected) core.sendEncryptedHeartbeatProbeForTest(markerValue);
        report += std::string("marker_probe_enqueued=") + (connected ? "ok\n" : "FAIL\n");

        // 150ms 发一次心跳，450ms 无入站即关闭。等待 1.1s 后仍连接，意味着至少多次
        // HeartbeatRq(1010) 经 1040/AES-GCM 发出并收到 HeartbeatRs 解密回包。
        if (connected) std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        const bool heartbeat = connected && core.isConnected();
        report += std::string("encrypted_heartbeat_roundtrip=") +
                  (heartbeat ? "ok\n" : "FAIL\n");
        core.disconnect();
        report += (connected && heartbeat) ? "result=ok\n" : "result=FAIL\n";
    } catch (const std::exception& e) {
        report += std::string("exception=") + e.what() + "\nresult=FAIL\n";
    } catch (...) {
        report += "exception=unknown\nresult=FAIL\n";
    }
    return env->NewStringUTF(report.c_str());
}

} // extern "C"
