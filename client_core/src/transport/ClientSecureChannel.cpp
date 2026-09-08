#include "transport/ClientSecureChannel.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "im.pb.h"

namespace im {
namespace transport {
namespace {

// 三个 label 的**字节内容**必须与服务端一致，改动必须三端同步
constexpr const char kHandshakeLabel[] = "jitong-app-handshake-v1"; // 22B
constexpr const char kChannelLabel[] = "jitong-app-channel-v1";     // 21B
constexpr const char kFrameLabel[] = "jitong-app-frame-v1";         // 19B

constexpr std::size_t kKeyLen = 32;
constexpr std::size_t kNonceLen = 32;
constexpr std::size_t kRandomIdLen = 16;
constexpr std::size_t kSessionIdLen = 16;
constexpr std::size_t kSignatureLen = 64;
constexpr std::size_t kFinishedLen = 32;
constexpr std::size_t kTagLen = 16;
constexpr std::size_t kGcmNonceLen = 12;
constexpr std::size_t kHkdfOutputLen = 136;

void appendU32Be(std::string& out, std::uint32_t v)
{
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}

void appendU64Be(std::string& out, std::uint64_t v)
{
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
    }
}

bool sha256Of(const std::string& data, unsigned char out[SHA256_DIGEST_LENGTH])
{
    return SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), out) != nullptr;
}

bool hmacSha256(const void* key, std::size_t keyLen, const std::string& data,
                unsigned char out[SHA256_DIGEST_LENGTH])
{
    unsigned int len = 0;
    return HMAC(EVP_sha256(), key, static_cast<int>(keyLen),
                reinterpret_cast<const unsigned char*>(data.data()), data.size(), out, &len) != nullptr &&
           len == SHA256_DIGEST_LENGTH;
}

bool constantTimeEqual(const unsigned char* a, const unsigned char* b, std::size_t n)
{
    return n == 0 || CRYPTO_memcmp(a, b, n) == 0;
}

bool x25519Generate(ClientSecureChannel::Bytes& priv, ClientSecureChannel::Bytes& pub)
{
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (!ctx) return false;
    EVP_PKEY* pkey = nullptr;
    const bool ok = EVP_PKEY_keygen_init(ctx) > 0 && EVP_PKEY_keygen(ctx, &pkey) > 0;
    EVP_PKEY_CTX_free(ctx);
    if (!ok || !pkey) {
        if (pkey) EVP_PKEY_free(pkey);
        return false;
    }

    priv.assign(kKeyLen, 0);
    pub.assign(kKeyLen, 0);
    std::size_t privLen = kKeyLen;
    std::size_t pubLen = kKeyLen;
    const bool got = EVP_PKEY_get_raw_private_key(pkey, priv.data(), &privLen) > 0 &&
                     EVP_PKEY_get_raw_public_key(pkey, pub.data(), &pubLen) > 0;
    EVP_PKEY_free(pkey);
    return got && privLen == kKeyLen && pubLen == kKeyLen;
}

bool x25519Derive(const ClientSecureChannel::Bytes& priv,
                  const ClientSecureChannel::Bytes& peerPub,
                  ClientSecureChannel::Bytes& secret)
{
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv.data(), priv.size());
    EVP_PKEY* peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peerPub.data(), peerPub.size());
    if (!pkey || !peer) {
        if (pkey) EVP_PKEY_free(pkey);
        if (peer) EVP_PKEY_free(peer);
        return false;
    }

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pkey, nullptr);
    bool ok = ctx && EVP_PKEY_derive_init(ctx) > 0 && EVP_PKEY_derive_set_peer(ctx, peer) > 0;
    std::size_t len = 0;
    if (ok) ok = EVP_PKEY_derive(ctx, nullptr, &len) > 0;
    if (ok) {
        secret.assign(len, 0);
        ok = EVP_PKEY_derive(ctx, secret.data(), &len) > 0;
    }
    if (ctx) EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    EVP_PKEY_free(peer);
    return ok && !secret.empty();
}

bool ed25519Verify(const ClientSecureChannel::Bytes& pub, const std::string& msg,
                   const ClientSecureChannel::Bytes& sig)
{
    if (pub.size() != kKeyLen || sig.size() != kSignatureLen) return false;

    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub.data(), pub.size());
    if (!pkey) return false;

    EVP_MD_CTX* md = EVP_MD_CTX_new();
    bool ok = false;
    if (md) {
        // 签名: (ctx, pctx, type, e, pkey) —— Ed25519 为 one-shot，type 传 nullptr
        ok = EVP_DigestVerifyInit(md, nullptr, nullptr, nullptr, pkey) > 0 &&
             EVP_DigestVerify(md, sig.data(), sig.size(),
                              reinterpret_cast<const unsigned char*>(msg.data()), msg.size()) == 1;
        EVP_MD_CTX_free(md);
    }
    EVP_PKEY_free(pkey);
    return ok;
}

bool hkdfSha256(const ClientSecureChannel::Bytes& ikm, const ClientSecureChannel::Bytes& salt,
                const ClientSecureChannel::Bytes& info, std::size_t outLen,
                ClientSecureChannel::Bytes& out)
{
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!ctx) return false;

    const bool ok =
        EVP_PKEY_derive_init(ctx) > 0 &&
        EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) > 0 &&
        EVP_PKEY_CTX_set1_hkdf_salt(ctx, salt.data(), static_cast<int>(salt.size())) > 0 &&
        EVP_PKEY_CTX_add1_hkdf_info(ctx, info.data(), static_cast<int>(info.size())) > 0 &&
        EVP_PKEY_CTX_set1_hkdf_key(ctx, ikm.data(), static_cast<int>(ikm.size())) > 0;

    bool derived = false;
    if (ok) {
        out.assign(outLen, 0);
        std::size_t len = outLen;
        derived = EVP_PKEY_derive(ctx, out.data(), &len) > 0 && len == outLen;
    }
    EVP_PKEY_CTX_free(ctx);
    return derived;
}

bool aesGcmEncrypt(const unsigned char* key, const unsigned char* nonce, const std::string& aad,
                   const std::string& plaintext, std::string& ciphertext,
                   unsigned char tag[kTagLen])
{
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    int len = 0;
    bool ok =
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kGcmNonceLen), nullptr) == 1 &&
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
        int finalLen = 0;
        ok = EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(ciphertext.data()) + len,
                                 &finalLen) == 1;
    }
    if (ok) {
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagLen), tag) == 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

bool aesGcmDecrypt(const unsigned char* key, const unsigned char* nonce, const std::string& aad,
                   const std::string& ciphertext, const unsigned char* tag, std::string& plaintext)
{
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    int len = 0;
    bool ok =
        EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kGcmNonceLen), nullptr) == 1 &&
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, nonce) == 1;

    // 顺序必须是：Init → AAD → 密文 → 设置 tag → Final。
    // AAD 必须在密文之前喂入，否则 GCM 认证必然失败。
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
        std::vector<unsigned char> tagCopy(tag, tag + kTagLen);
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTagLen),
                                 tagCopy.data()) == 1;
    }
    if (ok) {
        int finalLen = 0;
        ok = EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(plaintext.data()) + len,
                                 &finalLen) == 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

} // namespace

// ---------------- 生命周期 ----------------

ClientSecureChannel::ClientSecureChannel() = default;

ClientSecureChannel::~ClientSecureChannel()
{
    reset();
}

void ClientSecureChannel::secureClear(Bytes& b)
{
    if (!b.empty()) OPENSSL_cleanse(b.data(), b.size());
    b.clear();
}

void ClientSecureChannel::secureClear(std::string& s)
{
    if (!s.empty()) OPENSSL_cleanse(s.data(), s.size());
    s.clear();
}

void ClientSecureChannel::reset()
{
    secureClear(m_clientPriv);
    secureClear(m_clientPub);
    secureClear(m_clientNonce);
    secureClear(m_randomId);
    secureClear(m_clientPayload);
    secureClear(m_serverPub);
    secureClear(m_serverNonce);
    secureClear(m_sessionId);
    secureClear(m_serverPayload);
    secureClear(m_transcriptHash);
    secureClear(m_clientToServerKey);
    secureClear(m_serverToClientKey);
    secureClear(m_clientNoncePrefix);
    secureClear(m_serverNoncePrefix);
    secureClear(m_clientFinishedKey);
    secureClear(m_serverFinishedKey);

    m_keyId = 0;
    m_sendSequence = 0;
    m_receiveSequence = 0;
    m_status = Status::NotStarted;
    m_lastError = Error::None;
}

void ClientSecureChannel::fail(Error e)
{
    m_lastError = e;
    m_status = Status::Failed;
}

void ClientSecureChannel::setTrustedIdentityKeys(std::map<KeyId, Bytes> keys)
{
    m_identityKeys = std::move(keys);
}

void ClientSecureChannel::setDeterministicTestVector(const Bytes& clientPrivateKey,
                                                     const Bytes& clientNonce,
                                                     const Bytes& randomId)
{
    m_testPriv = clientPrivateKey;
    m_testNonce = clientNonce;
    m_testRandomId = randomId;
}

// ---------------- 步骤 1：ClientHello ----------------

bool ClientSecureChannel::buildClientHello(std::string& outPayload)
{
    reset();

    if (m_testPriv.empty()) {
        if (!x25519Generate(m_clientPriv, m_clientPub)) {
            fail(Error::CryptoInit);
            return false;
        }
    } else {
        // 测试向量：由给定的 X25519 私钥派生对应公钥（reset() 已清空 m_clientPriv，这里回填）
        m_clientPriv = m_testPriv;
        EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
                                                      m_clientPriv.data(),
                                                      static_cast<std::size_t>(m_clientPriv.size()));
        if (!pkey) {
            fail(Error::CryptoInit);
            return false;
        }
        m_clientPub.assign(kKeyLen, 0);
        std::size_t len = kKeyLen;
        const bool ok = EVP_PKEY_get_raw_public_key(pkey, m_clientPub.data(), &len) > 0;
        EVP_PKEY_free(pkey);
        if (!ok) {
            fail(Error::CryptoInit);
            return false;
        }
    }

    if (!m_testNonce.empty()) {
        m_clientNonce = m_testNonce;
    } else {
        m_clientNonce.assign(kNonceLen, 0);
        if (RAND_bytes(m_clientNonce.data(), static_cast<int>(kNonceLen)) != 1) {
            fail(Error::CryptoInit);
            return false;
        }
    }
    if (!m_testRandomId.empty()) {
        m_randomId = m_testRandomId;
    } else {
        m_randomId.assign(kRandomIdLen, 0);
        if (RAND_bytes(m_randomId.data(), static_cast<int>(kRandomIdLen)) != 1) {
            fail(Error::CryptoInit);
            return false;
        }
    }

    im::proto::AppClientHello hello;
    hello.set_version(proto::APP_SECURITY_VERSION);
    hello.set_client_ephemeral_public_key(m_clientPub.data(), m_clientPub.size());
    hello.set_client_nonce(m_clientNonce.data(), m_clientNonce.size());
    hello.set_client_random_id(m_randomId.data(), m_randomId.size());
    hello.set_cipher_suite(im::proto::APP_CIPHER_X25519_ED25519_HKDF_SHA256_AES_256_GCM);

    m_clientPayload = hello.SerializeAsString();
    outPayload = m_clientPayload;
    m_status = Status::HelloSent;
    return true;
}

// ---------------- 步骤 2：ServerHello ----------------

bool ClientSecureChannel::handleServerHello(const std::string& serverPayload,
                                            std::string& outFinishedPayload)
{
    if (m_status != Status::HelloSent) {
        fail(Error::BadState);
        return false;
    }
    m_serverPayload = serverPayload;

    im::proto::AppServerHello hello;
    if (!hello.ParseFromString(serverPayload)) {
        fail(Error::BadFieldLength);
        return false;
    }
    if (hello.version() != proto::APP_SECURITY_VERSION) {
        fail(Error::BadVersion);
        return false;
    }
    if (hello.cipher_suite() != im::proto::APP_CIPHER_X25519_ED25519_HKDF_SHA256_AES_256_GCM) {
        fail(Error::BadCipherSuite);
        return false;
    }
    if (hello.server_ephemeral_public_key().size() != kKeyLen ||
        hello.server_nonce().size() != kNonceLen ||
        hello.session_id().size() != kSessionIdLen ||
        hello.signature().size() != kSignatureLen) {
        fail(Error::BadFieldLength);
        return false;
    }

    m_serverPub.assign(hello.server_ephemeral_public_key().begin(),
                       hello.server_ephemeral_public_key().end());
    m_serverNonce.assign(hello.server_nonce().begin(), hello.server_nonce().end());
    m_sessionId.assign(hello.session_id().begin(), hello.session_id().end());
    m_keyId = hello.key_id();

    // 未知 key_id 一律 fail-close
    const auto it = m_identityKeys.find(m_keyId);
    if (it == m_identityKeys.end()) {
        fail(Error::BadKeyId);
        return false;
    }

    // signing transcript（与服务端逐字节一致）
    std::string transcript;
    transcript.append(kHandshakeLabel);
    appendU32Be(transcript, static_cast<std::uint32_t>(m_clientPayload.size()));
    transcript.append(m_clientPayload);
    appendU32Be(transcript, proto::APP_SECURITY_VERSION);
    transcript.append(reinterpret_cast<const char*>(m_serverPub.data()), m_serverPub.size());
    transcript.append(reinterpret_cast<const char*>(m_serverNonce.data()), m_serverNonce.size());
    transcript.append(reinterpret_cast<const char*>(m_sessionId.data()), m_sessionId.size());
    appendU32Be(transcript, m_keyId);
    appendU32Be(transcript, proto::APP_CIPHER_SUITE_V1);

    const Bytes signature(hello.signature().begin(), hello.signature().end());
    if (!ed25519Verify(it->second, transcript, signature)) {
        fail(Error::BadSignature);
        return false;
    }
    secureClear(transcript);

    // transcript hash
    std::string transcriptInput;
    appendU32Be(transcriptInput, static_cast<std::uint32_t>(m_clientPayload.size()));
    transcriptInput.append(m_clientPayload);
    appendU32Be(transcriptInput, static_cast<std::uint32_t>(m_serverPayload.size()));
    transcriptInput.append(m_serverPayload);
    unsigned char digest[SHA256_DIGEST_LENGTH] = {};
    if (!sha256Of(transcriptInput, digest)) {
        secureClear(transcriptInput);
        fail(Error::CryptoInit);
        return false;
    }
    secureClear(transcriptInput);
    m_transcriptHash.assign(digest, digest + SHA256_DIGEST_LENGTH);
    OPENSSL_cleanse(digest, sizeof(digest));

    // X25519 共享秘密；全零拒绝
    Bytes sharedSecret;
    if (!x25519Derive(m_clientPriv, m_serverPub, sharedSecret)) {
        fail(Error::CryptoInit);
        return false;
    }
    const bool allZero = std::all_of(sharedSecret.begin(), sharedSecret.end(),
                                     [](unsigned char c) { return c == 0; });
    if (allZero) {
        secureClear(sharedSecret);
        fail(Error::CryptoInit);
        return false;
    }

    // salt = sha256(clientNonce || serverNonce)
    std::string saltInput;
    saltInput.append(reinterpret_cast<const char*>(m_clientNonce.data()), m_clientNonce.size());
    saltInput.append(reinterpret_cast<const char*>(m_serverNonce.data()), m_serverNonce.size());
    unsigned char salt[SHA256_DIGEST_LENGTH] = {};
    if (!sha256Of(saltInput, salt)) {
        secureClear(sharedSecret);
        secureClear(saltInput);
        fail(Error::CryptoInit);
        return false;
    }
    secureClear(saltInput);

    // info = label || sessionId || transcriptHash
    Bytes info;
    info.reserve(std::strlen(kChannelLabel) + m_sessionId.size() + m_transcriptHash.size());
    info.insert(info.end(), kChannelLabel, kChannelLabel + std::strlen(kChannelLabel));
    info.insert(info.end(), m_sessionId.begin(), m_sessionId.end());
    info.insert(info.end(), m_transcriptHash.begin(), m_transcriptHash.end());

    Bytes material;
    if (!hkdfSha256(sharedSecret, Bytes(salt, salt + SHA256_DIGEST_LENGTH), info, kHkdfOutputLen,
                    material)) {
        secureClear(sharedSecret);
        OPENSSL_cleanse(salt, sizeof(salt));
        fail(Error::CryptoInit);
        return false;
    }
    secureClear(sharedSecret);
    OPENSSL_cleanse(salt, sizeof(salt));
    secureClear(info);

    // 密钥切片：顺序与服务端完全一致
    auto slice = [&material](std::size_t begin, std::size_t end) {
        return Bytes(material.begin() + static_cast<long>(begin),
                     material.begin() + static_cast<long>(end));
    };
    m_clientToServerKey = slice(0, 32);
    m_serverToClientKey = slice(32, 64);
    m_clientNoncePrefix = slice(64, 68);
    m_serverNoncePrefix = slice(68, 72);
    m_clientFinishedKey = slice(72, 104);
    m_serverFinishedKey = slice(104, 136);
    secureClear(material);

    // client Finished = HMAC(clientFinishedKey, transcriptHash)
    std::string finishedInput(reinterpret_cast<const char*>(m_transcriptHash.data()),
                              m_transcriptHash.size());
    unsigned char verify[SHA256_DIGEST_LENGTH] = {};
    if (!hmacSha256(m_clientFinishedKey.data(), m_clientFinishedKey.size(), finishedInput, verify)) {
        secureClear(finishedInput);
        fail(Error::CryptoInit);
        return false;
    }
    secureClear(finishedInput);

    im::proto::AppFinished finished;
    finished.set_verify_data(verify, SHA256_DIGEST_LENGTH);
    outFinishedPayload = finished.SerializeAsString();
    OPENSSL_cleanse(verify, sizeof(verify));

    m_status = Status::FinishedSent;
    return true;
}

// ---------------- 步骤 3：ServerFinished ----------------

bool ClientSecureChannel::handleServerFinished(const std::string& payload)
{
    if (m_status != Status::FinishedSent) {
        fail(Error::BadState);
        return false;
    }

    im::proto::AppFinished finished;
    if (!finished.ParseFromString(payload) || finished.verify_data().size() != kFinishedLen) {
        fail(Error::BadFinished);
        return false;
    }

    // expected = HMAC(serverFinishedKey, transcriptHash || clientVerify)
    im::proto::AppFinished clientFinished;
    std::string clientVerify;
    {
        std::string input(reinterpret_cast<const char*>(m_transcriptHash.data()),
                          m_transcriptHash.size());
        unsigned char verify[SHA256_DIGEST_LENGTH] = {};
        if (!hmacSha256(m_clientFinishedKey.data(), m_clientFinishedKey.size(), input, verify)) {
            secureClear(input);
            fail(Error::CryptoInit);
            return false;
        }
        secureClear(input);
        clientVerify.assign(reinterpret_cast<const char*>(verify), SHA256_DIGEST_LENGTH);
        OPENSSL_cleanse(verify, sizeof(verify));
    }

    std::string expectedInput;
    expectedInput.append(reinterpret_cast<const char*>(m_transcriptHash.data()),
                         m_transcriptHash.size());
    expectedInput.append(clientVerify);
    unsigned char expected[SHA256_DIGEST_LENGTH] = {};
    if (!hmacSha256(m_serverFinishedKey.data(), m_serverFinishedKey.size(), expectedInput, expected)) {
        secureClear(expectedInput);
        secureClear(clientVerify);
        fail(Error::CryptoInit);
        return false;
    }
    secureClear(expectedInput);
    secureClear(clientVerify);

    const bool ok = constantTimeEqual(
        expected, reinterpret_cast<const unsigned char*>(finished.verify_data().data()),
        kFinishedLen);
    OPENSSL_cleanse(expected, sizeof(expected));

    if (!ok) {
        fail(Error::BadFinished);
        return false;
    }

    m_status = Status::Established;
    return true;
}

// ---------------- 业务帧 ----------------

bool ClientSecureChannel::encrypt(proto::protType innerType, const std::string& payload,
                                  std::string& outFramePayload)
{
    if (m_status != Status::Established) {
        fail(Error::BadState);
        return false;
    }
    // 安全控制协议不得作为业务帧内嵌
    if (innerType >= proto::DEF_PROT_APP_CLIENT_HELLO &&
        innerType <= proto::DEF_PROT_APP_ENCRYPTED_FRAME) {
        fail(Error::BadFrame);
        return false;
    }
    if (m_sendSequence == UINT64_MAX) {
        fail(Error::Sequence);
        return false;
    }
    const std::uint64_t sequence = ++m_sendSequence;

    // plaintext = le32(innerType) || payload
    std::string plaintext;
    plaintext.resize(sizeof(proto::protType) + payload.size());
    proto::encodeType32(innerType, plaintext.data());
    if (!payload.empty()) {
        std::memcpy(plaintext.data() + sizeof(proto::protType), payload.data(), payload.size());
    }

    // nonce = prefix(4) || u64be(sequence)(8)
    unsigned char nonce[kGcmNonceLen] = {};
    std::memcpy(nonce, m_clientNoncePrefix.data(),
                std::min<std::size_t>(m_clientNoncePrefix.size(), 4));
    {
        std::string seqBe;
        appendU64Be(seqBe, sequence);
        std::memcpy(nonce + 4, seqBe.data(), 8);
    }

    // aad = label || u32be(version) || sessionId(16) || u64be(sequence)
    std::string aad;
    aad.append(kFrameLabel);
    appendU32Be(aad, proto::APP_SECURITY_VERSION);
    aad.append(reinterpret_cast<const char*>(m_sessionId.data()), m_sessionId.size());
    appendU64Be(aad, sequence);

    std::string ciphertext;
    unsigned char tag[kTagLen] = {};
    if (!aesGcmEncrypt(m_clientToServerKey.data(), nonce, aad, plaintext, ciphertext, tag)) {
        secureClear(plaintext);
        secureClear(aad);
        fail(Error::Internal);
        return false;
    }
    secureClear(plaintext);
    secureClear(aad);

    im::proto::AppEncryptedFrame frame;
    frame.set_version(proto::APP_SECURITY_VERSION);
    frame.set_session_id(m_sessionId.data(), m_sessionId.size());
    frame.set_sequence(sequence);
    frame.set_ciphertext(ciphertext.data(), ciphertext.size());
    frame.set_tag(tag, kTagLen);
    outFramePayload = frame.SerializeAsString();
    secureClear(ciphertext);
    OPENSSL_cleanse(tag, sizeof(tag));
    return true;
}

bool ClientSecureChannel::decrypt(const std::string& framePayload, proto::protType& outType,
                                  std::string& outPayload)
{
    if (m_status != Status::Established) {
        fail(Error::BadState);
        return false;
    }

    im::proto::AppEncryptedFrame frame;
    if (!frame.ParseFromString(framePayload)) {
        fail(Error::BadFrame);
        return false;
    }
    if (frame.version() != proto::APP_SECURITY_VERSION) {
        fail(Error::BadFrame);
        return false;
    }
    if (frame.session_id().size() != kSessionIdLen ||
        !constantTimeEqual(reinterpret_cast<const unsigned char*>(frame.session_id().data()),
                           m_sessionId.data(), kSessionIdLen)) {
        fail(Error::BadFrame);
        return false;
    }
    if (frame.tag().size() != kTagLen) {
        fail(Error::BadFrame);
        return false;
    }
    if (frame.ciphertext().size() < sizeof(proto::protType)) {
        fail(Error::BadFrame);
        return false;
    }
    // sequence 从 1 起严格 +1：重复、跳号、回退都拒绝
    if (frame.sequence() == 0 || m_receiveSequence == UINT64_MAX ||
        frame.sequence() != m_receiveSequence + 1) {
        fail(Error::Sequence);
        return false;
    }
    const std::uint64_t sequence = frame.sequence();

    unsigned char nonce[kGcmNonceLen] = {};
    std::memcpy(nonce, m_serverNoncePrefix.data(),
                std::min<std::size_t>(m_serverNoncePrefix.size(), 4));
    {
        std::string seqBe;
        appendU64Be(seqBe, sequence);
        std::memcpy(nonce + 4, seqBe.data(), 8);
    }

    std::string aad;
    aad.append(kFrameLabel);
    appendU32Be(aad, proto::APP_SECURITY_VERSION);
    aad.append(reinterpret_cast<const char*>(m_sessionId.data()), m_sessionId.size());
    appendU64Be(aad, sequence);

    std::string plaintext;
    const bool ok = aesGcmDecrypt(
        m_serverToClientKey.data(), nonce, aad, frame.ciphertext(),
        reinterpret_cast<const unsigned char*>(frame.tag().data()), plaintext);
    secureClear(aad);
    if (!ok) {
        // 认证失败：绝不把任何明文交给业务分发
        secureClear(plaintext);
        m_lastError = Error::Authentication;
        m_status = Status::Failed;
        return false;
    }

    const proto::protType innerType = proto::decodeType32(plaintext.data());
    if (innerType >= proto::DEF_PROT_APP_CLIENT_HELLO &&
        innerType <= proto::DEF_PROT_APP_ENCRYPTED_FRAME) {
        secureClear(plaintext);
        fail(Error::BadFrame);
        return false;
    }

    outType = innerType;
    outPayload.assign(plaintext.data() + sizeof(proto::protType),
                      plaintext.size() - sizeof(proto::protType));
    secureClear(plaintext);

    m_receiveSequence = sequence;
    return true;
}

// ---------------- 进程内握手自检（跨架构冒烟） ----------------

namespace {

// 仅自检用：生成一对 Ed25519 身份密钥（raw private 32B / raw public 32B）。
bool ed25519GenerateSelfTest(ClientSecureChannel::Bytes& priv, ClientSecureChannel::Bytes& pub)
{
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!ctx) return false;
    EVP_PKEY* pkey = nullptr;
    const bool ok = EVP_PKEY_keygen_init(ctx) > 0 && EVP_PKEY_keygen(ctx, &pkey) > 0;
    EVP_PKEY_CTX_free(ctx);
    if (!ok || !pkey) {
        if (pkey) EVP_PKEY_free(pkey);
        return false;
    }
    priv.assign(kKeyLen, 0);
    pub.assign(kKeyLen, 0);
    std::size_t privLen = kKeyLen, pubLen = kKeyLen;
    const bool got = EVP_PKEY_get_raw_private_key(pkey, priv.data(), &privLen) > 0 &&
                     EVP_PKEY_get_raw_public_key(pkey, pub.data(), &pubLen) > 0;
    EVP_PKEY_free(pkey);
    return got && privLen == kKeyLen && pubLen == kKeyLen;
}

// 仅自检用：用 raw Ed25519 私钥对 msg 做 one-shot 签名。
bool ed25519SignSelfTest(const ClientSecureChannel::Bytes& priv, const std::string& msg,
                         ClientSecureChannel::Bytes& sig)
{
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, priv.data(), priv.size());
    if (!pkey) return false;
    EVP_MD_CTX* md = EVP_MD_CTX_new();
    bool ok = false;
    if (md) {
        std::size_t sigLen = kSignatureLen;
        sig.assign(kSignatureLen, 0);
        ok = EVP_DigestSignInit(md, nullptr, nullptr, nullptr, pkey) > 0 &&
             EVP_DigestSign(md, sig.data(), &sigLen,
                            reinterpret_cast<const unsigned char*>(msg.data()), msg.size()) == 1 &&
             sigLen == kSignatureLen;
        EVP_MD_CTX_free(md);
    }
    EVP_PKEY_free(pkey);
    return ok;
}

void appendBytes(std::string& out, const ClientSecureChannel::Bytes& b)
{
    out.append(reinterpret_cast<const char*>(b.data()), b.size());
}

} // namespace

bool ClientSecureChannel::runLoopbackHandshakeSelfTest(std::string* outDiagnostics)
{
    auto note = [&](const char* step, bool ok) {
        if (outDiagnostics) {
            *outDiagnostics += step;
            *outDiagnostics += ok ? "=ok\n" : "=FAIL\n";
        }
        return ok;
    };

    // ---- 服务端半程：临时身份密钥 + 固定 keyId=1 ----
    const KeyId keyId = 1;
    Bytes idPriv, idPub;
    if (!note("ed25519_keygen", ed25519GenerateSelfTest(idPriv, idPub))) return false;

    ClientSecureChannel client;
    std::map<KeyId, Bytes> trusted;
    trusted[keyId] = idPub;
    client.setTrustedIdentityKeys(trusted);

    // 步骤 1：ClientHello
    std::string clientHelloPayload;
    if (!note("client_hello", client.buildClientHello(clientHelloPayload))) return false;

    im::proto::AppClientHello clientHello;
    if (!note("parse_client_hello", clientHello.ParseFromString(clientHelloPayload))) return false;

    // 服务端生成临时 X25519、nonce、sessionId，并签署 signingTranscript（对齐 Session.cpp）
    Bytes serverPriv, serverPub;
    if (!note("server_x25519", x25519Generate(serverPriv, serverPub))) return false;
    Bytes serverNonce(kNonceLen, 0), sessionId(kSessionIdLen, 0);
    if (!note("server_random",
              RAND_bytes(serverNonce.data(), static_cast<int>(kNonceLen)) == 1 &&
                  RAND_bytes(sessionId.data(), static_cast<int>(kSessionIdLen)) == 1))
        return false;

    std::string transcript;
    transcript.append(kHandshakeLabel);
    appendU32Be(transcript, static_cast<std::uint32_t>(clientHelloPayload.size()));
    transcript.append(clientHelloPayload);
    appendU32Be(transcript, proto::APP_SECURITY_VERSION);
    appendBytes(transcript, serverPub);
    appendBytes(transcript, serverNonce);
    appendBytes(transcript, sessionId);
    appendU32Be(transcript, keyId);
    appendU32Be(transcript, proto::APP_CIPHER_SUITE_V1);

    Bytes signature;
    if (!note("server_sign", ed25519SignSelfTest(idPriv, transcript, signature))) return false;

    im::proto::AppServerHello serverHello;
    serverHello.set_version(proto::APP_SECURITY_VERSION);
    serverHello.set_server_ephemeral_public_key(serverPub.data(), serverPub.size());
    serverHello.set_server_nonce(serverNonce.data(), serverNonce.size());
    serverHello.set_session_id(sessionId.data(), sessionId.size());
    serverHello.set_key_id(keyId);
    serverHello.set_signature(signature.data(), signature.size());
    serverHello.set_cipher_suite(im::proto::APP_CIPHER_X25519_ED25519_HKDF_SHA256_AES_256_GCM);
    const std::string serverHelloPayload = serverHello.SerializeAsString();

    // 步骤 2：客户端处理 ServerHello（含 Ed25519 验签），产出 ClientFinished
    std::string clientFinishedPayload;
    if (!note("client_handle_server_hello",
              client.handleServerHello(serverHelloPayload, clientFinishedPayload)))
        return false;

    // 服务端派生同一套密钥（与 client 内部一致），计算 transcriptHash / material
    Bytes shared;
    Bytes clientPub(clientHello.client_ephemeral_public_key().begin(),
                    clientHello.client_ephemeral_public_key().end());
    if (!note("server_x25519_derive", x25519Derive(serverPriv, clientPub, shared))) return false;

    std::string transcriptHashInput;
    appendU32Be(transcriptHashInput, static_cast<std::uint32_t>(clientHelloPayload.size()));
    transcriptHashInput.append(clientHelloPayload);
    appendU32Be(transcriptHashInput, static_cast<std::uint32_t>(serverHelloPayload.size()));
    transcriptHashInput.append(serverHelloPayload);
    unsigned char thDigest[SHA256_DIGEST_LENGTH] = {};
    if (!note("server_transcript_hash", sha256Of(transcriptHashInput, thDigest))) return false;
    Bytes transcriptHash(thDigest, thDigest + SHA256_DIGEST_LENGTH);

    Bytes saltInput(clientHello.client_nonce().begin(), clientHello.client_nonce().end());
    saltInput.insert(saltInput.end(), serverNonce.begin(), serverNonce.end());
    unsigned char saltDigest[SHA256_DIGEST_LENGTH] = {};
    if (!note("server_salt",
              SHA256(saltInput.data(), saltInput.size(), saltDigest) != nullptr))
        return false;
    Bytes salt(saltDigest, saltDigest + SHA256_DIGEST_LENGTH);

    Bytes info;
    const char* channelLabel = kChannelLabel;
    info.insert(info.end(), channelLabel, channelLabel + std::strlen(channelLabel));
    info.insert(info.end(), sessionId.begin(), sessionId.end());
    info.insert(info.end(), transcriptHash.begin(), transcriptHash.end());

    Bytes material;
    if (!note("server_hkdf", hkdfSha256(shared, salt, info, kHkdfOutputLen, material))) return false;
    auto slice = [&](std::size_t off, std::size_t len) {
        return Bytes(material.begin() + off, material.begin() + off + len);
    };
    const Bytes c2sKey = slice(0, 32);
    const Bytes s2cKey = slice(32, 32);
    const Bytes cNoncePrefix = slice(64, 4);
    const Bytes sNoncePrefix = slice(68, 4);
    const Bytes cFinishedKey = slice(72, 32);
    const Bytes sFinishedKey = slice(104, 32);

    // 校验 ClientFinished = HMAC(cFinishedKey, transcriptHash)
    im::proto::AppFinished clientFinished;
    if (!note("parse_client_finished", clientFinished.ParseFromString(clientFinishedPayload)))
        return false;
    std::string thStr(reinterpret_cast<const char*>(transcriptHash.data()), transcriptHash.size());
    unsigned char expectClientVerify[SHA256_DIGEST_LENGTH] = {};
    if (!note("server_hmac_client",
              hmacSha256(cFinishedKey.data(), cFinishedKey.size(), thStr, expectClientVerify)))
        return false;
    const bool clientFinishOk =
        clientFinished.verify_data().size() == kFinishedLen &&
        constantTimeEqual(expectClientVerify,
                          reinterpret_cast<const unsigned char*>(clientFinished.verify_data().data()),
                          kFinishedLen);
    if (!note("verify_client_finished", clientFinishOk)) return false;

    // 步骤 4：服务端 Finished = HMAC(sFinishedKey, transcriptHash || clientVerify)
    std::string serverFinishedInput = thStr;
    serverFinishedInput.append(clientFinished.verify_data());
    unsigned char serverVerify[SHA256_DIGEST_LENGTH] = {};
    if (!note("server_hmac_server",
              hmacSha256(sFinishedKey.data(), sFinishedKey.size(), serverFinishedInput, serverVerify)))
        return false;
    im::proto::AppFinished serverFinished;
    serverFinished.set_verify_data(serverVerify, kFinishedLen);
    if (!note("client_handle_server_finished",
              client.handleServerFinished(serverFinished.SerializeAsString())))
        return false;

    if (!note("client_established", client.established())) return false;

    // ---- 加密业务帧双向往返 ----
    // client → server
    const std::string appPayload = "jitong-arm64-secure-channel-smoke";
    std::string encryptedFrame;
    if (!note("client_encrypt",
              client.encrypt(proto::DEF_PROT_HEARTBEAT_RQ, appPayload, encryptedFrame)))
        return false;

    im::proto::AppEncryptedFrame inbound;
    if (!note("parse_frame", inbound.ParseFromString(encryptedFrame))) return false;
    // 服务端用 c2sKey + clientNoncePrefix 解密（sequence=1）
    {
        const std::uint64_t seq = inbound.sequence();
        unsigned char nonce[kGcmNonceLen] = {};
        std::memcpy(nonce, cNoncePrefix.data(), 4);
        std::string seqBe;
        appendU64Be(seqBe, seq);
        std::memcpy(nonce + 4, seqBe.data(), 8);
        std::string aad;
        aad.append(kFrameLabel);
        appendU32Be(aad, proto::APP_SECURITY_VERSION);
        appendBytes(aad, sessionId);
        appendU64Be(aad, seq);
        std::string plaintext;
        const bool ok = aesGcmDecrypt(
            c2sKey.data(), nonce, aad, inbound.ciphertext(),
            reinterpret_cast<const unsigned char*>(inbound.tag().data()), plaintext);
        const bool matches = ok && plaintext.size() >= sizeof(proto::protType) &&
                             plaintext.substr(sizeof(proto::protType)) == appPayload;
        if (!note("server_decrypt", matches)) return false;
    }

    // server → client：服务端用 s2cKey + serverNoncePrefix 加密 sequence=1
    {
        const std::string reply = "server-reply-ok";
        std::string plaintext;
        appendU32Be(plaintext, 0); // 占位，稍后覆写为 le32(innerType)
        plaintext.clear();
        // 内层明文 = le32(innerType) || payload
        proto::protType innerType = proto::DEF_PROT_HEARTBEAT_RS;
        char typeBuf[4];
        proto::encodeType32(innerType, typeBuf);
        plaintext.append(typeBuf, 4);
        plaintext.append(reply);

        const std::uint64_t seq = 1;
        unsigned char nonce[kGcmNonceLen] = {};
        std::memcpy(nonce, sNoncePrefix.data(), 4);
        std::string seqBe;
        appendU64Be(seqBe, seq);
        std::memcpy(nonce + 4, seqBe.data(), 8);
        std::string aad;
        aad.append(kFrameLabel);
        appendU32Be(aad, proto::APP_SECURITY_VERSION);
        appendBytes(aad, sessionId);
        appendU64Be(aad, seq);
        std::string ciphertext;
        unsigned char tag[kTagLen] = {};
        if (!note("server_encrypt",
                  aesGcmEncrypt(s2cKey.data(), nonce, aad, plaintext, ciphertext, tag)))
            return false;
        im::proto::AppEncryptedFrame outbound;
        outbound.set_version(proto::APP_SECURITY_VERSION);
        outbound.set_session_id(sessionId.data(), sessionId.size());
        outbound.set_sequence(seq);
        outbound.set_ciphertext(ciphertext.data(), ciphertext.size());
        outbound.set_tag(tag, kTagLen);

        proto::protType gotType = 0;
        std::string gotPayload;
        const bool ok = client.decrypt(outbound.SerializeAsString(), gotType, gotPayload);
        const bool matches = ok && gotType == innerType && gotPayload == reply;
        if (!note("client_decrypt", matches)) return false;
    }

    if (outDiagnostics) *outDiagnostics += "result=ok\n";
    return true;
}

} // namespace transport
} // namespace im
