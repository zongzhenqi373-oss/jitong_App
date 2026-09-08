#include "crypto/AppCrypto.h"

#include <cassert>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
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

// R2-F04：极简 golden JSON 读取器。只在 "vector" 段内按 "key": "value" 抽取字符串，
// 避免命中上方 finished/key_schedule 段的公式描述。与 client_core 端读的是同一文件。
struct GoldenJson {
    std::string raw;
    std::size_t vecOff = 0;
    bool loaded = false;

    bool load(const char* path)
    {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) return false;
        std::ostringstream ss;
        ss << ifs.rdbuf();
        raw = ss.str();
        vecOff = raw.find("\"vector\"");
        if (vecOff == std::string::npos) vecOff = 0;
        loaded = !raw.empty();
        return loaded;
    }

    std::string str(const std::string& key) const
    {
        const std::string needle = "\"" + key + "\"";
        std::size_t k = raw.find(needle, vecOff);
        if (k == std::string::npos) return {};
        std::size_t colon = raw.find(':', k + needle.size());
        if (colon == std::string::npos) return {};
        std::size_t q1 = raw.find('"', colon + 1);
        std::size_t q2 = (q1 == std::string::npos) ? std::string::npos : raw.find('"', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos) return {};
        return raw.substr(q1 + 1, q2 - q1 - 1);
    }
};

// 与 client_core / Session.cpp 一致的 HKDF info/salt 构造。
Bytes buildSalt(const Bytes& clientNonce, const Bytes& serverNonce)
{
    Bytes concat = clientNonce;
    concat.insert(concat.end(), serverNonce.begin(), serverNonce.end());
    return imsrv::crypto::sha256(concat);
}

Bytes buildInfo(const Bytes& sessionId, const Bytes& transcriptHash)
{
    static const std::string kLabel = "jitong-app-channel-v1";
    Bytes info(kLabel.begin(), kLabel.end());
    info.insert(info.end(), sessionId.begin(), sessionId.end());
    info.insert(info.end(), transcriptHash.begin(), transcriptHash.end());
    return info;
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

    // R2-F04：独立消费三端共享 golden（与 client_core 同一文件）。用服务端原语，从
    // JSON 的固定输入复算密钥调度，验证服务端与客户端的 HKDF 语义逐字节一致。
    {
        GoldenJson gv;
        if (!gv.load(IM_SERVER_GOLDEN_JSON)) {
            std::cerr << "无法读取 golden JSON: " << IM_SERVER_GOLDEN_JSON << "\n";
            return 1;
        }
        const Bytes clientPriv = fromHex(gv.str("client_private"));
        const Bytes serverPub = fromHex(gv.str("server_public"));
        const Bytes clientNonce = fromHex(gv.str("client_nonce"));
        const Bytes serverNonce = fromHex(gv.str("server_nonce"));
        const Bytes sessionId = fromHex(gv.str("session_id"));
        const Bytes transcriptHash = fromHex(gv.str("transcript_hash"));
        const Bytes expectC2S = fromHex(gv.str("client_to_server_key"));
        const Bytes expectS2C = fromHex(gv.str("server_to_client_key"));
        const Bytes expectCFin = fromHex(gv.str("client_finished_key"));
        const Bytes expectSFin = fromHex(gv.str("server_finished_key"));

        // 服务端视角：shared = X25519(server_priv, client_pub)；但 golden 只给了
        // client_priv + server_pub，二者算出的 shared 相同（ECDH 对称性），因此这里
        // 用 client_priv × server_pub 复算，结果应与客户端一致。
        const Bytes shared = x25519SharedSecret(clientPriv, serverPub);
        const Bytes salt = buildSalt(clientNonce, serverNonce);
        const Bytes info = buildInfo(sessionId, transcriptHash);
        const Bytes okm = hkdfSha256(shared, salt, info, 136);

        auto slice = [&](std::size_t off, std::size_t len) {
            return Bytes(okm.begin() + off, okm.begin() + off + len);
        };
        assert(constantTimeEqual(slice(0, 32), expectC2S));
        assert(constantTimeEqual(slice(32, 32), expectS2C));
        assert(constantTimeEqual(slice(72, 32), expectCFin));
        assert(constantTimeEqual(slice(104, 32), expectSFin));

        // 加密帧一致性：sequence=1，nonce = c_nonce_prefix(okm[64:68]) || u64be(1)，
        // AAD = "jitong-app-frame-v1" || u32be(1) || session_id || u64be(1)，
        // 明文 = protocol_type(4B LE, HEARTBEAT_RQ=1010) || "jitong-golden-payload"。
        const Bytes cNoncePrefix = slice(64, 4);
        Bytes gcmNonce = cNoncePrefix;
        for (int i = 7; i >= 0; --i) gcmNonce.push_back(i == 0 ? 1 : 0); // u64be(1)
        static const std::string frameLabel = "jitong-app-frame-v1";
        Bytes aad(frameLabel.begin(), frameLabel.end());
        aad.push_back(0); aad.push_back(0); aad.push_back(0); aad.push_back(1); // u32be(1)
        aad.insert(aad.end(), sessionId.begin(), sessionId.end());
        for (int i = 7; i >= 0; --i) aad.push_back(i == 0 ? 1 : 0); // u64be(1)

        const std::string payloadStr = "jitong-golden-payload";
        Bytes plaintext;
        // protocol_type 小端 4 字节，HEARTBEAT_RQ 数值来自 frame_inner_protocol 描述
        const std::uint32_t innerType = 1010; // DEF_PROT_HEARTBEAT_RQ
        plaintext.push_back(static_cast<std::uint8_t>(innerType & 0xFF));
        plaintext.push_back(static_cast<std::uint8_t>((innerType >> 8) & 0xFF));
        plaintext.push_back(static_cast<std::uint8_t>((innerType >> 16) & 0xFF));
        plaintext.push_back(static_cast<std::uint8_t>((innerType >> 24) & 0xFF));
        plaintext.insert(plaintext.end(), payloadStr.begin(), payloadStr.end());

        const auto enc = aes256GcmEncrypt(expectC2S, gcmNonce, plaintext, aad);
        assert(constantTimeEqual(enc.ciphertext, fromHex(gv.str("frame_ciphertext"))));
        assert(constantTimeEqual(enc.tag, fromHex(gv.str("frame_tag"))));

        std::cout << "golden app-security-v1 vectors consumed by server: OK\n";
    }

    std::cout << "application crypto vectors passed\n";
    return 0;
}
