// 设备证明（Device Proof）：客户端侧 P-256 密钥与签名。
//
// 与服务端 im_server/src/auth/DeviceProof.cpp 的规范逐字节一致：
//   message = "jitong-device-proof-v1"
//           || field(operation) || field(appSessionId) || field(deviceId)
//           || field(credentialBinding) || field(publicKey)
//   field(x) = u32be(len(x)) || x
// 签名为 SHA256withECDSA（prime256v1），DER 编码；公钥为 X.509 SubjectPublicKeyInfo DER。
//
// 该模块不做任何业务判断，只负责：生成/持有一对 P-256 密钥、导出 DER 公钥、
// 对给定 message 产出 DER 签名。密钥所有权与生命周期由调用方（ClientCore）管理。

#ifndef CLIENT_CORE_DEVICE_PROOF_H
#define CLIENT_CORE_DEVICE_PROOF_H

#include <cstdint>
#include <string>
#include <vector>

namespace im {
namespace transport {

class DeviceProofKey {
public:
    using Bytes = std::vector<unsigned char>;

    DeviceProofKey() = default;
    ~DeviceProofKey();

    DeviceProofKey(const DeviceProofKey&) = delete;
    DeviceProofKey& operator=(const DeviceProofKey&) = delete;

    /** 生成一对新的 P-256 (prime256v1) 密钥。成功返回 true。 */
    bool generate();

    /** 是否已持有可用私钥。 */
    bool valid() const { return m_pkey != nullptr; }

    /** 导出 X.509 SubjectPublicKeyInfo DER 公钥；失败返回空。 */
    Bytes publicKeyDer() const;

    /** 对 message 做 SHA256withECDSA 签名，返回 DER 编码；失败返回空。 */
    Bytes sign(const Bytes& message) const;

private:
    void* m_pkey = nullptr; // EVP_PKEY*（避免在头文件暴露 OpenSSL 类型）
};

/**
 * 构造设备证明规范 message（与服务端 deviceproof::message 逐字节一致）。
 * @param publicKeyDer 仅在需要绑定公钥的操作（如 password-login）里传入非空。
 */
DeviceProofKey::Bytes buildDeviceProofMessage(const std::string& operation,
                                              const DeviceProofKey::Bytes& appSessionId,
                                              const std::string& deviceId,
                                              const std::string& credentialBinding,
                                              const DeviceProofKey::Bytes& publicKeyDer);

} // namespace transport
} // namespace im

#endif // CLIENT_CORE_DEVICE_PROOF_H
