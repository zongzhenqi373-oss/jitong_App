#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace imsrv::crypto {

using Bytes = std::vector<std::uint8_t>;

struct X25519KeyPair {
    Bytes privateKey;
    Bytes publicKey;
};

struct AesGcmResult {
    Bytes ciphertext;
    Bytes tag;
};

Bytes randomBytes(std::size_t size);
X25519KeyPair generateX25519KeyPair();
Bytes x25519PublicFromPrivate(const Bytes& privateKey);
Bytes x25519SharedSecret(const Bytes& privateKey, const Bytes& peerPublicKey);

Bytes ed25519PublicFromPrivate(const Bytes& privateKey);
Bytes ed25519Sign(const Bytes& privateKey, const Bytes& message);
bool ed25519Verify(const Bytes& publicKey, const Bytes& message, const Bytes& signature);

Bytes hmacSha256(const Bytes& key, const Bytes& data);
Bytes sha256(const Bytes& data);
Bytes hkdfSha256(const Bytes& inputKeyMaterial, const Bytes& salt,
                 const Bytes& info, std::size_t outputLength);

AesGcmResult aes256GcmEncrypt(const Bytes& key, const Bytes& nonce,
                              const Bytes& plaintext, const Bytes& aad);
std::optional<Bytes> aes256GcmDecrypt(const Bytes& key, const Bytes& nonce,
                                     const Bytes& ciphertext, const Bytes& aad,
                                     const Bytes& tag);

bool constantTimeEqual(const Bytes& left, const Bytes& right);
void secureClear(Bytes& bytes);

} // namespace imsrv::crypto
