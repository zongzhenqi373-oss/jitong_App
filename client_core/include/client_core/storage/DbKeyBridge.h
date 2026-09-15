// 平台密钥桥（P6-T02）。
//
// C++ 侧只定义"我要一把 32 字节的库密钥"这个**请求**，不关心它在 Android 上怎么被
// Keystore/PBKDF2 包装持久化——那属于平台侧策略，由 Kotlin/Java 实现并通过 JNI 注入。
//
// 硬约束（对应 24.4）：
//   - 桥只返回"本次开库所需的 32 字节副本"，C++ 用完立即清零，**不持久化、不回传、不记录**；
//   - 拿不到就返回 Unavailable（Token-only 冷启动、密码未登录等场景），
//     **禁止**用 Access/Refresh Token 派生密钥，也**禁止**静默回退到任何"永远可用"的密钥；
//   - 错误消息中不得包含密钥或其 hex。

#ifndef CLIENT_CORE_DB_KEY_BRIDGE_H
#define CLIENT_CORE_DB_KEY_BRIDGE_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include <openssl/crypto.h>

namespace im {
namespace storage {

/** 32 字节密钥的安全容器：析构/clear 时清零。 */
class SecureKeyBuffer {
public:
    using Bytes = std::vector<unsigned char>;

    SecureKeyBuffer() = default;
    explicit SecureKeyBuffer(Bytes data) : m_data(std::move(data)) {}
    ~SecureKeyBuffer() { clear(); }

    SecureKeyBuffer(const SecureKeyBuffer&) = delete;
    SecureKeyBuffer& operator=(const SecureKeyBuffer&) = delete;

    SecureKeyBuffer(SecureKeyBuffer&& other) noexcept : m_data(std::move(other.m_data)) {}
    SecureKeyBuffer& operator=(SecureKeyBuffer&& other) noexcept
    {
        if (this != &other) {
            clear();
            m_data = std::move(other.m_data);
        }
        return *this;
    }

    void clear()
    {
        if (!m_data.empty()) {
            OPENSSL_cleanse(m_data.data(), m_data.size());
            m_data.clear();
        }
    }
    bool empty() const { return m_data.empty(); }
    std::size_t size() const { return m_data.size(); }
    const Bytes& bytes() const { return m_data; }

    static constexpr std::size_t kExpectedSize = 32;

private:
    Bytes m_data;
};

/** 平台密钥桥：由 Android（或其它平台）实现。 */
class IPlatformKeyBridge {
public:
    virtual ~IPlatformKeyBridge() = default;

    enum class Result {
        Ok,             // 成功返回 32 字节密钥
        Unavailable,    // 拿不到（未登录 / 需要密码 / Token-only 冷启动）→ 保持锁定
        Error,          // 其它错误
    };

    /**
     * 取指定账号的库密钥。
     * @param ownerId 账号 id
     * @param outKey  成功时写入 32 字节密钥
     */
    virtual Result loadKey(std::int64_t ownerId, SecureKeyBuffer& outKey) = 0;

    /** 删除指定账号的本地密钥（显式"清除本机数据"时使用，登录流程禁止自动调用）。 */
    virtual void deleteKey(std::int64_t ownerId) = 0;
};

const char* toString(IPlatformKeyBridge::Result r);

} // namespace storage
} // namespace im

#endif // CLIENT_CORE_DB_KEY_BRIDGE_H
