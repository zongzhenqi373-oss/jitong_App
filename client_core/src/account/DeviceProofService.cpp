#include "client_core/DeviceProofService.h"

namespace im {
namespace account {

namespace {
void appendU32Be(DeviceProofService::Bytes& out, std::uint32_t v)
{
    out.push_back(static_cast<unsigned char>((v >> 24) & 0xFF));
    out.push_back(static_cast<unsigned char>((v >> 16) & 0xFF));
    out.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
    out.push_back(static_cast<unsigned char>(v & 0xFF));
}

void field(DeviceProofService::Bytes& out, const unsigned char* data, std::size_t n)
{
    appendU32Be(out, static_cast<std::uint32_t>(n));
    if (n) out.insert(out.end(), data, data + n);
}
void field(DeviceProofService::Bytes& out, const std::string& s)
{
    field(out, reinterpret_cast<const unsigned char*>(s.data()), s.size());
}
void field(DeviceProofService::Bytes& out, const DeviceProofService::Bytes& b)
{
    field(out, b.data(), b.size());
}
} // namespace

DeviceProofService::Bytes DeviceProofService::buildMessage(const std::string& operation,
                                                           const Bytes& appSessionId,
                                                           const std::string& deviceId,
                                                           const std::string& credentialBinding,
                                                           const Bytes& publicKey)
{
    const std::string label = "jitong-device-proof-v1";
    Bytes out(label.begin(), label.end());
    field(out, operation);
    field(out, appSessionId);
    field(out, deviceId);
    field(out, credentialBinding);
    field(out, publicKey);
    return out;
}

DeviceProof DeviceProofService::make(const std::string& operation, const Bytes& appSessionId,
                                     const std::string& credentialBinding, bool bindPublicKey) const
{
    DeviceProof proof;
    const Bytes pub = m_signer.publicKeyDer();
    if (pub.empty()) return proof; // 无公钥→无法证明
    const Bytes boundPub = bindPublicKey ? pub : Bytes{};
    const Bytes msg = buildMessage(operation, appSessionId, m_deviceId, credentialBinding, boundPub);
    proof.signature = m_signer.sign(msg);
    if (proof.signature.empty()) return proof; // 签名失败
    if (bindPublicKey) proof.publicKeyDer = pub;
    return proof;
}

DeviceProof DeviceProofService::forPasswordLogin(const Bytes& appSessionId, const std::string& tel,
                                                 const std::string& passSha256Hex) const
{
    return make("password-login", appSessionId,
                tel + std::string(1, '\0') + passSha256Hex, /*bindPublicKey=*/true);
}

DeviceProof DeviceProofService::forTokenLogin(const Bytes& appSessionId,
                                              const std::string& accessToken) const
{
    return make("token-login", appSessionId, accessToken, /*bindPublicKey=*/false);
}

DeviceProof DeviceProofService::forTokenRefresh(const Bytes& appSessionId,
                                                const std::string& refreshToken,
                                                const std::string& requestId) const
{
    return make("token-refresh", appSessionId,
                refreshToken + std::string(1, '\0') + requestId, /*bindPublicKey=*/false);
}

DeviceProof DeviceProofService::forLogout(const Bytes& appSessionId,
                                          const std::string& refreshToken, bool allDevices) const
{
    return make("logout", appSessionId,
                refreshToken + std::string(1, '\0') + (allDevices ? "1" : "0"),
                /*bindPublicKey=*/false);
}

} // namespace account
} // namespace im
