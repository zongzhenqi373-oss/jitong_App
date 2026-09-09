// 敏感字符串容器（P5-T04）。
//
// 用于承载 access_token / refresh_token 等敏感值：析构（以及 clear/赋值覆盖前）会用
// OPENSSL_cleanse 把底层缓冲清零，避免明文 token 长期驻留在堆内存里被 dump/取证。
//
// 注意：这不是"加密存储"，只是尽量缩短明文在内存中的暴露窗口；持久化的加密由 TokenStore
// 的具体实现（Android Keystore / iOS Keychain）负责。

#ifndef CLIENT_CORE_SECURE_STRING_H
#define CLIENT_CORE_SECURE_STRING_H

#include <cstddef>
#include <string>
#include <utility>

#include <openssl/crypto.h>

namespace im {
namespace account {

class SecureString {
public:
    SecureString() = default;
    explicit SecureString(std::string value) : m_data(std::move(value)) {}
    SecureString(const char* s) : m_data(s ? s : "") {}

    SecureString(const SecureString& other) : m_data(other.m_data) {}
    SecureString(SecureString&& other) noexcept : m_data(std::move(other.m_data))
    {
        other.wipe();
    }

    SecureString& operator=(const SecureString& other)
    {
        if (this != &other) {
            wipe();
            m_data = other.m_data;
        }
        return *this;
    }
    SecureString& operator=(SecureString&& other) noexcept
    {
        if (this != &other) {
            wipe();
            m_data = std::move(other.m_data);
            other.wipe();
        }
        return *this;
    }

    ~SecureString() { wipe(); }

    const std::string& str() const { return m_data; }
    bool empty() const { return m_data.empty(); }
    std::size_t size() const { return m_data.size(); }

    void assign(std::string value)
    {
        wipe();
        m_data = std::move(value);
    }
    void clear() { wipe(); }

private:
    void wipe()
    {
        if (!m_data.empty()) {
            OPENSSL_cleanse(&m_data[0], m_data.size());
            m_data.clear();
        }
    }
    std::string m_data;
};

} // namespace account
} // namespace im

#endif // CLIENT_CORE_SECURE_STRING_H
