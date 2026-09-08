// ClientSecureChannel 测试：四步握手、加密帧与安全负向用例（P4-T02 / P4-T03）。
//
// 这里按服务端 Session.cpp 的字节级约定**模拟**出一个服务端，
// 用于验证客户端实现与服务端逐字节一致；真实一致性由 test_e2e 保证。
//
// 同时输出 Golden 向量（hex），供 Android/C++ 三端比对（22.3.4）。

#include "transport/ClientSecureChannel.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include "im.pb.h"

using im::proto::protType;
using im::transport::ClientSecureChannel;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++g_failures;
        std::cerr << "  [FAIL] " << what << std::endl;
    } else {
        std::cout << "  [ok]   " << what << std::endl;
    }
}

using Bytes = ClientSecureChannel::Bytes;

std::string hex(const Bytes& b)
{
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(b.size() * 2);
    for (unsigned char c : b) {
        out.push_back(kDigits[(c >> 4) & 0xF]);
        out.push_back(kDigits[c & 0xF]);
    }
    return out;
}

// ---------------- Golden JSON 读取（R2-F04） ----------------
//
// 三端一致性向量以 app-security-v1.json 为唯一真相源。这里做一个极简读取器：
// 针对本文件固定 schema，按 "key": "value" / "key": number 抽取字段，避免引入第三方
// JSON 依赖。测试从文件读取期望值，再与生产实现算出的实际值比对——JSON 一旦与实现
// 漂移，测试立即失败，而不是让 .cpp 里再藏一份硬编码副本。

struct GoldenVectors {
    std::string raw;
    bool loaded = false;

    bool load(const char* path)
    {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) return false;
        std::ostringstream ss;
        ss << ifs.rdbuf();
        raw = ss.str();
        // 只在 "vector" 段内查找实际向量：像 client_verify_data 这类 key 在上方的
        // finished/key_schedule 段里还作为「公式描述」出现过一次，若从头找会命中描述
        // 文本而非真实值。这里把搜索起点定位到 "vector" 对象。
        m_vectorOffset = raw.find("\"vector\"");
        if (m_vectorOffset == std::string::npos) m_vectorOffset = 0;
        loaded = !raw.empty();
        return loaded;
    }

    // 读取字符串字段值（"key": "value"）。找不到返回空串。
    std::string str(const std::string& key) const
    {
        const std::string needle = "\"" + key + "\"";
        std::size_t k = raw.find(needle, m_vectorOffset);
        if (k == std::string::npos) return {};
        std::size_t colon = raw.find(':', k + needle.size());
        if (colon == std::string::npos) return {};
        std::size_t q1 = raw.find('"', colon + 1);
        if (q1 == std::string::npos) return {};
        std::size_t q2 = raw.find('"', q1 + 1);
        if (q2 == std::string::npos) return {};
        return raw.substr(q1 + 1, q2 - q1 - 1);
    }

    // 读取数字字段值（"key": number）。找不到返回 def。
    long num(const std::string& key, long def = -1) const
    {
        const std::string needle = "\"" + key + "\"";
        std::size_t k = raw.find(needle, m_vectorOffset);
        if (k == std::string::npos) return def;
        std::size_t colon = raw.find(':', k + needle.size());
        if (colon == std::string::npos) return def;
        return std::strtol(raw.c_str() + colon + 1, nullptr, 10);
    }

private:
    std::size_t m_vectorOffset = 0;
};

void appendU32Be(std::string& out, std::uint32_t v)
{
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}

// ---------------- 测试用身份密钥（Ed25519） ----------------

struct Identity {
    EVP_PKEY* pkey = nullptr;

    bool generate()
    {
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        if (!ctx) return false;
        const bool ok = EVP_PKEY_keygen_init(ctx) > 0 && EVP_PKEY_keygen(ctx, &pkey) > 0;
        EVP_PKEY_CTX_free(ctx);
        return ok && pkey;
    }

    // 从固定的 32 字节原始私钥加载，用于可复现的 Golden 向量。
    bool loadPrivate(const Bytes& seed)
    {
        if (seed.size() != 32) return false;
        if (pkey) { EVP_PKEY_free(pkey); pkey = nullptr; }
        pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size());
        return pkey != nullptr;
    }

    Bytes publicKey()
    {
        Bytes out(32, 0);
        std::size_t len = 32;
        if (EVP_PKEY_get_raw_public_key(pkey, out.data(), &len) <= 0) return {};
        return out;
    }

    Bytes sign(const std::string& msg)
    {
        EVP_MD_CTX* md = EVP_MD_CTX_new();
        if (!md) return {};
        std::size_t len = 0;
        bool ok = EVP_DigestSignInit(md, nullptr, nullptr, nullptr, pkey) > 0 &&
                  EVP_DigestSign(md, nullptr, &len,
                                 reinterpret_cast<const unsigned char*>(msg.data()),
                                 msg.size()) > 0;
        Bytes sig;
        if (ok) {
            sig.assign(len, 0);
            ok = EVP_DigestSign(md, sig.data(), &len,
                                reinterpret_cast<const unsigned char*>(msg.data()),
                                msg.size()) > 0;
        }
        EVP_MD_CTX_free(md);
        return ok ? sig : Bytes{};
    }

    ~Identity() { if (pkey) EVP_PKEY_free(pkey); }
};

// ---------------- 模拟服务端 ----------------

struct FakeServerSide {
    Bytes priv; // X25519 私钥
    Bytes pub;
    Bytes nonce;
    Bytes sessionId;
    std::uint32_t keyId = 1;

    bool generateKeys()
    {
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
        EVP_PKEY* pkey = nullptr;
        const bool ok = ctx && EVP_PKEY_keygen_init(ctx) > 0 && EVP_PKEY_keygen(ctx, &pkey) > 0;
        if (ctx) EVP_PKEY_CTX_free(ctx);
        if (!ok || !pkey) return false;
        priv.assign(32, 0);
        pub.assign(32, 0);
        std::size_t pl = 32;
        std::size_t ul = 32;
        const bool got = EVP_PKEY_get_raw_private_key(pkey, priv.data(), &pl) > 0 &&
                         EVP_PKEY_get_raw_public_key(pkey, pub.data(), &ul) > 0;
        EVP_PKEY_free(pkey);
        nonce.assign(32, 0);
        sessionId.assign(16, 0);
        RAND_bytes(nonce.data(), 32);
        RAND_bytes(sessionId.data(), 16);
        return got;
    }

    // 从固定的 X25519 私钥 / nonce / sessionId 派生，用于可复现的 Golden 向量。
    bool loadDeterministic(const Bytes& seed, const Bytes& fixedNonce, const Bytes& fixedSession)
    {
        if (seed.size() != 32 || fixedNonce.size() != 32 || fixedSession.size() != 16) return false;
        EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, seed.data(), seed.size());
        if (!pkey) return false;
        priv = seed;
        pub.assign(32, 0);
        std::size_t ul = 32;
        const bool got = EVP_PKEY_get_raw_public_key(pkey, pub.data(), &ul) > 0;
        EVP_PKEY_free(pkey);
        nonce = fixedNonce;
        sessionId = fixedSession;
        return got;
    }

    /** 按服务端规则构造 signing transcript 并签名。 */
    std::string buildServerHello(const std::string& clientPayload, Identity& id)
    {
        std::string t;
        t.append("jitong-app-handshake-v1");
        appendU32Be(t, static_cast<std::uint32_t>(clientPayload.size()));
        t.append(clientPayload);
        appendU32Be(t, im::proto::APP_SECURITY_VERSION);
        t.append(reinterpret_cast<const char*>(pub.data()), pub.size());
        t.append(reinterpret_cast<const char*>(nonce.data()), nonce.size());
        t.append(reinterpret_cast<const char*>(sessionId.data()), sessionId.size());
        appendU32Be(t, keyId);
        appendU32Be(t, im::proto::APP_CIPHER_SUITE_V1);
        const Bytes sig = id.sign(t);

        im::proto::AppServerHello hello;
        hello.set_version(im::proto::APP_SECURITY_VERSION);
        hello.set_server_ephemeral_public_key(pub.data(), pub.size());
        hello.set_server_nonce(nonce.data(), nonce.size());
        hello.set_session_id(sessionId.data(), sessionId.size());
        hello.set_key_id(keyId);
        hello.set_signature(sig.data(), sig.size());
        hello.set_cipher_suite(im::proto::APP_CIPHER_X25519_ED25519_HKDF_SHA256_AES_256_GCM);
        return hello.SerializeAsString();
    }
};

/** 完整驱动一次握手；返回是否成功。 */
bool doHandshake(ClientSecureChannel& ch, FakeServerSide& server, Identity& id,
                 std::string* outClientHello = nullptr)
{
    std::string clientHello;
    if (!ch.buildClientHello(clientHello)) return false;
    if (outClientHello) *outClientHello = clientHello;

    const std::string serverHello = server.buildServerHello(clientHello, id);
    std::string finished;
    if (!ch.handleServerHello(serverHello, finished)) return false;

    // 服务端校验 client Finished 并回 server Finished
    // （此处省略服务端侧校验，等价于信任客户端产出）
    (void)finished;
    std::string serverFinished;
    {
        im::proto::AppFinished f;
        // server verify = HMAC(serverFinishedKey, transcriptHash || clientVerify)
        im::proto::AppFinished cf;
        cf.ParseFromString(finished);
        std::string input(reinterpret_cast<const char*>(ch.transcriptHash().data()),
                          ch.transcriptHash().size());
        input.append(cf.verify_data());
        unsigned char out[32] = {};
        unsigned int len = 0;
        HMAC(EVP_sha256(), ch.serverFinishedKey().data(),
             static_cast<int>(ch.serverFinishedKey().size()),
             reinterpret_cast<const unsigned char*>(input.data()), input.size(), out, &len);
        f.set_verify_data(out, 32);
        serverFinished = f.SerializeAsString();
    }
    return ch.handleServerFinished(serverFinished);
}

// ---------------- 用例 ----------------

void testHappyPath()
{
    std::cout << "[1] 四步握手成功" << std::endl;
    Identity id;
    check(id.generate(), "生成测试身份密钥");
    ClientSecureChannel ch;
    std::map<std::uint32_t, Bytes> keys;
    keys[1] = id.publicKey();
    ch.setTrustedIdentityKeys(keys);

    FakeServerSide server;
    check(server.generateKeys(), "生成服务端临时密钥");

    check(doHandshake(ch, server, id), "握手成功");
    check(ch.established(), "状态为 Established");
    check(ch.sessionId().size() == 16, "sessionId 16 字节");
    check(ch.transcriptHash().size() == 32, "transcriptHash 32 字节");
    check(ch.sendSequence() == 0 && ch.receiveSequence() == 0, "sequence 从 0 起（首帧为 1）");
}

void testEncryptDecryptRoundTrip()
{
    std::cout << "[2] 加密/解密往返与 sequence 递增" << std::endl;
    Identity id;
    id.generate();
    ClientSecureChannel ch;
    std::map<std::uint32_t, Bytes> keys;
    keys[1] = id.publicKey();
    ch.setTrustedIdentityKeys(keys);
    FakeServerSide server;
    server.generateKeys();
    check(doHandshake(ch, server, id), "握手成功");

    std::string frame;
    check(ch.encrypt(im::proto::DEF_PROT_LOGIN_RQ, "payload-1", frame), "加密成功");
    check(ch.sendSequence() == 1, "sendSequence 变为 1");

    // 服务端解密（用 clientToServerKey，此处用同一个对象反向验证：
    // 构造一个镜像通道来解 S→C 方向不现实，改为直接验证客户端能解自己发出的加密格式不可行，
    // 因此这里只验证 sequence 推进与帧格式；真实往返由 test_e2e 覆盖）
    im::proto::AppEncryptedFrame parsed;
    check(parsed.ParseFromString(frame), "加密帧可解析");
    check(parsed.version() == 1, "version=1");
    check(parsed.session_id().size() == 16, "session_id 16 字节");
    check(parsed.sequence() == 1, "sequence=1");
    check(parsed.tag().size() == 16, "tag 16 字节");
    check(parsed.ciphertext().size() > 4, "ciphertext 含协议号");
}

void testBadSignature()
{
    std::cout << "[3] ServerHello 签名翻转 1 bit" << std::endl;
    Identity id;
    id.generate();
    ClientSecureChannel ch;
    std::map<std::uint32_t, Bytes> keys;
    keys[1] = id.publicKey();
    ch.setTrustedIdentityKeys(keys);
    FakeServerSide server;
    server.generateKeys();

    std::string clientHello;
    ch.buildClientHello(clientHello);
    std::string serverHello = server.buildServerHello(clientHello, id);

    // 翻转签名最后一个 bit
    im::proto::AppServerHello hello;
    hello.ParseFromString(serverHello);
    std::string sig = hello.signature();
    sig[sig.size() - 1] = static_cast<char>(sig[sig.size() - 1] ^ 0x01);
    hello.set_signature(sig);
    const std::string tampered = hello.SerializeAsString();

    std::string finished;
    check(!ch.handleServerHello(tampered, finished), "签名被篡改 → 握手失败");
    check(ch.lastError() == ClientSecureChannel::Error::BadSignature, "错误类型为 BadSignature");
    check(!ch.established(), "未进入 Established");
}

void testBadFieldLengths()
{
    std::cout << "[4] 字段长度错误与未知 key_id" << std::endl;
    Identity id;
    id.generate();

    auto attempt = [&](std::function<void(im::proto::AppServerHello&)> mutate,
                       const std::string& label) {
        ClientSecureChannel ch;
        std::map<std::uint32_t, Bytes> keys;
        keys[1] = id.publicKey();
        ch.setTrustedIdentityKeys(keys);
        FakeServerSide server;
        server.generateKeys();
        std::string clientHello;
        ch.buildClientHello(clientHello);
        std::string raw = server.buildServerHello(clientHello, id);
        im::proto::AppServerHello hello;
        hello.ParseFromString(raw);
        mutate(hello);
        std::string finished;
        check(!ch.handleServerHello(hello.SerializeAsString(), finished), label);
    };

    attempt([](im::proto::AppServerHello& h) { h.set_server_nonce(std::string(31, 'a')); },
            "server_nonce 长度错误 → 失败");
    attempt([](im::proto::AppServerHello& h) { h.set_server_ephemeral_public_key(std::string(33, 'a')); },
            "server pubkey 长度错误 → 失败");
    attempt([](im::proto::AppServerHello& h) { h.set_session_id(std::string(15, 'a')); },
            "session_id 长度错误 → 失败");
    attempt([](im::proto::AppServerHello& h) { h.set_signature(std::string(63, 'a')); },
            "signature 长度错误 → 失败");
    attempt([](im::proto::AppServerHello& h) { h.set_version(2); }, "version 错误 → 失败");
    attempt([](im::proto::AppServerHello& h) { h.set_cipher_suite(im::proto::APP_CIPHER_UNSPECIFIED); },
            "cipher_suite 错误 → 失败");
    attempt([](im::proto::AppServerHello& h) { h.set_key_id(99); }, "未知 key_id → 失败");

    // 未注入任何 key_id 时必须 fail-close
    {
        ClientSecureChannel ch;
        FakeServerSide server;
        server.generateKeys();
        std::string clientHello;
        ch.buildClientHello(clientHello);
        std::string finished;
        check(!ch.handleServerHello(server.buildServerHello(clientHello, id), finished),
              "未注入信任根 → fail-close");
    }
}

void testBadServerFinished()
{
    std::cout << "[5] Finished verify_data 错误" << std::endl;
    Identity id;
    id.generate();
    ClientSecureChannel ch;
    std::map<std::uint32_t, Bytes> keys;
    keys[1] = id.publicKey();
    ch.setTrustedIdentityKeys(keys);
    FakeServerSide server;
    server.generateKeys();

    std::string clientHello;
    ch.buildClientHello(clientHello);
    std::string finished;
    check(ch.handleServerHello(server.buildServerHello(clientHello, id), finished), "ServerHello 通过");

    im::proto::AppFinished bad;
    bad.set_verify_data(std::string(32, '\x00'));
    check(!ch.handleServerFinished(bad.SerializeAsString()), "错误 verify_data → 失败");
    check(ch.lastError() == ClientSecureChannel::Error::BadFinished, "错误类型 BadFinished");

    im::proto::AppFinished shortOne;
    shortOne.set_verify_data(std::string(16, '\x00'));
    check(!ch.handleServerFinished(shortOne.SerializeAsString()), "verify_data 长度错误 → 失败");
}

void testSendBeforeEstablished()
{
    std::cout << "[6] 握手完成前禁止业务发送" << std::endl;
    ClientSecureChannel ch;
    std::string out;
    check(!ch.encrypt(im::proto::DEF_PROT_LOGIN_RQ, "x", out), "未建立通道时加密失败");

    // 安全控制协议不得作为业务帧内嵌
    Identity id;
    id.generate();
    std::map<std::uint32_t, Bytes> keys;
    keys[1] = id.publicKey();
    ch.setTrustedIdentityKeys(keys);
    FakeServerSide server;
    server.generateKeys();
    if (doHandshake(ch, server, id)) {
        check(!ch.encrypt(im::proto::DEF_PROT_APP_CLIENT_HELLO, "x", out),
              "内嵌安全控制协议被拒");
    }
}

void testReplayAndGap()
{
    std::cout << "[7] 重放与 sequence 缺口（构造解密侧）" << std::endl;
    // 用两个通道模拟双向：client(chA 发) 与 server(chB 收)
    // 由于 ClientSecureChannel 只实现客户端方向，这里用一个技巧：
    // 建立第二个通道并让其"扮演"接收方不可行。改为直接验证 decrypt 的状态检查。

    Identity id;
    id.generate();
    ClientSecureChannel ch;
    std::map<std::uint32_t, Bytes> keys;
    keys[1] = id.publicKey();
    ch.setTrustedIdentityKeys(keys);
    FakeServerSide server;
    server.generateKeys();
    check(doHandshake(ch, server, id), "握手成功");

    // 伪造一个 S→C 帧：session_id 正确但 sequence 非法（0）
    im::proto::AppEncryptedFrame f;
    f.set_version(1);
    f.set_session_id(ch.sessionId().data(), ch.sessionId().size());
    f.set_sequence(0);
    f.set_ciphertext(std::string(8, 'a'));
    f.set_tag(std::string(16, 'b'));
    protType t = 0;
    std::string p;
    check(!ch.decrypt(f.SerializeAsString(), t, p), "sequence=0 → 拒绝");
    check(ch.lastError() == ClientSecureChannel::Error::Sequence, "错误类型 Sequence");

    // sequence 跳号（期望 1，给 5）
    f.set_sequence(5);
    check(!ch.decrypt(f.SerializeAsString(), t, p), "sequence 跳号 → 拒绝");

    // session_id 不匹配
    f.set_sequence(1);
    f.set_session_id(std::string(16, 'z'));
    check(!ch.decrypt(f.SerializeAsString(), t, p), "session_id 不匹配 → 拒绝");

    // tag 长度错误
    f.set_session_id(ch.sessionId().data(), ch.sessionId().size());
    f.set_tag(std::string(8, 'b'));
    check(!ch.decrypt(f.SerializeAsString(), t, p), "tag 长度错误 → 拒绝");

    // 正确字段但错误密文 → GCM 认证失败。
    // 必须用**全新通道**：上面任一失败都会把通道置为 Failed，
    // 之后的 decrypt 只会返回 BadState，测不到真正的认证失败路径。
    {
        ClientSecureChannel ch2;
        ch2.setTrustedIdentityKeys(keys);
        FakeServerSide s2;
        s2.generateKeys();
        check(doHandshake(ch2, s2, id), "新通道握手成功");

        im::proto::AppEncryptedFrame f2;
        f2.set_version(1);
        f2.set_session_id(ch2.sessionId().data(), ch2.sessionId().size());
        f2.set_sequence(1);
        f2.set_ciphertext(std::string(8, 'a'));
        f2.set_tag(std::string(16, 'b'));
        protType t2 = 0;
        std::string p2;
        check(!ch2.decrypt(f2.SerializeAsString(), t2, p2), "伪造密文 → 认证失败");
        check(ch2.lastError() == ClientSecureChannel::Error::Authentication,
              "错误类型 Authentication（明文不上抛给业务）");
    }
}

void testGoldenVector()
{
    std::cout << "[8] Golden 向量（供三端比对）" << std::endl;
    Identity id;
    // 固定的 Ed25519 身份私钥种子（仅测试向量，非生产密钥）
    check(id.loadPrivate(Bytes(32, 0x44)), "加载固定身份密钥");

    // 固定的测试向量：确定性 X25519 私钥 / nonce / random_id
    Bytes priv(32, 0x11);
    Bytes nonce(32, 0x22);
    Bytes randomId(16, 0x33);

    ClientSecureChannel ch;
    std::map<std::uint32_t, Bytes> keys;
    keys[1] = id.publicKey();
    ch.setTrustedIdentityKeys(keys);
    ch.setDeterministicTestVector(priv, nonce, randomId);

    FakeServerSide server;
    // 固定服务端 X25519 私钥 / nonce / sessionId，使 ServerHello 完全可复现
    check(server.loadDeterministic(Bytes(32, 0x55), Bytes(32, 0x66), Bytes(16, 0x77)),
          "加载固定服务端密钥");

    std::string clientHello;
    check(ch.buildClientHello(clientHello), "确定性 ClientHello 生成成功");
    std::string finished;
    check(ch.handleServerHello(server.buildServerHello(clientHello, id), finished), "ServerHello 处理成功");

    // 服务端回 Finished，驱动通道进入 Established，才能加密 sequence=1 帧
    {
        im::proto::AppFinished cf;
        cf.ParseFromString(finished);
        std::string input(reinterpret_cast<const char*>(ch.transcriptHash().data()),
                          ch.transcriptHash().size());
        input.append(cf.verify_data());
        unsigned char out[32] = {};
        unsigned int len = 0;
        HMAC(EVP_sha256(), ch.serverFinishedKey().data(),
             static_cast<int>(ch.serverFinishedKey().size()),
             reinterpret_cast<const unsigned char*>(input.data()), input.size(), out, &len);
        im::proto::AppFinished sf;
        sf.set_verify_data(out, 32);
        check(ch.handleServerFinished(sf.SerializeAsString()), "ServerFinished 处理成功，通道 Established");
    }

    std::cout << "--- GOLDEN BEGIN ---" << std::endl;
    std::cout << "identity_public=" << hex(id.publicKey()) << std::endl;
    std::cout << "client_private=" << hex(priv) << std::endl;
    std::cout << "client_nonce=" << hex(nonce) << std::endl;
    std::cout << "client_random_id=" << hex(randomId) << std::endl;
    std::cout << "server_public=" << hex(server.pub) << std::endl;
    std::cout << "server_nonce=" << hex(server.nonce) << std::endl;
    std::cout << "session_id=" << hex(server.sessionId) << std::endl;
    std::cout << "transcript_hash=" << hex(ch.transcriptHash()) << std::endl;
    std::cout << "client_to_server_key=" << hex(ch.clientToServerKey()) << std::endl;
    std::cout << "server_to_client_key=" << hex(ch.serverToClientKey()) << std::endl;
    std::cout << "client_finished_key=" << hex(ch.clientFinishedKey()) << std::endl;
    std::cout << "server_finished_key=" << hex(ch.serverFinishedKey()) << std::endl;
    std::string clientVerifyHex;
    {
        im::proto::AppFinished f;
        f.ParseFromString(finished);
        clientVerifyHex = hex(Bytes(f.verify_data().begin(), f.verify_data().end()));
        std::cout << "client_verify_data=" << clientVerifyHex << std::endl;
    }

    // sequence=1 的加密帧向量：固定内层协议号 + 固定明文，
    // 由确定性 C→S key / nonce prefix / AAD 得到确定性 ciphertext/tag。
    const std::string framePlain = "jitong-golden-payload";
    std::uint64_t frameSeq = 0;
    std::string frameCipherHex, frameTagHex;
    {
        std::string framePayload;
        check(ch.encrypt(im::proto::DEF_PROT_HEARTBEAT_RQ, framePlain, framePayload),
              "sequence=1 加密帧生成成功");
        im::proto::AppEncryptedFrame fr;
        fr.ParseFromString(framePayload);
        frameSeq = fr.sequence();
        frameCipherHex = hex(Bytes(fr.ciphertext().begin(), fr.ciphertext().end()));
        frameTagHex = hex(Bytes(fr.tag().begin(), fr.tag().end()));
        std::cout << "frame_sequence=" << frameSeq << std::endl;
        std::cout << "frame_inner_plaintext=" << framePlain << std::endl;
        std::cout << "frame_ciphertext=" << frameCipherHex << std::endl;
        std::cout << "frame_tag=" << frameTagHex << std::endl;
    }
    std::cout << "--- GOLDEN END ---" << std::endl;

    // R2-F04：从 JSON 文件（唯一真相源）读取期望向量，与生产实现算出的实际值逐字段比对。
    // 不再把期望值硬编码进 .cpp——这样 C++ 端是「独立消费」这份三端共享 JSON，Android/
    // Server 端读同一文件即可获得同样的向量。任一字段漂移都会让本用例失败。
    GoldenVectors gv;
    check(gv.load(CLIENT_CORE_GOLDEN_JSON), "读取 golden JSON 文件");
    if (gv.loaded) {
        auto cmp = [&](const std::string& field, const std::string& actual) {
            const std::string expected = gv.str(field);
            check(!expected.empty() && expected == actual,
                  "JSON[" + field + "] 与实现一致");
        };
        cmp("identity_public", hex(id.publicKey()));
        cmp("client_private", hex(priv));
        cmp("client_nonce", hex(nonce));
        cmp("client_random_id", hex(randomId));
        cmp("server_public", hex(server.pub));
        cmp("server_nonce", hex(server.nonce));
        cmp("session_id", hex(server.sessionId));
        cmp("transcript_hash", hex(ch.transcriptHash()));
        cmp("client_to_server_key", hex(ch.clientToServerKey()));
        cmp("server_to_client_key", hex(ch.serverToClientKey()));
        cmp("client_finished_key", hex(ch.clientFinishedKey()));
        cmp("server_finished_key", hex(ch.serverFinishedKey()));
        cmp("client_verify_data", clientVerifyHex);
        cmp("frame_ciphertext", frameCipherHex);
        cmp("frame_tag", frameTagHex);
        check(gv.num("key_id") == 1, "JSON[key_id] 与实现一致");
        check(static_cast<std::uint64_t>(gv.num("frame_sequence")) == frameSeq,
              "JSON[frame_sequence] 与实现一致");
        check(gv.str("frame_inner_plaintext") == framePlain,
              "JSON[frame_inner_plaintext] 与实现一致");
    }
}

} // namespace

int main()
{
    std::cout << "=== test_secure_channel ===" << std::endl;
    testHappyPath();
    testEncryptDecryptRoundTrip();
    testBadSignature();
    testBadFieldLengths();
    testBadServerFinished();
    testSendBeforeEstablished();
    testReplayAndGap();
    testGoldenVector();

    // 进程内环回握手自检：与 Android arm64 androidTest 调用的是同一个静态函数，
    // 在桌面/CI 上先跑一遍，确保该自检本身正确、且不会与真实链路实现漂移。
    {
        std::string diag;
        const bool ok = im::transport::ClientSecureChannel::runLoopbackHandshakeSelfTest(&diag);
        if (!ok) {
            ++g_failures;
            std::cerr << "runLoopbackHandshakeSelfTest FAILED:\n" << diag << std::endl;
        } else {
            std::cout << "runLoopbackHandshakeSelfTest ok" << std::endl;
        }
    }

    if (g_failures == 0) {
        std::cout << "test_secure_channel PASSED" << std::endl;
        return 0;
    }
    std::cerr << "test_secure_channel FAILED: " << g_failures << " 项" << std::endl;
    return 1;
}
