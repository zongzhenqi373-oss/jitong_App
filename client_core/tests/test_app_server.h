#pragma once
// 测试用「服务端侧」应用层安全通道（header-only，供 test_integration 复用）。
//
// 作用：让假服务端具备与 im_server Session.cpp 相同的握手与加密行为。
// P4 之后客户端强制握手，任何不具备握手能力的假服务端都会让集成测试超时失败。
//
// 与客户端共用同一套字节级约定（label、长度前缀、HKDF 切片、AAD），
// 只是**方向相反**：解密用 clientToServerKey，加密用 serverToClientKey。

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "client_core/Protocol.h"
#include "im.pb.h"

namespace im {
namespace test {

class TestAppServerCrypto {
public:
    using Bytes = std::vector<unsigned char>;

    bool initIdentity()
    {
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        if (!ctx) return false;
        const bool ok = EVP_PKEY_keygen_init(ctx) > 0 && EVP_PKEY_keygen(ctx, &m_identity) > 0;
        EVP_PKEY_CTX_free(ctx);
        return ok && m_identity;
    }

    Bytes identityPublic()
    {
        Bytes out(32, 0);
        std::size_t len = 32;
        if (EVP_PKEY_get_raw_public_key(m_identity, out.data(), &len) <= 0) return {};
        return out;
    }

    /** 处理 AppClientHello，产出 AppServerHello（含 Ed25519 签名）。 */
    bool handleClientHello(const std::string& clientPayload, std::string& outServerHello)
    {
        EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
        EVP_PKEY* pkey = nullptr;
        bool ok = kctx && EVP_PKEY_keygen_init(kctx) > 0 && EVP_PKEY_keygen(kctx, &pkey) > 0;
        if (kctx) EVP_PKEY_CTX_free(kctx);
        if (!ok || !pkey) return false;

        m_serverPriv.assign(32, 0);
        m_serverPub.assign(32, 0);
        std::size_t pl = 32;
        std::size_t ul = 32;
        ok = EVP_PKEY_get_raw_private_key(pkey, m_serverPriv.data(), &pl) > 0 &&
             EVP_PKEY_get_raw_public_key(pkey, m_serverPub.data(), &ul) > 0;
        EVP_PKEY_free(pkey);
        if (!ok) return false;

        m_serverNonce.assign(32, 0);
        m_sessionId.assign(16, 0);
        RAND_bytes(m_serverNonce.data(), 32);
        RAND_bytes(m_sessionId.data(), 16);

        im::proto::AppClientHello hello;
        if (!hello.ParseFromString(clientPayload)) return false;
        if (hello.client_ephemeral_public_key().size() != 32 ||
            hello.client_nonce().size() != 32) {
            std::cerr << "[SRV] bad field size pub="
                      << hello.client_ephemeral_public_key().size()
                      << " nonce=" << hello.client_nonce().size() << std::endl;
            return false;
        }
        m_clientPub.assign(hello.client_ephemeral_public_key().begin(),
                           hello.client_ephemeral_public_key().end());
        m_clientNonce.assign(hello.client_nonce().begin(), hello.client_nonce().end());
        m_clientPayload = clientPayload;

        std::string t;
        t.append("jitong-app-handshake-v1");
        appendU32Be(t, static_cast<std::uint32_t>(m_clientPayload.size()));
        t.append(m_clientPayload);
        appendU32Be(t, im::proto::APP_SECURITY_VERSION);
        t.append(reinterpret_cast<const char*>(m_serverPub.data()), m_serverPub.size());
        t.append(reinterpret_cast<const char*>(m_serverNonce.data()), m_serverNonce.size());
        t.append(reinterpret_cast<const char*>(m_sessionId.data()), m_sessionId.size());
        appendU32Be(t, m_keyId);
        appendU32Be(t, im::proto::APP_CIPHER_SUITE_V1);

        Bytes sig;
        {
            EVP_MD_CTX* md = EVP_MD_CTX_new();
            std::size_t len = 0;
            bool sok = md &&
                       EVP_DigestSignInit(md, nullptr, nullptr, nullptr, m_identity) > 0 &&
                       EVP_DigestSign(md, nullptr, &len,
                                      reinterpret_cast<const unsigned char*>(t.data()),
                                      t.size()) > 0;
            if (sok) {
                sig.assign(len, 0);
                sok = EVP_DigestSign(md, sig.data(), &len,
                                     reinterpret_cast<const unsigned char*>(t.data()),
                                     t.size()) > 0;
            }
            if (md) EVP_MD_CTX_free(md);
            if (!sok) return false;
        }

        im::proto::AppServerHello sh;
        sh.set_version(im::proto::APP_SECURITY_VERSION);
        sh.set_server_ephemeral_public_key(m_serverPub.data(), m_serverPub.size());
        sh.set_server_nonce(m_serverNonce.data(), m_serverNonce.size());
        sh.set_session_id(m_sessionId.data(), m_sessionId.size());
        sh.set_key_id(m_keyId);
        sh.set_signature(sig.data(), sig.size());
        sh.set_cipher_suite(im::proto::APP_CIPHER_X25519_ED25519_HKDF_SHA256_AES_256_GCM);
        const std::string serverPayload = sh.SerializeAsString();
        m_serverPayload = serverPayload;

        std::string ti;
        appendU32Be(ti, static_cast<std::uint32_t>(m_clientPayload.size()));
        ti.append(m_clientPayload);
        appendU32Be(ti, static_cast<std::uint32_t>(m_serverPayload.size()));
        ti.append(m_serverPayload);
        unsigned char digest[32] = {};
        SHA256(reinterpret_cast<const unsigned char*>(ti.data()), ti.size(), digest);
        m_transcriptHash.assign(digest, digest + 32);

        if (!deriveKeys()) {
            std::cerr << "[SRV] deriveKeys FAILED" << std::endl;
            return false;
        }
        outServerHello = serverPayload;
        return true;
    }

    /** 处理 AppFinished，产出服务端 AppFinished。 */
    bool handleClientFinished(const std::string& finishedPayload, std::string& outServerFinished)
    {
        im::proto::AppFinished cf;
        if (!cf.ParseFromString(finishedPayload)) return false;
        if (cf.verify_data().size() != 32) return false;

        std::string expectInput(reinterpret_cast<const char*>(m_transcriptHash.data()),
                                m_transcriptHash.size());
        unsigned char expect[32] = {};
        unsigned int len = 0;
        HMAC(EVP_sha256(), m_clientFinishedKey.data(),
             static_cast<int>(m_clientFinishedKey.size()),
             reinterpret_cast<const unsigned char*>(expectInput.data()), expectInput.size(),
             expect, &len);
        if (CRYPTO_memcmp(expect, cf.verify_data().data(), 32) != 0) return false;

        std::string input(expectInput);
        input.append(cf.verify_data());
        unsigned char out[32] = {};
        HMAC(EVP_sha256(), m_serverFinishedKey.data(),
             static_cast<int>(m_serverFinishedKey.size()),
             reinterpret_cast<const unsigned char*>(input.data()), input.size(), out, &len);

        im::proto::AppFinished sf;
        sf.set_verify_data(out, 32);
        outServerFinished = sf.SerializeAsString();
        m_established = true;
        return true;
    }

    bool established() const { return m_established; }

    /** 解密 C→S 帧。 */
    bool decryptFromClient(const std::string& framePayload, im::proto::protType& outType,
                           std::string& outPayload)
    {
        im::proto::AppEncryptedFrame f;
        if (!f.ParseFromString(framePayload)) return false;
        if (f.version() != 1 || f.tag().size() != 16) return false;
        if (f.session_id().size() != 16 ||
            CRYPTO_memcmp(f.session_id().data(), m_sessionId.data(), 16) != 0) return false;
        if (f.sequence() == 0 || f.sequence() != m_receiveSeq + 1) return false;
        if (f.ciphertext().size() < 4) return false;

        const std::uint64_t seq = f.sequence();
        unsigned char nonce[12] = {};
        std::memcpy(nonce, m_clientNoncePrefix.data(), 4);
        {
            std::string s;
            appendU64Be(s, seq);
            std::memcpy(nonce + 4, s.data(), 8);
        }
        std::string aad;
        aad.append("jitong-app-frame-v1");
        appendU32Be(aad, im::proto::APP_SECURITY_VERSION);
        aad.append(reinterpret_cast<const char*>(m_sessionId.data()), m_sessionId.size());
        appendU64Be(aad, seq);

        std::string plaintext;
        if (!gcmDecrypt(m_clientToServerKey.data(), nonce, aad, f.ciphertext(),
                        reinterpret_cast<const unsigned char*>(f.tag().data()), plaintext)) {
            return false;
        }
        outType = im::proto::decodeType32(plaintext.data());
        outPayload.assign(plaintext.data() + 4, plaintext.size() - 4);
        m_receiveSeq = seq;
        return true;
    }

    /** 加密 S→C 帧。 */
    bool encryptToClient(im::proto::protType type, const std::string& payload,
                         std::string& outFramePayload)
    {
        if (m_sendSeq == UINT64_MAX) return false;
        const std::uint64_t seq = ++m_sendSeq;

        std::string plaintext;
        plaintext.resize(4 + payload.size());
        im::proto::encodeType32(type, plaintext.data());
        if (!payload.empty()) std::memcpy(plaintext.data() + 4, payload.data(), payload.size());

        unsigned char nonce[12] = {};
        std::memcpy(nonce, m_serverNoncePrefix.data(), 4);
        {
            std::string s;
            appendU64Be(s, seq);
            std::memcpy(nonce + 4, s.data(), 8);
        }
        std::string aad;
        aad.append("jitong-app-frame-v1");
        appendU32Be(aad, im::proto::APP_SECURITY_VERSION);
        aad.append(reinterpret_cast<const char*>(m_sessionId.data()), m_sessionId.size());
        appendU64Be(aad, seq);

        std::string ciphertext;
        unsigned char tag[16] = {};
        if (!gcmEncrypt(m_serverToClientKey.data(), nonce, aad, plaintext, ciphertext, tag)) {
            return false;
        }

        im::proto::AppEncryptedFrame f;
        f.set_version(1);
        f.set_session_id(m_sessionId.data(), m_sessionId.size());
        f.set_sequence(seq);
        f.set_ciphertext(ciphertext.data(), ciphertext.size());
        f.set_tag(tag, 16);
        outFramePayload = f.SerializeAsString();
        return true;
    }

    ~TestAppServerCrypto() { if (m_identity) EVP_PKEY_free(m_identity); }

private:
    static void appendU32Be(std::string& out, std::uint32_t v)
    {
        out.push_back(static_cast<char>((v >> 24) & 0xFF));
        out.push_back(static_cast<char>((v >> 16) & 0xFF));
        out.push_back(static_cast<char>((v >> 8) & 0xFF));
        out.push_back(static_cast<char>(v & 0xFF));
    }

    static void appendU64Be(std::string& out, std::uint64_t v)
    {
        for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
    }

    bool deriveKeys()
    {
        EVP_PKEY* priv = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
                                                     m_serverPriv.data(), m_serverPriv.size());
        EVP_PKEY* peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
                                                    m_clientPub.data(), m_clientPub.size());
        if (!priv || !peer) {
            if (priv) EVP_PKEY_free(priv);
            if (peer) EVP_PKEY_free(peer);
            return false;
        }
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(priv, nullptr);
        std::size_t len = 0;
        bool ok = ctx && EVP_PKEY_derive_init(ctx) > 0 &&
                  EVP_PKEY_derive_set_peer(ctx, peer) > 0 &&
                  EVP_PKEY_derive(ctx, nullptr, &len) > 0;
        Bytes secret;
        if (ok) {
            secret.assign(len, 0);
            ok = EVP_PKEY_derive(ctx, secret.data(), &len) > 0;
        }
        if (ctx) EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(priv);
        EVP_PKEY_free(peer);
        if (!ok) return false;

        std::string saltInput;
        saltInput.append(reinterpret_cast<const char*>(m_clientNonce.data()), m_clientNonce.size());
        saltInput.append(reinterpret_cast<const char*>(m_serverNonce.data()), m_serverNonce.size());
        unsigned char salt[32] = {};
        SHA256(reinterpret_cast<const unsigned char*>(saltInput.data()), saltInput.size(), salt);

        Bytes info;
        const char* label = "jitong-app-channel-v1";
        info.insert(info.end(), label, label + std::strlen(label));
        info.insert(info.end(), m_sessionId.begin(), m_sessionId.end());
        info.insert(info.end(), m_transcriptHash.begin(), m_transcriptHash.end());

        EVP_PKEY_CTX* hctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
        ok = hctx && EVP_PKEY_derive_init(hctx) > 0 &&
             EVP_PKEY_CTX_set_hkdf_md(hctx, EVP_sha256()) > 0 &&
             EVP_PKEY_CTX_set1_hkdf_salt(hctx, salt, 32) > 0 &&
             EVP_PKEY_CTX_add1_hkdf_info(hctx, info.data(), static_cast<int>(info.size())) > 0 &&
             EVP_PKEY_CTX_set1_hkdf_key(hctx, secret.data(), static_cast<int>(secret.size())) > 0;
        Bytes material;
        if (ok) {
            material.assign(136, 0);
            std::size_t outLen = 136;
            ok = EVP_PKEY_derive(hctx, material.data(), &outLen) > 0 && outLen == 136;
        }
        if (hctx) EVP_PKEY_CTX_free(hctx);
        if (!ok) return false;

        auto slice = [&material](std::size_t b, std::size_t e) {
            return Bytes(material.begin() + static_cast<long>(b),
                         material.begin() + static_cast<long>(e));
        };
        m_clientToServerKey = slice(0, 32);
        m_serverToClientKey = slice(32, 64);
        m_clientNoncePrefix = slice(64, 68);
        m_serverNoncePrefix = slice(68, 72);
        m_clientFinishedKey = slice(72, 104);
        m_serverFinishedKey = slice(104, 136);
        return true;
    }

    static bool gcmEncrypt(const unsigned char* key, const unsigned char* nonce,
                           const std::string& aad, const std::string& plaintext,
                           std::string& ciphertext, unsigned char tag[16])
    {
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return false;
        int len = 0;
        bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
                  EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
                  EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, nonce) == 1;
        if (ok && !aad.empty()) {
            ok = EVP_EncryptUpdate(ctx, nullptr, &len,
                                   reinterpret_cast<const unsigned char*>(aad.data()),
                                   static_cast<int>(aad.size())) == 1;
        }
        if (ok) {
            ciphertext.assign(plaintext.size(), '\0');
            ok = EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(ciphertext.data()), &len,
                                   reinterpret_cast<const unsigned char*>(plaintext.data()),
                                   static_cast<int>(plaintext.size())) == 1;
        }
        if (ok) {
            int fl = 0;
            ok = EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(ciphertext.data()) + len,
                                     &fl) == 1;
        }
        if (ok) ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
        EVP_CIPHER_CTX_free(ctx);
        return ok;
    }

    static bool gcmDecrypt(const unsigned char* key, const unsigned char* nonce,
                           const std::string& aad, const std::string& ciphertext,
                           const unsigned char* tag, std::string& plaintext)
    {
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return false;
        int len = 0;
        bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
                  EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
                  EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, nonce) == 1;
        // AAD 必须在密文之前喂入，否则 GCM 认证必然失败
        if (ok && !aad.empty()) {
            ok = EVP_DecryptUpdate(ctx, nullptr, &len,
                                   reinterpret_cast<const unsigned char*>(aad.data()),
                                   static_cast<int>(aad.size())) == 1;
        }
        plaintext.assign(ciphertext.size(), '\0');
        if (ok && !ciphertext.empty()) {
            ok = EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(plaintext.data()), &len,
                                   reinterpret_cast<const unsigned char*>(ciphertext.data()),
                                   static_cast<int>(ciphertext.size())) == 1;
        }
        if (ok) {
            std::vector<unsigned char> tagCopy(tag, tag + 16);
            ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tagCopy.data()) == 1;
        }
        if (ok) {
            int fl = 0;
            ok = EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(plaintext.data()) + len,
                                     &fl) == 1;
        }
        EVP_CIPHER_CTX_free(ctx);
        return ok;
    }

    EVP_PKEY* m_identity = nullptr;
    std::uint32_t m_keyId = 1;

    Bytes m_serverPriv;
    Bytes m_serverPub;
    Bytes m_serverNonce;
    Bytes m_sessionId;
    Bytes m_clientPub;
    Bytes m_clientNonce;

    std::string m_clientPayload;
    std::string m_serverPayload;
    Bytes m_transcriptHash;

    Bytes m_clientToServerKey;
    Bytes m_serverToClientKey;
    Bytes m_clientNoncePrefix;
    Bytes m_serverNoncePrefix;
    Bytes m_clientFinishedKey;
    Bytes m_serverFinishedKey;

    std::uint64_t m_sendSeq = 0;
    std::uint64_t m_receiveSeq = 0;
    bool m_established = false;
};

} // namespace test
} // namespace im
