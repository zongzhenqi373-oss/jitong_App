#include "jni_auth_platform.h"

#include <android/log.h>
#include <cstdint>
#include <limits>

namespace jt {
namespace {

class EnvScope {
public:
    explicit EnvScope(JavaVM* vm) : m_vm(vm) {
        if (!m_vm) return;
        if (m_vm->GetEnv(reinterpret_cast<void**>(&m_env), JNI_VERSION_1_6) == JNI_EDETACHED) {
            if (m_vm->AttachCurrentThread(&m_env, nullptr) == JNI_OK) m_attached = true;
            else m_env = nullptr;
        }
    }
    ~EnvScope() { if (m_attached) m_vm->DetachCurrentThread(); }
    JNIEnv* get() const { return m_env; }
private:
    JavaVM* m_vm = nullptr;
    JNIEnv* m_env = nullptr;
    bool m_attached = false;
};

bool clearException(JNIEnv* env, const char* operation) {
    if (!env || !env->ExceptionCheck()) return false;
    env->ExceptionClear();
    __android_log_print(ANDROID_LOG_ERROR, "JitongKernel", "platform bridge failed: %s", operation);
    return true;
}

std::vector<unsigned char> bytes(JNIEnv* env, jbyteArray array) {
    if (!env || !array) return {};
    const jsize size = env->GetArrayLength(array);
    std::vector<unsigned char> out(static_cast<std::size_t>(size));
    if (size) env->GetByteArrayRegion(array, 0, size, reinterpret_cast<jbyte*>(out.data()));
    return out;
}

void appendU32(std::vector<unsigned char>& out, std::uint32_t value) {
    out.push_back(static_cast<unsigned char>((value >> 24) & 0xff));
    out.push_back(static_cast<unsigned char>((value >> 16) & 0xff));
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xff));
    out.push_back(static_cast<unsigned char>(value & 0xff));
}

bool readU32(const std::vector<unsigned char>& in, std::size_t& at, std::uint32_t& value) {
    if (at + 4 > in.size()) return false;
    value = (static_cast<std::uint32_t>(in[at]) << 24) |
            (static_cast<std::uint32_t>(in[at + 1]) << 16) |
            (static_cast<std::uint32_t>(in[at + 2]) << 8) |
            static_cast<std::uint32_t>(in[at + 3]);
    at += 4;
    return true;
}

std::vector<unsigned char> encode(const std::map<std::string, std::string>& values) {
    std::vector<unsigned char> out{'J','T','K','V',1};
    appendU32(out, static_cast<std::uint32_t>(values.size()));
    for (const auto& item : values) {
        appendU32(out, static_cast<std::uint32_t>(item.first.size()));
        appendU32(out, static_cast<std::uint32_t>(item.second.size()));
        out.insert(out.end(), item.first.begin(), item.first.end());
        out.insert(out.end(), item.second.begin(), item.second.end());
    }
    return out;
}

bool decode(const std::vector<unsigned char>& in, std::map<std::string, std::string>& values) {
    if (in.size() < 9 || in[0] != 'J' || in[1] != 'T' || in[2] != 'K' || in[3] != 'V' || in[4] != 1)
        return false;
    std::size_t at = 5;
    std::uint32_t count = 0;
    if (!readU32(in, at, count) || count > 64) return false;
    std::map<std::string, std::string> parsed;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t keySize = 0, valueSize = 0;
        if (!readU32(in, at, keySize) || !readU32(in, at, valueSize)) return false;
        if (keySize > 1024 || valueSize > 64 * 1024 || at + keySize + valueSize > in.size()) return false;
        std::string key(reinterpret_cast<const char*>(in.data() + at), keySize); at += keySize;
        std::string value(reinterpret_cast<const char*>(in.data() + at), valueSize); at += valueSize;
        if (key.empty() || !parsed.emplace(std::move(key), std::move(value)).second) return false;
    }
    if (at != in.size()) return false;
    values = std::move(parsed);
    return true;
}

} // namespace

JniAuthPlatform::JniAuthPlatform(JavaVM* vm, JNIEnv* env, jobject bridge) : m_vm(vm) {
    if (!m_vm || !env || !bridge) return;
    m_bridge = env->NewGlobalRef(bridge);
    const jclass cls = env->GetObjectClass(bridge);
    if (!m_bridge || !cls) return;
    m_deviceId = env->GetMethodID(cls, "deviceId", "()Ljava/lang/String;");
    m_publicKey = env->GetMethodID(cls, "publicKey", "()[B");
    m_sign = env->GetMethodID(cls, "sign", "([B)[B");
    m_loadStore = env->GetMethodID(cls, "loadStore", "()[B");
    m_saveStore = env->GetMethodID(cls, "saveStore", "([B)Z");
    env->DeleteLocalRef(cls);
    clearException(env, "resolve methods");
}

JniAuthPlatform::~JniAuthPlatform() {
    EnvScope scope(m_vm);
    if (scope.get() && m_bridge) scope.get()->DeleteGlobalRef(m_bridge);
}

bool JniAuthPlatform::valid() const {
    return m_bridge && m_deviceId && m_publicKey && m_sign && m_loadStore && m_saveStore;
}

std::string JniAuthPlatform::deviceId() const {
    EnvScope scope(m_vm); JNIEnv* env = scope.get();
    if (!env || !valid()) return {};
    auto value = static_cast<jstring>(env->CallObjectMethod(m_bridge, m_deviceId));
    if (clearException(env, "deviceId") || !value) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string out = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(value, chars);
    env->DeleteLocalRef(value);
    return out;
}

std::vector<unsigned char> JniAuthPlatform::publicKey() const {
    EnvScope scope(m_vm); JNIEnv* env = scope.get();
    if (!env || !valid()) return {};
    auto array = static_cast<jbyteArray>(env->CallObjectMethod(m_bridge, m_publicKey));
    if (clearException(env, "publicKey") || !array) return {};
    auto out = bytes(env, array); env->DeleteLocalRef(array); return out;
}

std::vector<unsigned char> JniAuthPlatform::sign(const std::vector<unsigned char>& payload) const {
    EnvScope scope(m_vm); JNIEnv* env = scope.get();
    if (!env || !valid()) return {};
    auto input = env->NewByteArray(static_cast<jsize>(payload.size()));
    if (!input) return {};
    if (!payload.empty()) env->SetByteArrayRegion(input, 0, static_cast<jsize>(payload.size()), reinterpret_cast<const jbyte*>(payload.data()));
    auto result = static_cast<jbyteArray>(env->CallObjectMethod(m_bridge, m_sign, input));
    env->DeleteLocalRef(input);
    if (clearException(env, "sign") || !result) return {};
    auto out = bytes(env, result); env->DeleteLocalRef(result); return out;
}

bool JniAuthPlatform::loadStore(std::vector<unsigned char>& out) const {
    EnvScope scope(m_vm); JNIEnv* env = scope.get();
    if (!env || !valid()) return false;
    auto result = static_cast<jbyteArray>(env->CallObjectMethod(m_bridge, m_loadStore));
    if (clearException(env, "loadStore") || !result) return false;
    out = bytes(env, result); env->DeleteLocalRef(result); return true;
}

bool JniAuthPlatform::saveStore(const std::vector<unsigned char>& blob) const {
    EnvScope scope(m_vm); JNIEnv* env = scope.get();
    if (!env || !valid()) return false;
    auto input = env->NewByteArray(static_cast<jsize>(blob.size()));
    if (!input) return false;
    if (!blob.empty()) env->SetByteArrayRegion(input, 0, static_cast<jsize>(blob.size()), reinterpret_cast<const jbyte*>(blob.data()));
    const bool ok = env->CallBooleanMethod(m_bridge, m_saveStore, input) == JNI_TRUE;
    env->DeleteLocalRef(input);
    return !clearException(env, "saveStore") && ok;
}

im::account::IP256Signer::Bytes JniP256Signer::publicKeyDer() const {
    return m_platform ? m_platform->publicKey() : Bytes{};
}
im::account::IP256Signer::Bytes JniP256Signer::sign(const Bytes& message) const {
    return m_platform ? m_platform->sign(message) : Bytes{};
}

JniTokenStore::JniTokenStore(std::shared_ptr<JniAuthPlatform> platform)
    : m_platform(std::move(platform)) {
    std::vector<unsigned char> blob;
    std::map<std::string, std::string> restored;
    if (m_platform && m_platform->loadStore(blob) && decode(blob, restored)) m_values = std::move(restored);
}

bool JniTokenStore::get(const std::string& key, std::string& valueOut) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_values.find(key); if (it == m_values.end()) return false;
    valueOut = it->second; return true;
}
bool JniTokenStore::persist(const std::map<std::string, std::string>& values) const {
    return m_platform && m_platform->saveStore(encode(values));
}
bool JniTokenStore::applyAtomically(const Puts& puts, const Removes& removes) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto next = m_values;
    for (const auto& item : puts) next[item.first] = item.second;
    for (const auto& key : removes) next.erase(key);
    if (!persist(next)) return false;
    m_values = std::move(next); return true;
}
bool JniTokenStore::put(const std::string& key, const std::string& value) { return applyAtomically({{key, value}}, {}); }
bool JniTokenStore::remove(const std::string& key) { return applyAtomically({}, {key}); }
bool JniTokenStore::clearAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<std::string, std::string> empty;
    if (!persist(empty)) return false;
    m_values.clear(); return true;
}

} // namespace jt
