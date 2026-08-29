#include "crypto/AppCrypto.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

namespace imsrv::crypto {
namespace {

constexpr std::size_t X25519_KEY_SIZE = 32;
constexpr std::size_t ED25519_KEY_SIZE = 32;
constexpr std::size_t ED25519_SIGNATURE_SIZE = 64;
constexpr std::size_t SHA256_SIZE = 32;
constexpr std::size_t AES256_KEY_SIZE = 32;
constexpr std::size_t GCM_NONCE_SIZE = 12;
constexpr std::size_t GCM_TAG_SIZE = 16;

using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using PkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using CipherCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

void requireSize(const Bytes& value, std::size_t expected, const char* label)
{
    if (value.size() != expected) throw std::invalid_argument(std::string(label) + " length invalid");
}

PkeyPtr rawPrivateKey(int type, const Bytes& key)
{
    EVP_PKEY* raw = EVP_PKEY_new_raw_private_key(type, nullptr, key.data(), key.size());
    if (!raw) throw std::runtime_error("OpenSSL private key import failed");
    return PkeyPtr(raw, EVP_PKEY_free);
}

PkeyPtr rawPublicKey(int type, const Bytes& key)
{
    EVP_PKEY* raw = EVP_PKEY_new_raw_public_key(type, nullptr, key.data(), key.size());
    if (!raw) throw std::runtime_error("OpenSSL public key import failed");
    return PkeyPtr(raw, EVP_PKEY_free);
}

Bytes publicFromPrivate(int type, const Bytes& privateKey)
{
    auto key = rawPrivateKey(type, privateKey);
    std::size_t size = 0;
    if (EVP_PKEY_get_raw_public_key(key.get(), nullptr, &size) != 1)
        throw std::runtime_error("OpenSSL public key length failed");
    Bytes result(size);
    if (EVP_PKEY_get_raw_public_key(key.get(), result.data(), &size) != 1)
        throw std::runtime_error("OpenSSL public key export failed");
    result.resize(size);
    return result;
}

} // namespace

Bytes randomBytes(std::size_t size)
{
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("random size too large");
    Bytes result(size);
    if (size != 0 && RAND_bytes(result.data(), static_cast<int>(size)) != 1)
        throw std::runtime_error("OpenSSL secure random failed");
    return result;
}

X25519KeyPair generateX25519KeyPair()
{
    PkeyCtxPtr context(EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr), EVP_PKEY_CTX_free);
    if (!context || EVP_PKEY_keygen_init(context.get()) != 1)
        throw std::runtime_error("X25519 keygen init failed");
    EVP_PKEY* generated = nullptr;
    if (EVP_PKEY_keygen(context.get(), &generated) != 1)
        throw std::runtime_error("X25519 keygen failed");
    PkeyPtr key(generated, EVP_PKEY_free);
    X25519KeyPair result{Bytes(X25519_KEY_SIZE), Bytes(X25519_KEY_SIZE)};
    std::size_t privateSize = result.privateKey.size();
    std::size_t publicSize = result.publicKey.size();
    if (EVP_PKEY_get_raw_private_key(key.get(), result.privateKey.data(), &privateSize) != 1 ||
        EVP_PKEY_get_raw_public_key(key.get(), result.publicKey.data(), &publicSize) != 1)
        throw std::runtime_error("X25519 key export failed");
    result.privateKey.resize(privateSize);
    result.publicKey.resize(publicSize);
    return result;
}

Bytes x25519PublicFromPrivate(const Bytes& privateKey)
{
    requireSize(privateKey, X25519_KEY_SIZE, "X25519 private key");
    return publicFromPrivate(EVP_PKEY_X25519, privateKey);
}

Bytes x25519SharedSecret(const Bytes& privateKey, const Bytes& peerPublicKey)
{
    requireSize(privateKey, X25519_KEY_SIZE, "X25519 private key");
    requireSize(peerPublicKey, X25519_KEY_SIZE, "X25519 public key");
    auto local = rawPrivateKey(EVP_PKEY_X25519, privateKey);
    auto peer = rawPublicKey(EVP_PKEY_X25519, peerPublicKey);
    PkeyCtxPtr context(EVP_PKEY_CTX_new(local.get(), nullptr), EVP_PKEY_CTX_free);
    if (!context || EVP_PKEY_derive_init(context.get()) != 1 ||
        EVP_PKEY_derive_set_peer(context.get(), peer.get()) != 1)
        throw std::runtime_error("X25519 derive init failed");
    std::size_t size = 0;
    if (EVP_PKEY_derive(context.get(), nullptr, &size) != 1)
        throw std::runtime_error("X25519 derive length failed");
    Bytes secret(size);
    if (EVP_PKEY_derive(context.get(), secret.data(), &size) != 1)
        throw std::runtime_error("X25519 derive failed");
    secret.resize(size);
    if (std::all_of(secret.begin(), secret.end(), [](std::uint8_t byte) { return byte == 0; })) {
        secureClear(secret);
        throw std::runtime_error("X25519 rejected all-zero shared secret");
    }
    return secret;
}

Bytes ed25519PublicFromPrivate(const Bytes& privateKey)
{
    requireSize(privateKey, ED25519_KEY_SIZE, "Ed25519 private key");
    return publicFromPrivate(EVP_PKEY_ED25519, privateKey);
}

Bytes ed25519Sign(const Bytes& privateKey, const Bytes& message)
{
    requireSize(privateKey, ED25519_KEY_SIZE, "Ed25519 private key");
    auto key = rawPrivateKey(EVP_PKEY_ED25519, privateKey);
    MdCtxPtr context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1)
        throw std::runtime_error("Ed25519 sign init failed");
    Bytes signature(ED25519_SIGNATURE_SIZE);
    std::size_t size = signature.size();
    if (EVP_DigestSign(context.get(), signature.data(), &size,
                       message.data(), message.size()) != 1)
        throw std::runtime_error("Ed25519 sign failed");
    signature.resize(size);
    return signature;
}

bool ed25519Verify(const Bytes& publicKey, const Bytes& message, const Bytes& signature)
{
    requireSize(publicKey, ED25519_KEY_SIZE, "Ed25519 public key");
    if (signature.size() != ED25519_SIGNATURE_SIZE) return false;
    auto key = rawPublicKey(EVP_PKEY_ED25519, publicKey);
    MdCtxPtr context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1)
        throw std::runtime_error("Ed25519 verify init failed");
    return EVP_DigestVerify(context.get(), signature.data(), signature.size(),
                            message.data(), message.size()) == 1;
}

Bytes hmacSha256(const Bytes& key, const Bytes& data)
{
    Bytes result(SHA256_SIZE);
    unsigned int size = static_cast<unsigned int>(result.size());
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
              data.data(), data.size(), result.data(), &size))
        throw std::runtime_error("HMAC-SHA256 failed");
    result.resize(size);
    return result;
}

Bytes sha256(const Bytes& data)
{
    MdCtxPtr context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(context.get(), data.data(), data.size()) != 1) {
        throw std::runtime_error("SHA-256 init failed");
    }
    Bytes result(SHA256_SIZE);
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(context.get(), result.data(), &size) != 1)
        throw std::runtime_error("SHA-256 finalize failed");
    result.resize(size);
    return result;
}

Bytes hkdfSha256(const Bytes& inputKeyMaterial, const Bytes& salt,
                 const Bytes& info, std::size_t outputLength)
{
    if (outputLength > 255 * SHA256_SIZE) throw std::invalid_argument("HKDF output too large");
    const Bytes effectiveSalt = salt.empty() ? Bytes(SHA256_SIZE, 0) : salt;
    Bytes pseudoRandomKey = hmacSha256(effectiveSalt, inputKeyMaterial);
    Bytes output;
    output.reserve(outputLength);
    Bytes previous;
    for (std::uint16_t counter = 1; output.size() < outputLength; ++counter) {
        Bytes blockInput;
        blockInput.reserve(previous.size() + info.size() + 1);
        blockInput.insert(blockInput.end(), previous.begin(), previous.end());
        blockInput.insert(blockInput.end(), info.begin(), info.end());
        blockInput.push_back(static_cast<std::uint8_t>(counter));
        Bytes block = hmacSha256(pseudoRandomKey, blockInput);
        const std::size_t needed = std::min(block.size(), outputLength - output.size());
        output.insert(output.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(needed));
        secureClear(previous);
        previous = std::move(block);
    }
    secureClear(previous);
    secureClear(pseudoRandomKey);
    return output;
}

AesGcmResult aes256GcmEncrypt(const Bytes& key, const Bytes& nonce,
                              const Bytes& plaintext, const Bytes& aad)
{
    requireSize(key, AES256_KEY_SIZE, "AES-256 key");
    requireSize(nonce, GCM_NONCE_SIZE, "AES-GCM nonce");
    CipherCtxPtr context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!context || EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce.data()) != 1)
        throw std::runtime_error("AES-GCM encrypt init failed");
    int ignored = 0;
    if (!aad.empty() && EVP_EncryptUpdate(context.get(), nullptr, &ignored, aad.data(),
                                          static_cast<int>(aad.size())) != 1)
        throw std::runtime_error("AES-GCM AAD failed");
    // 为Final保留有效输出地址，即使明文为空也不对nullptr做指针运算。
    AesGcmResult result{Bytes(plaintext.size() + EVP_MAX_BLOCK_LENGTH), Bytes(GCM_TAG_SIZE)};
    int written = 0;
    if (!plaintext.empty() && EVP_EncryptUpdate(context.get(), result.ciphertext.data(), &written,
                                                plaintext.data(), static_cast<int>(plaintext.size())) != 1)
        throw std::runtime_error("AES-GCM encrypt failed");
    int finalWritten = 0;
    if (EVP_EncryptFinal_ex(context.get(), result.ciphertext.data() + written, &finalWritten) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, GCM_TAG_SIZE, result.tag.data()) != 1)
        throw std::runtime_error("AES-GCM finalize failed");
    result.ciphertext.resize(static_cast<std::size_t>(written + finalWritten));
    return result;
}

std::optional<Bytes> aes256GcmDecrypt(const Bytes& key, const Bytes& nonce,
                                     const Bytes& ciphertext, const Bytes& aad,
                                     const Bytes& tag)
{
    requireSize(key, AES256_KEY_SIZE, "AES-256 key");
    requireSize(nonce, GCM_NONCE_SIZE, "AES-GCM nonce");
    requireSize(tag, GCM_TAG_SIZE, "AES-GCM tag");
    CipherCtxPtr context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!context || EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_DecryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce.data()) != 1)
        throw std::runtime_error("AES-GCM decrypt init failed");
    int ignored = 0;
    if (!aad.empty() && EVP_DecryptUpdate(context.get(), nullptr, &ignored, aad.data(),
                                          static_cast<int>(aad.size())) != 1)
        throw std::runtime_error("AES-GCM AAD failed");
    Bytes plaintext(ciphertext.size() + EVP_MAX_BLOCK_LENGTH);
    int written = 0;
    if (!ciphertext.empty() && EVP_DecryptUpdate(context.get(), plaintext.data(), &written,
                                                 ciphertext.data(), static_cast<int>(ciphertext.size())) != 1)
        throw std::runtime_error("AES-GCM decrypt failed");
    Bytes mutableTag = tag;
    if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, GCM_TAG_SIZE, mutableTag.data()) != 1)
        throw std::runtime_error("AES-GCM tag setup failed");
    int finalWritten = 0;
    if (EVP_DecryptFinal_ex(context.get(), plaintext.data() + written, &finalWritten) != 1) {
        secureClear(plaintext);
        return std::nullopt;
    }
    plaintext.resize(static_cast<std::size_t>(written + finalWritten));
    return plaintext;
}

bool constantTimeEqual(const Bytes& left, const Bytes& right)
{
    return left.size() == right.size() &&
        (left.empty() || CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0);
}

void secureClear(Bytes& bytes)
{
    if (!bytes.empty()) OPENSSL_cleanse(bytes.data(), bytes.size());
    bytes.clear();
    bytes.shrink_to_fit();
}

} // namespace imsrv::crypto
