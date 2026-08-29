#include "auth/DeviceProof.h"

#include <memory>
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/x509.h>

namespace imsrv::deviceproof {
namespace {
void field(Bytes& out, const std::uint8_t* data, std::size_t size)
{
    const auto n = static_cast<std::uint32_t>(size);
    out.push_back(static_cast<std::uint8_t>(n >> 24));
    out.push_back(static_cast<std::uint8_t>(n >> 16));
    out.push_back(static_cast<std::uint8_t>(n >> 8));
    out.push_back(static_cast<std::uint8_t>(n));
    if (size != 0) out.insert(out.end(), data, data + size);
}
void field(Bytes& out, const std::string& value)
{
    field(out, reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
}
void field(Bytes& out, const Bytes& value) { field(out, value.data(), value.size()); }
}

Bytes message(const std::string& operation, const Bytes& appSessionId,
              const std::string& deviceId, const std::string& credentialBinding,
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

bool verifyP256(const Bytes& publicKeyDer, const Bytes& signedMessage, const Bytes& signatureDer)
{
    if (publicKeyDer.empty() || publicKeyDer.size() > 512 || signatureDer.empty() || signatureDer.size() > 144)
        return false;
    const unsigned char* cursor = publicKeyDer.data();
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        d2i_PUBKEY(nullptr, &cursor, static_cast<long>(publicKeyDer.size())), EVP_PKEY_free);
    if (!key || cursor != publicKeyDer.data() + publicKeyDer.size() ||
        EVP_PKEY_base_id(key.get()) != EVP_PKEY_EC) return false;
    char group[64]{};
    std::size_t groupLength = 0;
    if (EVP_PKEY_get_utf8_string_param(key.get(), OSSL_PKEY_PARAM_GROUP_NAME,
                                       group, sizeof(group), &groupLength) != 1 ||
        (std::string(group) != "prime256v1" && std::string(group) != "secp256r1")) return false;
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    return context && EVP_DigestVerifyInit(context.get(), nullptr, EVP_sha256(), nullptr, key.get()) == 1 &&
        EVP_DigestVerifyUpdate(context.get(), signedMessage.data(), signedMessage.size()) == 1 &&
        EVP_DigestVerifyFinal(context.get(), signatureDer.data(), signatureDer.size()) == 1;
}
} // namespace imsrv::deviceproof
