#include "jni_db_key_bridge.h"

#include <android/log.h>

#include <vector>

namespace jt {
namespace {

// 在任意（可能是 Native 后台）线程上取 JNIEnv：必要时 Attach，析构时仅对本次 Attach 的线程 Detach。
class EnvScope {
public:
    explicit EnvScope(JavaVM* vm) : m_vm(vm)
    {
        if (!m_vm) return;
        if (m_vm->GetEnv(reinterpret_cast<void**>(&m_env), JNI_VERSION_1_6) == JNI_EDETACHED) {
            if (m_vm->AttachCurrentThread(&m_env, nullptr) == JNI_OK) m_attached = true;
            else m_env = nullptr;
        }
    }
    ~EnvScope()
    {
        if (m_attached) m_vm->DetachCurrentThread();
    }
    JNIEnv* get() const { return m_env; }

private:
    JavaVM* m_vm = nullptr;
    JNIEnv* m_env = nullptr;
    bool m_attached = false;
};

bool clearException(JNIEnv* env, const char* operation)
{
    if (!env || !env->ExceptionCheck()) return false;
    env->ExceptionClear();
    __android_log_print(ANDROID_LOG_ERROR, "JitongKernel", "db key bridge failed: %s", operation);
    return true;
}

} // namespace

JniDbKeyBridge::JniDbKeyBridge(JavaVM* vm, JNIEnv* env, jobject bridge) : m_vm(vm)
{
    if (!m_vm || !env || !bridge) return;
    m_bridge = env->NewGlobalRef(bridge);
    const jclass cls = env->GetObjectClass(bridge);
    if (!m_bridge || !cls) return;
    m_loadKey = env->GetMethodID(cls, "loadKey", "(I)[B");
    m_deleteKey = env->GetMethodID(cls, "deleteKey", "(I)V");
    env->DeleteLocalRef(cls);
    clearException(env, "resolve db key methods");
}

JniDbKeyBridge::~JniDbKeyBridge()
{
    EnvScope scope(m_vm);
    if (scope.get() && m_bridge) scope.get()->DeleteGlobalRef(m_bridge);
}

bool JniDbKeyBridge::valid() const
{
    return m_bridge && m_loadKey && m_deleteKey;
}

im::storage::IPlatformKeyBridge::Result JniDbKeyBridge::loadKey(
    std::int64_t ownerId, im::storage::SecureKeyBuffer& outKey)
{
    EnvScope scope(m_vm);
    JNIEnv* env = scope.get();
    if (!env || !valid()) return Result::Error;

    auto array = static_cast<jbyteArray>(
        env->CallObjectMethod(m_bridge, m_loadKey, static_cast<jint>(ownerId)));
    if (clearException(env, "loadKey")) return Result::Error;
    // Kotlin 返回 null：无 passHash（Token-only 冷启动）或密钥不可用 → 保持锁定
    if (!array) return Result::Unavailable;

    const jsize n = env->GetArrayLength(array);
    if (n != static_cast<jsize>(im::storage::SecureKeyBuffer::kExpectedSize)) {
        env->DeleteLocalRef(array);
        return Result::Error;
    }

    im::storage::SecureKeyBuffer::Bytes key(static_cast<std::size_t>(n));
    env->GetByteArrayRegion(array, 0, n, reinterpret_cast<jbyte*>(key.data()));
    // F13：覆盖 Java 侧 key 字节（尽力清零；JVM 内存无法绝对擦除，但至少不残留明文副本）
    std::vector<jbyte> zeros(static_cast<std::size_t>(n), 0);
    env->SetByteArrayRegion(array, 0, n, zeros.data());
    env->DeleteLocalRef(array);

    outKey = im::storage::SecureKeyBuffer(std::move(key));
    return Result::Ok;
}

void JniDbKeyBridge::deleteKey(std::int64_t ownerId)
{
    EnvScope scope(m_vm);
    JNIEnv* env = scope.get();
    if (!env || !valid()) return;
    env->CallVoidMethod(m_bridge, m_deleteKey, static_cast<jint>(ownerId));
    clearException(env, "deleteKey");
}

} // namespace jt
