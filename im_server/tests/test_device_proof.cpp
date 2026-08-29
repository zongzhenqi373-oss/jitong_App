#include "auth/DeviceProof.h"

#include <cassert>
#include <memory>
#include <openssl/evp.h>
#include <openssl/x509.h>

int main()
{
    using namespace imsrv::deviceproof;
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> keyContext(
        EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
    assert(keyContext && EVP_PKEY_keygen_init(keyContext.get()) == 1);
    assert(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(keyContext.get(), NID_X9_62_prime256v1) == 1);
    EVP_PKEY* rawKey = nullptr;
    assert(EVP_PKEY_keygen(keyContext.get(), &rawKey) == 1);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(rawKey, EVP_PKEY_free);

    const int derLength = i2d_PUBKEY(key.get(), nullptr);
    assert(derLength > 0);
    Bytes publicDer(static_cast<std::size_t>(derLength));
    unsigned char* derCursor = publicDer.data();
    assert(i2d_PUBKEY(key.get(), &derCursor) == derLength);

    const Bytes session(16, 7);
    Bytes proof = message("token-login", session, "device-1", "access-token");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> signContext(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    assert(EVP_DigestSignInit(signContext.get(), nullptr, EVP_sha256(), nullptr, key.get()) == 1);
    assert(EVP_DigestSignUpdate(signContext.get(), proof.data(), proof.size()) == 1);
    std::size_t signatureLength = 0;
    assert(EVP_DigestSignFinal(signContext.get(), nullptr, &signatureLength) == 1);
    Bytes signature(signatureLength);
    assert(EVP_DigestSignFinal(signContext.get(), signature.data(), &signatureLength) == 1);
    signature.resize(signatureLength);

    assert(verifyP256(publicDer, proof, signature));
    proof.back() ^= 1;
    assert(!verifyP256(publicDer, proof, signature));
    signature.back() ^= 1;
    assert(!verifyP256(publicDer, proof, signature));
    return 0;
}
