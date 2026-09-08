#include "transport/DeviceProof.h"

#include <openssl/evp.h>
#include <openssl/x509.h>

namespace im {
namespace transport {
namespace {

void appendField(DeviceProofKey::Bytes& out, const unsigned char* data, std::size_t size)
{
    const auto n = static_cast<std::uint32_t>(size);
    out.push_back(static_cast<unsigned char>((n >> 24) & 0xFF));
    out.push_back(static_cast<unsigned char>((n >> 16) & 0xFF));
    out.push_back(static_cast<unsigned char>((n >> 8) & 0xFF));
    out.push_back(static_cast<unsigned char>(n & 0xFF));
    if (size != 0) out.insert(out.end(), data, data + size);
}

void appendField(DeviceProofKey::Bytes& out, const std::string& value)
{
    appendField(out, reinterpret_cast<const unsigned char*>(value.data()), value.size());
}

void appendField(DeviceProofKey::Bytes& out, const DeviceProofKey::Bytes& value)
{
    appendField(out, value.data(), value.size());
}

} // namespace

DeviceProofKey::~DeviceProofKey()
{
    if (m_pkey) EVP_PKEY_free(static_cast<EVP_PKEY*>(m_pkey));
    m_pkey = nullptr;
}

bool DeviceProofKey::generate()
{
    if (m_pkey) {
        EVP_PKEY_free(static_cast<EVP_PKEY*>(m_pkey));
        m_pkey = nullptr;
    }
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (!ctx) return false;
    EVP_PKEY* pkey = nullptr;
    const bool ok = EVP_PKEY_keygen_init(ctx) > 0 &&
                    EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_X9_62_prime256v1) > 0 &&
                    EVP_PKEY_keygen(ctx, &pkey) > 0;
    EVP_PKEY_CTX_free(ctx);
    if (!ok || !pkey) {
        if (pkey) EVP_PKEY_free(pkey);
        return false;
    }
    m_pkey = pkey;
    return true;
}

DeviceProofKey::Bytes DeviceProofKey::publicKeyDer() const
{
    if (!m_pkey) return {};
    unsigned char* der = nullptr;
    const int len = i2d_PUBKEY(static_cast<EVP_PKEY*>(m_pkey), &der);
    if (len <= 0 || !der) {
        if (der) OPENSSL_free(der);
        return {};
    }
    Bytes out(der, der + len);
    OPENSSL_free(der);
    return out;
}

DeviceProofKey::Bytes DeviceProofKey::sign(const Bytes& message) const
{
    if (!m_pkey) return {};
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};
    Bytes signature;
    std::size_t sigLen = 0;
    const bool ok =
        EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr,
                           static_cast<EVP_PKEY*>(m_pkey)) == 1 &&
        EVP_DigestSignUpdate(ctx, message.data(), message.size()) == 1 &&
        EVP_DigestSignFinal(ctx, nullptr, &sigLen) == 1;
    if (ok) {
        signature.resize(sigLen);
        if (EVP_DigestSignFinal(ctx, signature.data(), &sigLen) == 1) {
            signature.resize(sigLen);
        } else {
            signature.clear();
        }
    }
    EVP_MD_CTX_free(ctx);
    return signature;
}

DeviceProofKey::Bytes buildDeviceProofMessage(const std::string& operation,
                                              const DeviceProofKey::Bytes& appSessionId,
                                              const std::string& deviceId,
                                              const std::string& credentialBinding,
                                              const DeviceProofKey::Bytes& publicKeyDer)
{
    const std::string label = "jitong-device-proof-v1";
    DeviceProofKey::Bytes out(label.begin(), label.end());
    appendField(out, operation);
    appendField(out, appSessionId);
    appendField(out, deviceId);
    appendField(out, credentialBinding);
    appendField(out, publicKeyDer);
    return out;
}

} // namespace transport
} // namespace im
