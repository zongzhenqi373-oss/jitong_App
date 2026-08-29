#include "crypto/AppCrypto.h"

#include <cassert>
#include <cctype>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using imsrv::crypto::Bytes;

Bytes fromHex(const std::string& value)
{
    if (value.size() % 2 != 0) throw std::invalid_argument("odd hex length");
    Bytes result(value.size() / 2);
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto part = value.substr(i * 2, 2);
        result[i] = static_cast<std::uint8_t>(std::stoul(part, nullptr, 16));
    }
    return result;
}

} // namespace

int main()
{
    using namespace imsrv::crypto;

    // RFC 7748 section 6.1: 锁死Android/C++共同使用的X25519原始密钥字节序。
    const Bytes alicePrivate = fromHex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const Bytes alicePublic = fromHex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    const Bytes bobPrivate = fromHex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    const Bytes bobPublic = fromHex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    const Bytes expectedSecret = fromHex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    assert(constantTimeEqual(x25519PublicFromPrivate(alicePrivate), alicePublic));
    assert(constantTimeEqual(x25519PublicFromPrivate(bobPrivate), bobPublic));
    assert(constantTimeEqual(x25519SharedSecret(alicePrivate, bobPublic), expectedSecret));
    assert(constantTimeEqual(x25519SharedSecret(bobPrivate, alicePublic), expectedSecret));

    // RFC 5869 test case 1: HKDF-SHA256跨语言固定向量。
    const Bytes ikm(22, 0x0b);
    const Bytes salt = fromHex("000102030405060708090a0b0c");
    const Bytes info = fromHex("f0f1f2f3f4f5f6f7f8f9");
    const Bytes expectedOkm = fromHex(
        "3cb25f25faacd57a90434f64d0362f2a"
        "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
        "34007208d5b887185865");
    assert(constantTimeEqual(hkdfSha256(ikm, salt, info, 42), expectedOkm));

    // NIST AES-256-GCM公开向量：空AAD、16字节全零明文。
    const Bytes aesKey(32, 0);
    const Bytes nonce(12, 0);
    const Bytes plaintext(16, 0);
    const Bytes expectedCiphertext = fromHex("cea7403d4d606b6e074ec5d3baf39d18");
    const Bytes expectedTag = fromHex("d0d1c8a799996bf0265b98b5d48ab919");
    const auto encrypted = aes256GcmEncrypt(aesKey, nonce, plaintext, {});
    assert(constantTimeEqual(encrypted.ciphertext, expectedCiphertext));
    assert(constantTimeEqual(encrypted.tag, expectedTag));
    const auto decrypted = aes256GcmDecrypt(aesKey, nonce, encrypted.ciphertext, {}, encrypted.tag);
    assert(decrypted && constantTimeEqual(*decrypted, plaintext));
    Bytes tamperedTag = encrypted.tag;
    tamperedTag[0] ^= 0x01;
    assert(!aes256GcmDecrypt(aesKey, nonce, encrypted.ciphertext, {}, tamperedTag));

    const Bytes edPrivate = randomBytes(32);
    const Bytes edPublic = ed25519PublicFromPrivate(edPrivate);
    const Bytes message{'j', 'i', 't', 'o', 'n', 'g'};
    const Bytes signature = ed25519Sign(edPrivate, message);
    assert(ed25519Verify(edPublic, message, signature));
    Bytes changedMessage = message;
    changedMessage[0] ^= 1;
    assert(!ed25519Verify(edPublic, changedMessage, signature));

    auto generated = generateX25519KeyPair();
    assert(generated.privateKey.size() == 32 && generated.publicKey.size() == 32);
    assert(constantTimeEqual(x25519PublicFromPrivate(generated.privateKey), generated.publicKey));
    secureClear(generated.privateKey);
    assert(generated.privateKey.empty());

    std::cout << "application crypto vectors passed\n";
    return 0;
}
