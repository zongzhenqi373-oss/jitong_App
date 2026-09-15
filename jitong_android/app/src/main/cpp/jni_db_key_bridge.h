#pragma once
// Native 数据库密钥平台桥（P6-T02 / S6 正式路径）。
//
// 实现 im::storage::IPlatformKeyBridge：C++ 只表达「我要一把 32 字节的库密钥」这个请求，
// 具体如何从 Android 的 DbKeyManager（PBKDF2(passHash) + AES-GCM 包装 realKey）解出，
// 交给 Kotlin 侧对象完成。Token-only 冷启动没有 passHash 时，Kotlin 返回 null →
// C++ 映射为 Unavailable → NativeDatabase 保持 Locked，绝不降级、绝不拿 Token 派生 key。
//
// 约束（对应 24.4）：
//   - 只持有 Kotlin 桥对象的 GlobalRef，不缓存 key、不持久化 key、不回传 key；
//   - loadKey 拿到的 32 字节副本进入 SecureKeyBuffer，用完由 NativeDatabase 清零；
//   - deleteKey 仅服务于「清除本机数据」这类显式操作，登录/认证流程禁止自动调用。

#include <jni.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "client_core/storage/DbKeyBridge.h"

namespace jt {

class JniDbKeyBridge final : public im::storage::IPlatformKeyBridge {
public:
    JniDbKeyBridge(JavaVM* vm, JNIEnv* env, jobject bridge);
    ~JniDbKeyBridge() override;
    JniDbKeyBridge(const JniDbKeyBridge&) = delete;
    JniDbKeyBridge& operator=(const JniDbKeyBridge&) = delete;

    /** 桥对象及其方法是否都解析成功。 */
    bool valid() const;

    im::storage::IPlatformKeyBridge::Result loadKey(
        std::int64_t ownerId, im::storage::SecureKeyBuffer& outKey) override;
    void deleteKey(std::int64_t ownerId) override;

private:
    JavaVM* m_vm = nullptr;
    jobject m_bridge = nullptr;
    jmethodID m_loadKey = nullptr;   // byte[] loadKey(int ownerId)
    jmethodID m_deleteKey = nullptr; // void  deleteKey(int ownerId)
};

} // namespace jt
