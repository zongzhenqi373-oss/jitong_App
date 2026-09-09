#pragma once

#include <jni.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "client_core/DeviceProofService.h"
#include "client_core/TokenManager.h"

namespace jt {

/** JNI 平台原子能力桥。持有 Java GlobalRef，可从 Native 后台线程调用。 */
class JniAuthPlatform {
public:
    JniAuthPlatform(JavaVM* vm, JNIEnv* env, jobject bridge);
    ~JniAuthPlatform();
    JniAuthPlatform(const JniAuthPlatform&) = delete;
    JniAuthPlatform& operator=(const JniAuthPlatform&) = delete;

    bool valid() const;
    std::string deviceId() const;
    std::vector<unsigned char> publicKey() const;
    std::vector<unsigned char> sign(const std::vector<unsigned char>& payload) const;
    bool loadStore(std::vector<unsigned char>& out) const;
    bool saveStore(const std::vector<unsigned char>& blob) const;

private:
    JavaVM* m_vm = nullptr;
    jobject m_bridge = nullptr;
    jmethodID m_deviceId = nullptr;
    jmethodID m_publicKey = nullptr;
    jmethodID m_sign = nullptr;
    jmethodID m_loadStore = nullptr;
    jmethodID m_saveStore = nullptr;
};

class JniP256Signer final : public im::account::IP256Signer {
public:
    explicit JniP256Signer(std::shared_ptr<JniAuthPlatform> platform)
        : m_platform(std::move(platform)) {}
    Bytes publicKeyDer() const override;
    Bytes sign(const Bytes& message) const override;
private:
    std::shared_ptr<JniAuthPlatform> m_platform;
};

/** 单 blob 持久化：先完整保存新快照，成功后才替换内存态，提供原子批量语义。 */
class JniTokenStore final : public im::account::ITokenStore {
public:
    explicit JniTokenStore(std::shared_ptr<JniAuthPlatform> platform);
    bool get(const std::string& key, std::string& valueOut) const override;
    bool put(const std::string& key, const std::string& value) override;
    bool remove(const std::string& key) override;
    bool clearAll() override;
    bool applyAtomically(const Puts& puts, const Removes& removes) override;

private:
    bool persist(const std::map<std::string, std::string>& values) const;
    std::shared_ptr<JniAuthPlatform> m_platform;
    mutable std::mutex m_mutex;
    std::map<std::string, std::string> m_values;
};

} // namespace jt
