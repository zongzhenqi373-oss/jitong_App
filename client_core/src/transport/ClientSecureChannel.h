#pragma once
// 应用层安全通道客户端（对齐 im_server Session.cpp 与 Android SecureChannel.kt）。
//
// ## 四步握手
// ```text
// C -> S  AppClientHello(1036)  version / X25519 pub / nonce(32) / randomId(16) / suite
// S -> C  AppServerHello(1037)  server pub / nonce / sessionId(16) / keyId / Ed25519 sig / suite
// C -> S  AppFinished(1038)     HMAC(clientFinishedKey, transcriptHash)
// S -> C  AppFinished(1039)     HMAC(serverFinishedKey, transcriptHash || clientVerify)
// 双向     AppEncryptedFrame(1040)  AES-256-GCM，收发 sequence 分离且严格 +1
// ```
//
// ## 字节级约定（与服务端逐字段一致，改动必须同步三端）
// ```text
// signingTranscript = "jitong-app-handshake-v1"
//                  || u32be(len(clientPayload)) || clientPayload
//                  || u32be(version) || serverPub(32) || serverNonce(32) || sessionId(16)
//                  || u32be(keyId)   || u32be(cipherSuite)
// transcriptHash   = sha256(u32be(len(clientPayload)) || clientPayload
//                        || u32be(len(serverPayload)) || serverPayload)
// salt             = sha256(clientNonce || serverNonce)
// info             = "jitong-app-channel-v1" || sessionId(16) || transcriptHash(32)
// material         = HKDF-SHA256(sharedSecret, salt, info, 136)
//                    [0,32) C2S key | [32,64) S2C key | [64,68) C nonce prefix
//                    [68,72) S nonce prefix | [72,104) C finished key | [104,136) S finished key
// clientVerify     = HMAC-SHA256(clientFinishedKey, transcriptHash)
// serverVerify     = HMAC-SHA256(serverFinishedKey, transcriptHash || clientVerify)
// plaintext        = le32(innerType) || payload
// nonce            = noncePrefix(4) || u64be(sequence)(8)
// aad              = "jitong-app-frame-v1" || u32be(version) || sessionId(16) || u64be(sequence)
// ```
//
// 安全约束：
//   - 未知 key_id 一律 fail-close；
//   - X25519 全零共享秘密拒绝；
//   - Finished 常量时间比较，未完成握手不得发送业务；
//   - 收发 sequence 从 1 起严格 +1，重复/跳号/回退/溢出均断线；
//   - 私钥、共享秘密、派生密钥与临时明文离开作用域即清零。

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "client_core/Protocol.h"

namespace im {
namespace transport {

class ClientSecureChannel {
public:
    enum class Status {
        NotStarted,
        HelloSent,    // 已发出 ClientHello，等待 ServerHello
                FinishedSent, // 已发出 Finished，等待 ServerFinished
        Established,
        Failed,
    };

    enum class Error {
        None,
        CryptoInit,       // OpenSSL 初始化/生成失败
        BadVersion,
        BadCipherSuite,
        BadFieldLength,
        BadKeyId,         // 未知或不受信的 key_id
        BadSignature,     // Ed25519 验签失败
        BadFinished,      // Finished 校验失败
        BadState,         // 握手顺序错误
        BadFrame,         // 加密帧字段非法
        Authentication,   // GCM 认证失败
        Sequence,         // sequence 非法
        Internal,
    };

    using KeyId = std::uint32_t;
    using Bytes = std::vector<unsigned char>;

    ClientSecureChannel();
    ~ClientSecureChannel();

    ClientSecureChannel(const ClientSecureChannel&) = delete;
    ClientSecureChannel& operator=(const ClientSecureChannel&) = delete;

    /**
     * 注入受信的服务端 Ed25519 身份公钥。
     * 未注入任何 key_id 时，任何 ServerHello 都会因 BadKeyId 失败（fail-close）。
     */
    void setTrustedIdentityKeys(std::map<KeyId, Bytes> keys);

    /**
     * 仅测试用：注入确定性的 X25519 私钥 / client nonce / random id，
     * 使握手输出可复现，用于三端 Golden 向量比对。
     */
    void setDeterministicTestVector(const Bytes& clientPrivateKey, const Bytes& clientNonce,
                                    const Bytes& randomId);

    /** 步骤 1：构造 AppClientHello 的 pb 序列化；同时保存本次握手状态。 */
    bool buildClientHello(std::string& outPayload);

    /** 步骤 2：处理 AppServerHello；成功时 outFinishedPayload 为 AppFinished 序列化。 */
    bool handleServerHello(const std::string& serverPayload, std::string& outFinishedPayload);

    /** 步骤 3：处理 AppServerFinished。 */
    bool handleServerFinished(const std::string& payload);

    /** 加密业务帧：返回 AppEncryptedFrame 的 pb 序列化。 */
    bool encrypt(proto::protType innerType, const std::string& payload, std::string& outFramePayload);

    /** 解密 AppEncryptedFrame 的 pb 序列化，输出内层协议号与 payload。 */
    bool decrypt(const std::string& framePayload, proto::protType& outType, std::string& outPayload);

    bool established() const { return m_status == Status::Established; }
    Status status() const { return m_status; }
    Error lastError() const { return m_lastError; }

    std::uint64_t sendSequence() const { return m_sendSequence; }
    std::uint64_t receiveSequence() const { return m_receiveSequence; }

    /** 当前 sessionId（握手成功后有效），用于日志与诊断。 */
    const Bytes& sessionId() const { return m_sessionId; }

    /** 供测试断言的中间值（仅在握手成功后有意义）。 */
    const Bytes& transcriptHash() const { return m_transcriptHash; }
    const Bytes& clientToServerKey() const { return m_clientToServerKey; }
    const Bytes& serverToClientKey() const { return m_serverToClientKey; }
    const Bytes& clientFinishedKey() const { return m_clientFinishedKey; }
    const Bytes& serverFinishedKey() const { return m_serverFinishedKey; }

    void reset();

    /**
     * 进程内自检（仅用于跨端/跨架构冒烟，例如 Android arm64 androidTest）。
     *
     * 在同一进程里用与服务端 Session.cpp 相同的字节级约定实现一个"服务端半程"
     * （临时 Ed25519 身份密钥 + X25519 + HKDF + AES-GCM），驱动一个真实的
     * ClientSecureChannel 走完整四步握手，并做一次加密业务帧的双向往返：
     *   ClientHello → ServerHello(验签) → ClientFinished → ServerFinished(校验)
     *   → client.encrypt → server.decrypt → server.encrypt → client.decrypt
     *
     * 全程使用本文件内与生产链路完全相同的加密原语，因此不会与真实服务端产生实现
     * 漂移；不涉及任何 socket/TLS，可在没有服务端的设备上验证握手与加密逻辑。
     *
     * @param outDiagnostics 可选，输出人类可读的分步诊断（每步 ok/FAIL）。
     * @return 全部步骤通过返回 true。
     */
    static bool runLoopbackHandshakeSelfTest(std::string* outDiagnostics = nullptr);

private:
    void fail(Error e);
    static void secureClear(Bytes& b);
    static void secureClear(std::string& s);

    std::map<KeyId, Bytes> m_identityKeys;

    // 本次握手状态
    Status m_status = Status::NotStarted;
    Error m_lastError = Error::None;

    // 测试向量：由 setDeterministicTestVector 注入，**不受 reset() 影响**，
    // 保证重复调用 buildClientHello 仍能产出可复现的 Golden。
    Bytes m_testPriv;
    Bytes m_testNonce;
    Bytes m_testRandomId;

    Bytes m_clientPriv;    // X25519 私钥 32B
    Bytes m_clientPub;     // X25519 公钥 32B
    Bytes m_clientNonce;   // 32B
    Bytes m_randomId;      // 16B
    std::string m_clientPayload; // AppClientHello 序列化（transcript 用）

    Bytes m_serverPub;
    Bytes m_serverNonce;
    Bytes m_sessionId;
    KeyId m_keyId = 0;
    std::string m_serverPayload;

    Bytes m_transcriptHash;
    Bytes m_clientToServerKey;
    Bytes m_serverToClientKey;
    Bytes m_clientNoncePrefix;
    Bytes m_serverNoncePrefix;
    Bytes m_clientFinishedKey;
    Bytes m_serverFinishedKey;

    std::uint64_t m_sendSequence = 0;
    std::uint64_t m_receiveSequence = 0;
};

} // namespace transport
} // namespace im
