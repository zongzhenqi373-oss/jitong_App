#include "Session.h"
#include "Server.h"
#include "Log.h"
#include "client_core/Protocol.h"
#include "im.pb.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <openssl/ssl.h>


namespace imsrv {
using namespace im::proto;

namespace {
using crypto::Bytes;

void appendU32(Bytes& output, std::uint32_t value)
{
    output.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
    output.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    output.push_back(static_cast<std::uint8_t>(value & 0xff));
}

void appendU64(Bytes& output, std::uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
}

void append(Bytes& output, const std::string& value)
{
    output.insert(output.end(), value.begin(), value.end());
}

void append(Bytes& output, const Bytes& value)
{
    output.insert(output.end(), value.begin(), value.end());
}

Bytes bytesOf(const std::string& value)
{
    return Bytes(value.begin(), value.end());
}

Bytes signingTranscript(const std::string& clientPayload, std::uint32_t version,
                        const Bytes& serverPublic, const Bytes& serverNonce,
                        const Bytes& sessionId, std::uint32_t keyId,
                        std::uint32_t cipherSuite)
{
    const std::string label = "jitong-app-handshake-v1";
    Bytes result(label.begin(), label.end());
    appendU32(result, static_cast<std::uint32_t>(clientPayload.size()));
    append(result, clientPayload);
    appendU32(result, version);
    append(result, serverPublic);
    append(result, serverNonce);
    append(result, sessionId);
    appendU32(result, keyId);
    appendU32(result, cipherSuite);
    return result;
}

Bytes finishedTranscriptHash(const std::string& clientPayload, const std::string& serverPayload)
{
    Bytes encoded;
    appendU32(encoded, static_cast<std::uint32_t>(clientPayload.size()));
    append(encoded, clientPayload);
    appendU32(encoded, static_cast<std::uint32_t>(serverPayload.size()));
    append(encoded, serverPayload);
    return crypto::sha256(encoded);
}

Bytes slice(const Bytes& input, std::size_t offset, std::size_t size)
{
    return Bytes(input.begin() + static_cast<std::ptrdiff_t>(offset),
                 input.begin() + static_cast<std::ptrdiff_t>(offset + size));
}

Bytes frameNonce(const Bytes& prefix, std::uint64_t sequence)
{
    if (prefix.size() != 4 || sequence == 0)
        throw std::invalid_argument("application frame nonce input invalid");
    Bytes nonce = prefix;
    appendU64(nonce, sequence);
    return nonce;
}

Bytes frameAad(const Bytes& sessionId, std::uint64_t sequence)
{
    const std::string label = "jitong-app-frame-v1";
    Bytes aad(label.begin(), label.end());
    appendU32(aad, APP_SECURITY_VERSION);
    append(aad, sessionId);
    appendU64(aad, sequence);
    return aad;
}
} // namespace

// 单包体上限（与 client_core MAX_PACK_LEN 一致）：防异常/恶意超大包
constexpr std::uint32_t MAX_PACK_LEN = 10 * 1024 * 1024;

Session::Session(asio::ip::tcp::socket socket, Server& server, asio::ssl::context& sslContext,
                 std::shared_ptr<asio::strand<asio::thread_pool::executor_type>> biz)
    : m_streamsocket(std::move(socket),sslContext)
    , m_strand(asio::make_strand(m_streamsocket.get_executor()))
    , m_biz(std::move(biz))
    , m_server(server)
    , m_handshakeTimer(m_streamsocket.get_executor())
{
}

void Session::start()
{
    doHandshake();
}

void Session::deliver(std::uint32_t type, const std::string& payload)
{
    auto self = shared_from_this();
    asio::post(m_strand, [this, self, type, payload]() {
        const bool handshakeType = type >= DEF_PROT_APP_CLIENT_HELLO &&
                                   type <= DEF_PROT_APP_SERVER_FINISHED;
        if (handshakeType) {
            enqueueRawFrame(type, payload);
            return;
        }
        if (m_securityState != SecurityState::Established) {
            failApplicationHandshake("安全通道建立前禁止发送业务帧");
            return;
        }
        try {
            enqueueRawFrame(DEF_PROT_APP_ENCRYPTED_FRAME, encryptBusinessFrame(type, payload));
        } catch (const std::exception& error) {
            log("[APP-SEC] 业务帧加密失败 type=", type, " error=", error.what());
            failApplicationHandshake("业务帧加密失败");
        }
    });
}

void Session::enqueueRawFrame(std::uint32_t type, const std::string& payload)
{
    auto buf = std::make_shared<std::vector<char>>(8 + payload.size());
    encodeLen32(static_cast<std::uint32_t>(4 + payload.size()), buf->data());
    encodeType32(type, buf->data() + 4);
    std::memcpy(buf->data() + 8, payload.data(), payload.size());
    m_writeQueue.push_back(std::move(buf));
    if (m_writeQueue.size() == 1) doWrite();
}

void Session::close()
{
    auto self = shared_from_this();
    asio::post(m_strand, [this, self]() {
        asio::error_code ec;
        m_streamsocket.lowest_layer().close(ec);
        onClosed();
    });
}

void Session::doReadHeader()
{
    auto self = shared_from_this();
    asio::async_read(m_streamsocket, asio::buffer(m_hdrBuf),
        asio::bind_executor(m_strand,
            [this, self](const asio::error_code& ec, std::size_t) {
                if (ec) { onClosed(); return; }
                const std::uint32_t bodyLen = decodeLen32(m_hdrBuf.data());
                if (bodyLen < 4 || bodyLen > MAX_PACK_LEN) { onClosed(); return; }
                doReadBody(bodyLen);
            }));
}

void Session::doReadBody(std::uint32_t bodyLen)
{
    auto self = shared_from_this();
    m_bodyBuf.resize(bodyLen);
    asio::async_read(m_streamsocket, asio::buffer(m_bodyBuf.data(), bodyLen),
        asio::bind_executor(m_strand,
            [this, self, bodyLen](const asio::error_code& ec, std::size_t) {
                if (ec) { onClosed(); return; }
                const std::uint32_t type = decodeType32(m_bodyBuf.data());
                std::string payload(m_bodyBuf.data() + 4, bodyLen - 4);
                if (m_securityState != SecurityState::Established) {
                    handleApplicationHandshake(type, payload);
                } else {
                    if (type != DEF_PROT_APP_ENCRYPTED_FRAME) {
                        log("[APP-SEC] 拒绝明文业务帧 type=", type);
                        failApplicationHandshake("安全通道建立后仅允许加密帧");
                        return;
                    }
                    handleEncryptedFrame(payload);
                }
                if (m_securityState == SecurityState::Closed) return;
                doReadHeader();
            }));
}

std::string Session::encryptBusinessFrame(std::uint32_t type, const std::string& payload)
{
    if (m_sendSequence == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("application send sequence exhausted");
    const std::uint64_t sequence = ++m_sendSequence;
    Bytes plaintext(4 + payload.size());
    plaintext[0] = static_cast<std::uint8_t>(type & 0xff);
    plaintext[1] = static_cast<std::uint8_t>((type >> 8) & 0xff);
    plaintext[2] = static_cast<std::uint8_t>((type >> 16) & 0xff);
    plaintext[3] = static_cast<std::uint8_t>((type >> 24) & 0xff);
    std::copy(payload.begin(), payload.end(), plaintext.begin() + 4);
    Bytes nonce = frameNonce(m_serverNoncePrefix, sequence);
    Bytes aad = frameAad(m_appSessionId, sequence);
    auto encrypted = crypto::aes256GcmEncrypt(m_serverToClientKey, nonce, plaintext, aad);

    AppEncryptedFrame frame;
    frame.set_version(APP_SECURITY_VERSION);
    frame.set_session_id(m_appSessionId.data(), m_appSessionId.size());
    frame.set_sequence(sequence);
    frame.set_ciphertext(encrypted.ciphertext.data(), encrypted.ciphertext.size());
    frame.set_tag(encrypted.tag.data(), encrypted.tag.size());
    crypto::secureClear(plaintext);
    crypto::secureClear(nonce);
    crypto::secureClear(aad);
    return frame.SerializeAsString();
}

void Session::handleEncryptedFrame(const std::string& payload)
{
    try {
        AppEncryptedFrame frame;
        if (!frame.ParseFromString(payload) || frame.version() != APP_SECURITY_VERSION ||
            !crypto::constantTimeEqual(bytesOf(frame.session_id()), m_appSessionId) ||
            frame.tag().size() != APP_GCM_TAG_LEN || frame.ciphertext().size() < 4 ||
            frame.sequence() == 0 || frame.sequence() != m_receiveSequence + 1) {
            failApplicationHandshake("加密帧字段、会话或序列号非法");
            return;
        }
        Bytes nonce = frameNonce(m_clientNoncePrefix, frame.sequence());
        Bytes aad = frameAad(m_appSessionId, frame.sequence());
        auto plaintext = crypto::aes256GcmDecrypt(
            m_clientToServerKey, nonce, bytesOf(frame.ciphertext()), aad, bytesOf(frame.tag()));
        crypto::secureClear(nonce);
        crypto::secureClear(aad);
        if (!plaintext || plaintext->size() < 4) {
            failApplicationHandshake("加密帧认证失败");
            return;
        }
        const std::uint32_t innerType = static_cast<std::uint32_t>((*plaintext)[0]) |
            (static_cast<std::uint32_t>((*plaintext)[1]) << 8) |
            (static_cast<std::uint32_t>((*plaintext)[2]) << 16) |
            (static_cast<std::uint32_t>((*plaintext)[3]) << 24);
        if (innerType >= DEF_PROT_APP_CLIENT_HELLO && innerType <= DEF_PROT_APP_ENCRYPTED_FRAME) {
            crypto::secureClear(*plaintext);
            failApplicationHandshake("加密帧内嵌安全控制协议非法");
            return;
        }
        std::string innerPayload(plaintext->begin() + 4, plaintext->end());
        crypto::secureClear(*plaintext);
        m_receiveSequence = frame.sequence();
        log("[APP-SEC] 已认证解密业务帧 seq=", m_receiveSequence,
            " innerType=", innerType, " payloadBytes=", innerPayload.size());
        auto self = shared_from_this();
        asio::post(*m_biz, [this, self, innerType, payload = std::move(innerPayload)]() mutable {
            m_server.dispatchPacket(shared_from_this(), innerType, std::move(payload));
        });
    } catch (const std::exception& error) {
        log("[APP-SEC] 加密帧处理异常 error=", error.what());
        failApplicationHandshake("加密帧处理异常");
    }
}

void Session::handleApplicationHandshake(std::uint32_t type, const std::string& payload)
{
    if (payload.size() > APP_MAX_HANDSHAKE_PAYLOAD) {
        failApplicationHandshake("握手负载超过上限");
        return;
    }
    if (m_securityState == SecurityState::WaitClientHello && type == DEF_PROT_APP_CLIENT_HELLO) {
        handleClientHello(payload);
        return;
    }
    if (m_securityState == SecurityState::WaitClientFinished && type == DEF_PROT_APP_CLIENT_FINISHED) {
        handleClientFinished(payload);
        return;
    }
    log("[APP-SEC] 非法握手帧 state=", securityStateName(),
        " actualType=", type,
        " expectedType=",
        m_securityState == SecurityState::WaitClientHello
            ? DEF_PROT_APP_CLIENT_HELLO
            : DEF_PROT_APP_CLIENT_FINISHED,
        " payloadBytes=", payload.size());
    failApplicationHandshake("握手状态或协议顺序非法");
}

const char* Session::securityStateName() const
{
    switch (m_securityState) {
    case SecurityState::TlsHandshake: return "TLS_HANDSHAKE";
    case SecurityState::WaitClientHello: return "WAIT_CLIENT_HELLO";
    case SecurityState::WaitClientFinished: return "WAIT_CLIENT_FINISHED";
    case SecurityState::Established: return "ESTABLISHED";
    case SecurityState::Closed: return "CLOSED";
    }
    return "UNKNOWN";
}

void Session::handleClientHello(const std::string& payload)
{
    AppClientHello hello;
    if (!hello.ParseFromString(payload) || hello.version() != APP_SECURITY_VERSION ||
        hello.cipher_suite() != APP_CIPHER_X25519_ED25519_HKDF_SHA256_AES_256_GCM ||
        hello.client_ephemeral_public_key().size() != APP_X25519_KEY_LEN ||
        hello.client_nonce().size() != APP_NONCE_LEN ||
        hello.client_random_id().size() != APP_RANDOM_ID_LEN) {
        failApplicationHandshake("ClientHello字段非法");
        return;
    }

    try {
        auto ephemeral = crypto::generateX25519KeyPair();
        Bytes serverNonce = crypto::randomBytes(APP_NONCE_LEN);
        m_appSessionId = crypto::randomBytes(APP_SESSION_ID_LEN);
        const auto keyId = m_server.appIdentityKeyId();
        Bytes toSign = signingTranscript(payload, APP_SECURITY_VERSION, ephemeral.publicKey,
                                         serverNonce, m_appSessionId, keyId, APP_CIPHER_SUITE_V1);
        Bytes signature = crypto::ed25519Sign(m_server.appIdentityPrivateKey(), toSign);

        AppServerHello response;
        response.set_version(APP_SECURITY_VERSION);
        response.set_server_ephemeral_public_key(ephemeral.publicKey.data(), ephemeral.publicKey.size());
        response.set_server_nonce(serverNonce.data(), serverNonce.size());
        response.set_session_id(m_appSessionId.data(), m_appSessionId.size());
        response.set_key_id(keyId);
        response.set_signature(signature.data(), signature.size());
        response.set_cipher_suite(APP_CIPHER_X25519_ED25519_HKDF_SHA256_AES_256_GCM);
        const std::string serverPayload = response.SerializeAsString();
        m_transcriptHash = finishedTranscriptHash(payload, serverPayload);

        const Bytes clientPublic = bytesOf(hello.client_ephemeral_public_key());
        Bytes sharedSecret = crypto::x25519SharedSecret(ephemeral.privateKey, clientPublic);
        Bytes saltInput = bytesOf(hello.client_nonce());
        append(saltInput, serverNonce);
        Bytes salt = crypto::sha256(saltInput);
        const std::string infoLabel = "jitong-app-channel-v1";
        Bytes info(infoLabel.begin(), infoLabel.end());
        append(info, m_appSessionId);
        append(info, m_transcriptHash);
        Bytes material = crypto::hkdfSha256(sharedSecret, salt, info, 136);
        m_clientToServerKey = slice(material, 0, 32);
        m_serverToClientKey = slice(material, 32, 32);
        m_clientNoncePrefix = slice(material, 64, 4);
        m_serverNoncePrefix = slice(material, 68, 4);
        m_clientFinishedKey = slice(material, 72, 32);
        m_serverFinishedKey = slice(material, 104, 32);

        crypto::secureClear(ephemeral.privateKey);
        crypto::secureClear(sharedSecret);
        crypto::secureClear(material);
        crypto::secureClear(salt);
        m_securityState = SecurityState::WaitClientFinished;
        deliver(DEF_PROT_APP_SERVER_HELLO, serverPayload);
        log("[APP-SEC] ServerHello已发送 keyId=", keyId);
    } catch (const std::exception& error) {
        log("[APP-SEC] ClientHello处理失败 error=", error.what());
        failApplicationHandshake("密码学处理失败");
    }
}

void Session::handleClientFinished(const std::string& payload)
{
    AppFinished finished;
    if (!finished.ParseFromString(payload) || finished.verify_data().size() != APP_FINISHED_LEN) {
        failApplicationHandshake("ClientFinished字段非法");
        return;
    }
    const Bytes actual = bytesOf(finished.verify_data());
    const Bytes expected = crypto::hmacSha256(m_clientFinishedKey, m_transcriptHash);
    if (!crypto::constantTimeEqual(actual, expected)) {
        failApplicationHandshake("ClientFinished校验失败");
        return;
    }

    Bytes serverInput = m_transcriptHash;
    append(serverInput, actual);
    const Bytes serverVerify = crypto::hmacSha256(m_serverFinishedKey, serverInput);
    AppFinished response;
    response.set_verify_data(serverVerify.data(), serverVerify.size());
    m_securityState = SecurityState::Established;
    m_handshakeTimer.cancel();
    deliver(DEF_PROT_APP_SERVER_FINISHED, response.SerializeAsString());
    log("[APP-SEC] 应用层安全握手成功 sessionBytes=", m_appSessionId.size());
}

void Session::failApplicationHandshake(const char* reason)
{
    if (m_securityState == SecurityState::Closed) return;
    log("[APP-SEC] 握手失败: ", reason);
    m_securityState = SecurityState::Closed;
    close();
}

void Session::startApplicationHandshakeTimeout()
{
    auto self = shared_from_this();
    // Android 模拟器首次加载系统密码学 Provider、生成 X25519 密钥时可能明显慢于
    // 真机。这里是整个应用层握手的总期限，不是网络读超时；5 秒会误杀正常首连。
    m_handshakeTimer.expires_after(std::chrono::seconds(15));
    m_handshakeTimer.async_wait(asio::bind_executor(m_strand,
        [this, self](const asio::error_code& error) {
            if (!error && m_securityState != SecurityState::Established)
                failApplicationHandshake("握手超时");
        }));
}

void Session::doWrite()
{
    // 本函数只允许在 m_strand 中调用。队首 buffer 必须一直保留到 async_write
    // 回调结束；提前 pop 会使 Asio 持有悬空 buffer，并让回调再次 pop 空队列。
    if (m_writeQueue.empty()) {
        if (m_closeAfterWrite) close();
        return;
    }

    auto self = shared_from_this();
    asio::async_write(m_streamsocket, asio::buffer(*m_writeQueue.front()),
        asio::bind_executor(m_strand,
            [this, self](const asio::error_code& ec, std::size_t) {
                if (ec) { onClosed(); return; }
                m_writeQueue.pop_front();
                if (!m_writeQueue.empty()) {
                    doWrite();
                } else if (m_closeAfterWrite) {
                    close();
                }
            }));
}

void Session::onClosed()
{
    // 在 strand 上执行，保证只收尾一次
    if (m_closedNotified) return;
    m_closedNotified = true;
    m_securityState = SecurityState::Closed;
    m_handshakeTimer.cancel();
    crypto::secureClear(m_clientFinishedKey);
    crypto::secureClear(m_serverFinishedKey);
    crypto::secureClear(m_clientToServerKey);
    crypto::secureClear(m_serverToClientKey);
    crypto::secureClear(m_transcriptHash);
    m_sendSequence = 0;
    m_receiveSequence = 0;

    asio::error_code ec;
    m_streamsocket.lowest_layer().shutdown(
        asio::ip::tcp::socket::shutdown_both,
        ec
    );
    m_streamsocket.lowest_layer().close(ec);
    m_server.onSessionClosed(shared_from_this());
}

void Session::doHandshake()
{
    auto self = shared_from_this();

    m_streamsocket.async_handshake(
        asio::ssl::stream_base::server,
        asio::bind_executor(
            m_strand,
            [this, self](const asio::error_code& ec) {
                if (ec) {
                    std::cerr << "handshake failed: " << ec.message() << std::endl;
                    onClosed();
                    return;
                }

                SSL* ssl = m_streamsocket.native_handle();
                std::cout << "handshake success: " << SSL_get_version(ssl) << "  " 
                << "cipher: " << SSL_get_cipher_name(ssl) << std::endl;
                m_securityState = SecurityState::WaitClientHello;
                startApplicationHandshakeTimeout();
                doReadHeader();
            }
        )
    );
}

void Session::closeAfterWrite()
{
    auto self = shared_from_this();
    asio::post(m_strand, [this, self]() {
        m_closeAfterWrite = true;
        if (m_writeQueue.empty()) 
            close();
    });
}

} // namespace imsrv
