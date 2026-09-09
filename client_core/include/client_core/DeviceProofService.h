// 设备证明服务（P5-T03）。
//
// 把"为某类认证操作生成设备签名"这件事封装成语义清晰的四个方法，规范字节与服务端
// im_server/src/auth/DeviceProof.cpp 的 deviceproof::message 以及 AuthHandler::verifyDevice
// 逐字节一致：
//
//   message = "jitong-device-proof-v1"
//           || field(operation) || field(appSessionId) || field(deviceId)
//           || field(credentialBinding) || field(publicKey)
//   field(x) = u32be(len(x)) || x
//   signature = SHA256withECDSA(prime256v1) over message, DER 编码
//
// 四类操作与其 credentialBinding / 是否绑定公钥（与服务端一致）：
//   password-login : tel + '\0' + sha256Hex(pass)                 , 绑定公钥
//   token-login    : access_token                                  , 不绑定公钥
//   token-refresh  : refresh_token + '\0' + request_id             , 不绑定公钥
//   logout         : refresh_token + '\0' + (all ? "1" : "0")      , 不绑定公钥
//
// 签名器通过 IP256Signer 抽象注入：桌面/测试用软件实现（OpenSSL），Android 用 Keystore
// 硬件密钥实现（在 JNI 层适配），本服务本身不关心密钥来自哪里。

#ifndef CLIENT_CORE_DEVICE_PROOF_SERVICE_H
#define CLIENT_CORE_DEVICE_PROOF_SERVICE_H

#include <cstdint>
#include <string>
#include <vector>

namespace im {
namespace account {

/** P-256 签名器抽象。实现方保证密钥为 prime256v1，签名为 SHA256withECDSA DER。 */
class IP256Signer {
public:
    using Bytes = std::vector<unsigned char>;
    virtual ~IP256Signer() = default;
    /** X.509 SubjectPublicKeyInfo DER 公钥；不可用返回空。 */
    virtual Bytes publicKeyDer() const = 0;
    /** 对 message 做 SHA256withECDSA 签名，返回 DER；失败返回空。 */
    virtual Bytes sign(const Bytes& message) const = 0;
};

/** 一次设备证明结果：公钥（可能为空）+ 签名。 */
struct DeviceProof {
    IP256Signer::Bytes publicKeyDer; // 仅 password-login 非空
    IP256Signer::Bytes signature;
    bool ok() const { return !signature.empty(); }
};

class DeviceProofService {
public:
    using Bytes = IP256Signer::Bytes;

    // signer 生命周期由调用方保证（引用持有）。deviceId 为本设备稳定标识。
    DeviceProofService(const IP256Signer& signer, std::string deviceId)
        : m_signer(signer), m_deviceId(std::move(deviceId)) {}

    const std::string& deviceId() const { return m_deviceId; }

    // 以下四个方法的 appSessionId 均为当前应用安全通道 sessionId（握手成功后有效）。

    /** password-login：credential = tel + '\0' + passSha256Hex；绑定设备公钥。 */
    DeviceProof forPasswordLogin(const Bytes& appSessionId, const std::string& tel,
                                 const std::string& passSha256Hex) const;

    /** token-login：credential = accessToken；不绑定公钥。 */
    DeviceProof forTokenLogin(const Bytes& appSessionId, const std::string& accessToken) const;

    /** token-refresh：credential = refreshToken + '\0' + requestId；不绑定公钥。 */
    DeviceProof forTokenRefresh(const Bytes& appSessionId, const std::string& refreshToken,
                                const std::string& requestId) const;

    /** logout：credential = refreshToken + '\0' + (allDevices ? "1" : "0")；不绑定公钥。 */
    DeviceProof forLogout(const Bytes& appSessionId, const std::string& refreshToken,
                          bool allDevices) const;

    // 供测试/服务端对齐使用：构造 canonical message（与服务端 deviceproof::message 一致）。
    static Bytes buildMessage(const std::string& operation, const Bytes& appSessionId,
                              const std::string& deviceId, const std::string& credentialBinding,
                              const Bytes& publicKey);

private:
    DeviceProof make(const std::string& operation, const Bytes& appSessionId,
                     const std::string& credentialBinding, bool bindPublicKey) const;

    const IP256Signer& m_signer;
    std::string m_deviceId;
};

} // namespace account
} // namespace im

#endif // CLIENT_CORE_DEVICE_PROOF_SERVICE_H
