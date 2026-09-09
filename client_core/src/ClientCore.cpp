#include "client_core/ClientCore.h"
#include "client_core/IStorage.h"
#include "TcpTransport.h"
#include "transport/ClientSecureChannel.h"
#include "transport/DeviceProof.h"
#include "im.pb.h"
#include "sha256.h"
#if defined(CLIENT_CORE_WITH_MEDIA)
#include "httplib.h"
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

namespace im {

using namespace proto;

namespace {
// pb payload 解析（data/len 为去掉 4B 协议号后的包体）
template <typename T>
bool parsePayload(const char* data, std::size_t len, T& out)
{
    return out.ParseFromArray(data, static_cast<int>(len));
}

std::int64_t steadyNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 生成消息唯一 id（msg_id）：时间戳 + 自增计数 + 随机数，漫游/去重/回执关联用
std::string makeMsgId()
{
    static std::atomic<std::uint64_t> counter{0};
    static std::random_device rd;
    const std::uint64_t rand64 = (static_cast<std::uint64_t>(rd()) << 32) | rd();
    const std::uint64_t ts = static_cast<std::uint64_t>(steadyNowMs());
    const std::uint64_t seq = counter.fetch_add(1);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%013llx%05llx%016llx",
                  static_cast<unsigned long long>(ts),
                  static_cast<unsigned long long>(seq & 0xFFFFF),
                  static_cast<unsigned long long>(rand64));
    return buf;
}

// 生成一个进程内唯一的设备标识（default device id）。
// 设备证明模型下，每个 ClientCore 实例代表一台逻辑设备，必须有各自稳定且互不
// 冲突的 device_id；服务端会把 (userId, device_id) 绑定到某一把设备公钥，若两台
// 不同设备复用同一个 device_id 但公钥不同，服务端会以“device_id已绑定其他设备
// 密钥”拒绝。生产环境应改为按安装持久化的稳定 id，这里给出安全的默认值。
std::string makeDefaultDeviceId()
{
    static std::atomic<std::uint64_t> counter{0};
    static std::random_device rd;
    const std::uint64_t rand64 = (static_cast<std::uint64_t>(rd()) << 32) | rd();
    const std::uint64_t seq = counter.fetch_add(1);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "client-core-%016llx%04llx",
                  static_cast<unsigned long long>(rand64),
                  static_cast<unsigned long long>(seq & 0xFFFF));
    return buf;
}

// 从 HttpFileServer 的上传响应体（形如 {"file_id":"...","sha256":"...","size":N,
// "content_type":"..."}）里抠一个字符串字段。响应格式完全由我们自己的服务端生成、
// 值都是 hex/mime 这类不含引号转义的简单字符串，不需要引入完整 JSON 库。
std::string extractJsonStringField(const std::string& json, const std::string& key)
{
    const std::string needle = "\"" + key + "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}
} // namespace

ClientCore::ClientCore(ClientConfig config)
    : m_tlsServerName(config.tlsServerName)
    , m_caFile(config.caFile)
    , m_httpPort(config.httpPort)
{
    m_identityKeys = std::move(config.identityKeys);
    m_secureChannel = std::make_unique<transport::ClientSecureChannel>();
    m_deviceKey = std::make_unique<transport::DeviceProofKey>();
    // 每个 ClientCore 实例默认视为一台独立设备，分配唯一 device_id，避免多实例
    // 复用同一个 device_id 时因设备公钥不同被服务端拒绝。可由上层覆盖为持久化 id。
    m_deviceId = makeDefaultDeviceId();

    //开发阶段如果证书SAN是 IP:127.0.0.1，serverName可以暂时传 "127.0.0.1"。
    //不能使用 verify_none 或永远返回true的验证回调。
    m_transport = std::make_unique<TcpTransport>(
        std::move(config.tlsServerName),
        std::move(config.caFile)
    );

    // R2-F02：把配置里的 SPKI pin 接到传输层。非空即启用证书公钥固定；为空则不启用
    // （仅依赖 CA 链 + 主机名）。这样生产路径才真正具备 pinning，而不只是测试里直接
    // 调 setSpkiPins 才生效。
    if (!config.spkiPins.empty()) {
        m_transport->setSpkiPins(std::move(config.spkiPins));
    }

    initFunArr();

    // type/payload 已由 FrameCodec 解析，业务层不再处理端序
    m_transport->setPacketHandler([this](proto::protType type, const char* payload, std::size_t len) {
        // 任何入站包都刷新活跃时间（含心跳回复）
        m_lastRecvMs.store(steadyNowMs());
        dispatchPacket(type, payload, len);
    });
    m_transport->setCloseHandler([this]() {
        if (auto* sink = m_authSink.load()) sink->onConnectionClosed();
        if (auto* ev = m_events.load()) ev->onConnectionClosed();
    });
}

ClientCore::~ClientCore()
{
    // 先停心跳线程（避免其在析构中调用 transport），再断连
    stopHeartbeat();
}

void ClientCore::setEventSink(IClientEvents* events) { m_events.store(events); }
void ClientCore::setStorage(IStorage* storage) { m_storage.store(storage); }
void ClientCore::setAuthProtocolSink(IAuthProtocolSink* sink) { m_authSink.store(sink); }

void ClientCore::sendAuthRaw(protType type, const std::string& payload)
{
    // 与业务帧同一发送路径：安全通道建立后自动加密为 1040。
    sendPacket(type, payload);
}

// ---------------- 连接管理 ----------------

bool ClientCore::connectToServer(const std::string& ip, std::uint16_t port)
{
    if (!m_transport->connect(ip, port)) return false;
    m_host = ip;
    if (m_httpPort == 0) m_httpPort = static_cast<std::uint16_t>(port + 1);
    m_lastRecvMs.store(steadyNowMs());

    // 服务端强制 TLS 之上的应用层安全握手：未完成握手就发业务帧会被 fail-close。
    // 因此握手必须在 startHeartbeat() 之前完成，否则明文心跳会立刻触发断开。
    if (!performAppHandshake()) {
        std::cerr << "[APP-SEC] 应用层安全握手失败，关闭连接" << std::endl;
        m_transport->close();
        return false;
    }

    startHeartbeat();
    return true;
}

void ClientCore::disconnect()
{
    stopHeartbeat();
    m_transport->close();
}

bool ClientCore::isConnected() const
{
    return m_transport->isOpen();
}

std::vector<unsigned char> ClientCore::appSessionId() const
{
    if (!m_secureChannel || !m_secureChannel->established()) return {};
    const auto& sid = m_secureChannel->sessionId();
    return std::vector<unsigned char>(sid.begin(), sid.end());
}

// ---------------- 协议分发 ----------------

void ClientCore::initFunArr()
{
    m_dealFunArr[DEF_PROT_REGISTER_RS    - DEF_BASE] = &ClientCore::onRegisterRs;
    m_dealFunArr[DEF_PROT_LOGIN_RS       - DEF_BASE] = &ClientCore::onLoginRs;
    m_dealFunArr[DEF_PROT_FRIEND_INFO    - DEF_BASE] = &ClientCore::onFriendInfoPkt;
    m_dealFunArr[DEF_PROT_CHAT_INFO_RS   - DEF_BASE] = &ClientCore::onChatInfoRs;
    m_dealFunArr[DEF_PROT_CHAT_INFO_RQ   - DEF_BASE] = &ClientCore::onChatInfoRq;
    m_dealFunArr[DEF_PROT_ADD_FRIEND_RS  - DEF_BASE] = &ClientCore::onAddFriRs;
    m_dealFunArr[DEF_PROT_ADD_FRIEND_RQ  - DEF_BASE] = &ClientCore::onAddFriRq;
    m_dealFunArr[DEF_PROT_FRIEND_OFFLINE - DEF_BASE] = &ClientCore::onFriendOfflinePkt;
    m_dealFunArr[DEF_PROT_HEARTBEAT_RS   - DEF_BASE] = &ClientCore::onHeartbeatRs;
    m_dealFunArr[DEF_PROT_KICKED_OFFLINE - DEF_BASE] = &ClientCore::onKickedOfflinePkt;
    m_dealFunArr[DEF_PROT_ROAM_CONV_RS   - DEF_BASE] = &ClientCore::onRoamConvRs;
    m_dealFunArr[DEF_PROT_ROAM_MSG_RS    - DEF_BASE] = &ClientCore::onRoamMsgRs;
    // P5-T06：Token 认证响应
    m_dealFunArr[DEF_PROT_TOKEN_LOGIN_RS   - DEF_BASE] = &ClientCore::onTokenLoginRs;
    m_dealFunArr[DEF_PROT_TOKEN_REFRESH_RS - DEF_BASE] = &ClientCore::onRefreshTokenRs;
    m_dealFunArr[DEF_PROT_LOGOUT_RS        - DEF_BASE] = &ClientCore::onLogoutRs;
}

void ClientCore::dispatchPacket(protType type, const char* payload, std::size_t len)
{
    // 协议号与 payload 已由传输层用 FrameCodec 解析，这里只做分发
    if (!payload && len > 0) return;

    // ---------------- 应用层安全通道帧（P4） ----------------
    if (type == DEF_PROT_APP_SERVER_HELLO) {
        handleAppServerHello(payload, len);
        return;
    }
    if (type == DEF_PROT_APP_SERVER_FINISHED) {
        handleAppServerFinished(payload, len);
        return;
    }
    if (type == DEF_PROT_APP_ENCRYPTED_FRAME) {
        handleAppEncryptedFrame(payload, len);
        return;
    }

    // 安全通道建立之后，服务端只会发 1040；外层收到任何明文帧说明链路异常，丢弃不分发
    if (m_secureChannel && m_secureChannel->established()) {
        std::cerr << "[APP-SEC] 安全通道建立后收到明文帧，丢弃 type=" << type << std::endl;
        return;
    }

    dispatchBusiness(type, payload, len);
}

void ClientCore::dispatchBusiness(protType type, const char* payload, std::size_t len)
{
    // 范围校验（防越界访问函数指针数组）
    if (type < DEF_BASE) return;
    const std::size_t index = type - DEF_BASE;
    if (index >= static_cast<std::size_t>(DEF_PROT_COUNT)) return;

    DealFun pFun = m_dealFunArr[index];
    if (pFun) (this->*pFun)(payload, len);
}

void ClientCore::sendPacket(protType type, const std::string& payload)
{
    // 安全通道建立后，所有业务帧必须先加密再封装成 1040 发出；
    // 握手帧（1036/1038）走明文，这是协议本身规定的顺序。
    //
    // R2-F01：分配 sequence + 加密 + 组 body + 入队必须在**同一线程序列**里串行发生。
    // sendPacket 可能被心跳线程、业务/UI 线程并发调用，若各自在自己的线程里做
    // encrypt()（内部 ++m_sendSequence）会产生数据竞争，轻则序号乱序被服务端按
    // 严格 +1 校验 fail-close，重则 AES-GCM nonce 重用破坏机密性。这里把整段加密
    // 逻辑 post 到 Transport 的 IO 线程（与入站解密/握手同一 executor），彻底串行化。
    if (m_secureChannel && m_secureChannel->established()) {
        m_transport->postToIo([this, type, payload]() {
            // 已切到 IO 线程：encrypt() 与入站 decrypt()、握手状态变更严格串行
            if (!m_secureChannel || !m_secureChannel->established()) {
                return; // 期间连接被关闭/重置，直接丢弃
            }
            std::string frame;
            if (!m_secureChannel->encrypt(type, payload, frame)) {
                std::cerr << "[APP-SEC] 加密失败，丢弃 type=" << type << std::endl;
                return;
            }
            sendRawPacket(DEF_PROT_APP_ENCRYPTED_FRAME, frame);
        });
        return;
    }
    sendRawPacket(type, payload);
}

void ClientCore::sendRawPacket(protType type, const std::string& payload)
{
    std::string body;
    body.resize(sizeof(protType) + payload.size());
    encodeType32(type, body.data());
    if (!payload.empty()) {
        std::memcpy(body.data() + sizeof(protType), payload.data(), payload.size());
    }
    // R2-F01：不再忽略发送结果。send 内部会把帧 post 到 IO 线程写队列，返回值反映
    // 「是否已入队」。未连接/超限等未入队情形要记录，避免业务以为已发出。
    const auto result = m_transport->send(body.data(), body.size());
    if (result != TcpTransport::SendResult::Ok) {
        std::cerr << "[发送] 未入队 type=" << type
                  << " result=" << static_cast<int>(result) << std::endl;
    }
}

// ---------------- 应用层安全握手（P4） ----------------

bool ClientCore::performAppHandshake()
{
    if (!m_secureChannel) return false;

    m_secureChannel->reset();
    m_secureChannel->setTrustedIdentityKeys(m_identityKeys);

    std::string hello;
    if (!m_secureChannel->buildClientHello(hello)) {
        std::cerr << "[APP-SEC] 构造 ClientHello 失败" << std::endl;
        return false;
    }

    auto promise = std::make_shared<std::promise<bool>>();
    std::future<bool> future = promise->get_future();
    {
        std::lock_guard<std::mutex> lk(m_handshakeMutex);
        m_handshakePromise = promise;
    }

    sendRawPacket(DEF_PROT_APP_CLIENT_HELLO, hello);

    // 服务端握手超时为 15s，这里留出同样上界，避免连接线程无限等待
    if (future.wait_for(std::chrono::seconds(15)) != std::future_status::ready) {
        std::cerr << "[APP-SEC] 应用层握手超时" << std::endl;
        std::lock_guard<std::mutex> lk(m_handshakeMutex);
        m_handshakePromise.reset();
        return false;
    }
    return future.get();
}

void ClientCore::completeHandshake(bool ok)
{
    std::shared_ptr<std::promise<bool>> promise;
    {
        std::lock_guard<std::mutex> lk(m_handshakeMutex);
        promise = m_handshakePromise;
        m_handshakePromise.reset();
    }
    if (promise) promise->set_value(ok);
}

void ClientCore::handleAppServerHello(const char* data, std::size_t len)
{
    if (!m_secureChannel) {
        completeHandshake(false);
        return;
    }
    std::string finished;
    if (!m_secureChannel->handleServerHello(std::string(data, len), finished)) {
        std::cerr << "[APP-SEC] ServerHello 校验失败，断开连接" << std::endl;
        completeHandshake(false);
        m_transport->close();
        return;
    }
    sendRawPacket(DEF_PROT_APP_CLIENT_FINISHED, finished);
}

void ClientCore::handleAppServerFinished(const char* data, std::size_t len)
{
    if (!m_secureChannel) {
        completeHandshake(false);
        return;
    }
    if (!m_secureChannel->handleServerFinished(std::string(data, len))) {
        std::cerr << "[APP-SEC] ServerFinished 校验失败，断开连接" << std::endl;
        completeHandshake(false);
        m_transport->close();
        return;
    }
    completeHandshake(true);
}

void ClientCore::handleAppEncryptedFrame(const char* data, std::size_t len)
{
    if (!m_secureChannel || !m_secureChannel->established()) {
        std::cerr << "[APP-SEC] 未建立安全通道却收到加密帧" << std::endl;
        m_transport->close();
        return;
    }
    protType innerType = 0;
    std::string inner;
    if (!m_secureChannel->decrypt(std::string(data, len), innerType, inner)) {
        // 解密/认证失败：与服务端一致 fail-close，绝不上抛未认证明文
        std::cerr << "[APP-SEC] 加密帧解密失败，断开连接" << std::endl;
        m_transport->close();
        return;
    }
    // 内层协议号已由 decrypt 排除 1036..1040。
    // 必须走 dispatchBusiness：再走 dispatchPacket 会被"已建立通道拒绝明文"误杀。
    dispatchBusiness(innerType, inner.data(), inner.size());
}

// ---------------- 业务请求 ----------------

void ClientCore::sendRegister(const std::string& nickUtf8, const std::string& tel, const std::string& pass)
{
    im::proto::RegisterRq rq;
    rq.set_nick(utf8Truncate(nickUtf8, USER_NICK_LEN - 1));
    rq.set_tel(utf8Truncate(tel, USER_TEL_LEN - 1));
    // 对齐 QQNT：密码绝不原文上链路，客户端先 SHA-256 一次（固定 64 字符 hex）
    rq.set_pass(sha256Hex(pass));
    sendPacket(DEF_PROT_REGISTER_RQ, rq.SerializeAsString());
}

void ClientCore::sendLogin(const std::string& tel, const std::string& pass)
{
    im::proto::LoginRq rq;
    const std::string telField = utf8Truncate(tel, USER_TEL_LEN - 1);
    // 对齐 QQNT：传输的是密码哈希，而非明文
    const std::string passProof = sha256Hex(pass);
    rq.set_tel(telField);
    rq.set_pass(passProof);
    rq.set_device_id(m_deviceId);
    rq.set_device_name("C++ ClientCore");
    rq.set_client_version("client-core-0.5.0");

    // 设备证明（P-256）：服务端要求密码登录携带设备公钥并对本次应用会话签名，
    // 否则以 LOGIN_INVALID 拒绝。签名内容与服务端 deviceproof::message 逐字节一致：
    //   operation        = "password-login"
    //   appSessionId     = 应用层安全通道 sessionId（握手成功后有效）
    //   deviceId         = m_deviceId
    //   credentialBinding= tel || '\0' || sha256Hex(pass)
    //   publicKey        = 设备 P-256 X.509 SPKI DER（本操作绑定公钥）
    if (!m_deviceKey || (!m_deviceKey->valid() && !m_deviceKey->generate())) {
        std::cerr << "[认证] 设备密钥生成失败，无法登录" << std::endl;
        return;
    }
    const auto publicKeyDer = m_deviceKey->publicKeyDer();
    transport::DeviceProofKey::Bytes appSessionId;
    if (m_secureChannel) {
        const auto& sid = m_secureChannel->sessionId();
        appSessionId.assign(sid.begin(), sid.end());
    }
    const std::string credentialBinding = telField + std::string(1, '\0') + passProof;
    const auto message = transport::buildDeviceProofMessage(
        "password-login", appSessionId, m_deviceId, credentialBinding, publicKeyDer);
    const auto signature = m_deviceKey->sign(message);
    if (publicKeyDer.empty() || signature.empty()) {
        std::cerr << "[认证] 设备签名失败，无法登录" << std::endl;
        return;
    }
    rq.set_device_public_key(publicKeyDer.data(), publicKeyDer.size());
    rq.set_device_signature(signature.data(), signature.size());

    sendPacket(DEF_PROT_LOGIN_RQ, rq.SerializeAsString());
}

void ClientCore::sendChatMessage(int friId, const std::string& msgUtf8)
{
    const std::string msg = utf8Truncate(msgUtf8, CHAT_MSG_LEN - 1);

    im::proto::ChatInfoRq rq;
    rq.set_myid(m_myId);
    rq.set_friid(friId);
    rq.set_msg(msg);
    rq.set_type(im::proto::TEXT);
    rq.set_msg_id(makeMsgId());
    sendPacket(DEF_PROT_CHAT_INFO_RQ, rq.SerializeAsString());

    // 本地持久化：发出的消息
    if (m_myId > 0) {
        if (auto* st = m_storage.load()) {
            st->saveChatMessage(m_myId, friId, true, msg,
                                static_cast<std::int64_t>(std::time(nullptr)));
        }
    }
}

void ClientCore::sendAddFriendRequest(const std::string& friNickUtf8)
{
    im::proto::AddFriendRq rq;
    rq.set_myid(m_myId);
    rq.set_mynick(m_nick);
    rq.set_frinick(utf8Truncate(friNickUtf8, USER_NICK_LEN - 1));
    sendPacket(DEF_PROT_ADD_FRIEND_RQ, rq.SerializeAsString());
}

void ClientCore::answerAddFriend(int destId, const std::string& destNickUtf8, bool agree)
{
    im::proto::AddFriendRs rs;
    rs.set_result(agree ? ADD_FRIEND_AGREE : ADD_FRIEND_REJECT);
    rs.set_destid(destId);
    rs.set_myid(m_myId);
    rs.set_mynick(m_nick);
    rs.set_destnick(utf8Truncate(destNickUtf8, USER_NICK_LEN - 1));
    sendPacket(DEF_PROT_ADD_FRIEND_RS, rs.SerializeAsString());
}

void ClientCore::sendOfflineNotify()
{
    im::proto::FriendOffline pkt;
    pkt.set_offlineid(m_myId);
    sendPacket(DEF_PROT_FRIEND_OFFLINE, pkt.SerializeAsString());
}

void ClientCore::sendRoamConvRq()
{
    im::proto::RoamConvRq rq;
    rq.set_myid(m_myId); // 占位，服务端以 session 登录态为准
    sendPacket(DEF_PROT_ROAM_CONV_RQ, rq.SerializeAsString());
}

void ClientCore::sendRoamMsgRq(int peerId, std::int64_t beforeSeq, int limit)
{
    im::proto::RoamMsgRq rq;
    rq.set_myid(m_myId);
    rq.set_peer_id(peerId);
    rq.set_before_seq(beforeSeq);
    rq.set_limit(limit);
    sendPacket(DEF_PROT_ROAM_MSG_RQ, rq.SerializeAsString());
}

#if defined(CLIENT_CORE_WITH_MEDIA)
std::string ClientCore::uploadMedia(const std::string& localPath, int receiverId, bool isImage,
                                    const MediaProgress& onProgress)
{
    if (m_tlsServerName.empty() || m_httpPort == 0 || m_accessToken.empty()) return "";

    std::ifstream ifs(localPath, std::ios::binary | std::ios::ate);
    if (!ifs) return "";
    const auto sizeSigned = static_cast<std::int64_t>(ifs.tellg());
    if (sizeSigned <= 0 || sizeSigned > proto::FILE_MAX_SIZE) return "";
    const auto size = static_cast<std::size_t>(sizeSigned);
    ifs.seekg(0, std::ios::beg);

    httplib::SSLClient cli(m_tlsServerName, m_httpPort);
    if (!m_caFile.empty()) cli.set_ca_cert_path(m_caFile);
    if (!m_host.empty() && m_host != m_tlsServerName) {
        // SNI/证书校验走 m_tlsServerName，实际拨号走 connectToServer 时传入的地址，
        // 跟 TcpTransport 的 TLS 校验方式保持一致
        cli.set_hostname_addr_map({{m_tlsServerName, m_host}});
    }
    cli.set_connection_timeout(10);
    cli.set_read_timeout(120);
    cli.set_write_timeout(120);

    const std::string fileName = std::filesystem::path(localPath).filename().string();
    const httplib::Headers headers = {
        {"Authorization", "Bearer " + m_accessToken},
        {"X-Device-Id", m_deviceId},
        {"X-File-Name", fileName},
        {"X-Receiver-Id", std::to_string(receiverId)},
    };
    const std::string contentType = isImage ? "image/jpeg" : "application/octet-stream";

    // 流式上传：边读本地文件边写 sink，不整体载入内存
    auto provider = [&ifs](std::size_t /*offset*/, std::size_t length, httplib::DataSink& sink) -> bool {
        std::vector<char> buf(64 * 1024);
        std::size_t remaining = length;
        while (remaining > 0 && ifs) {
            const std::size_t chunk = std::min(remaining, buf.size());
            ifs.read(buf.data(), static_cast<std::streamsize>(chunk));
            const auto got = ifs.gcount();
            if (got <= 0) break;
            if (!sink.write(buf.data(), static_cast<std::size_t>(got))) return false;
            remaining -= static_cast<std::size_t>(got);
        }
        return true;
    };

    httplib::UploadProgress progressCb = nullptr;
    if (onProgress) {
        progressCb = [&onProgress](std::size_t current, std::size_t total) -> bool {
            onProgress(static_cast<std::int64_t>(current), static_cast<std::int64_t>(total));
            return true;
        };
    }

    auto res = cli.Post("/api/v1/upload", headers, size, provider, contentType, progressCb);
    if (!res || res->status != 200) return "";
    return extractJsonStringField(res->body, "file_id");
}

bool ClientCore::downloadMedia(const std::string& fileId, const std::string& destPath,
                               const MediaProgress& onProgress)
{
    if (m_tlsServerName.empty() || m_httpPort == 0 || m_accessToken.empty()) return false;

    httplib::SSLClient cli(m_tlsServerName, m_httpPort);
    if (!m_caFile.empty()) cli.set_ca_cert_path(m_caFile);
    if (!m_host.empty() && m_host != m_tlsServerName) {
        cli.set_hostname_addr_map({{m_tlsServerName, m_host}});
    }
    cli.set_connection_timeout(10);
    cli.set_read_timeout(120);
    cli.set_write_timeout(120);

    std::ofstream ofs(destPath, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;

    const httplib::Headers headers = {
        {"Authorization", "Bearer " + m_accessToken},
        {"X-Device-Id", m_deviceId},
    };

    httplib::DownloadProgress progressCb = nullptr;
    if (onProgress) {
        progressCb = [&onProgress](std::size_t current, std::size_t total) -> bool {
            onProgress(static_cast<std::int64_t>(current), static_cast<std::int64_t>(total));
            return true;
        };
    }

    // 流式下载：边收边写盘，不整体载入内存
    auto res = cli.Get(
        "/api/v1/download/" + fileId, headers,
        [&ofs](const char* data, std::size_t len) -> bool {
            ofs.write(data, static_cast<std::streamsize>(len));
            return static_cast<bool>(ofs);
        },
        progressCb);

    ofs.close();
    if (!res || (res->status != 200 && res->status != 206)) {
        std::error_code ec;
        std::filesystem::remove(destPath, ec);
        return false;
    }
    return true;
}
#endif // CLIENT_CORE_WITH_MEDIA

void ClientCore::sendFileMessage(int friId, const std::string& fileId, const std::string& fileName,
                                 std::int64_t size, const std::string& contentType,
                                 const std::string& sha256, bool isImage, int w, int h)
{
    im::proto::ChatInfoRq rq;
    rq.set_myid(m_myId);
    rq.set_friid(friId);
    rq.set_type(isImage ? im::proto::IMAGE : im::proto::FILE);
    rq.set_file_id(fileId);
    rq.set_file_name(fileName);
    rq.set_file_size(size);
    rq.set_content_type(contentType);
    rq.set_sha256(sha256);
    rq.set_image_width(w);
    rq.set_image_height(h);
    rq.set_msg_id(makeMsgId());
    sendPacket(DEF_PROT_CHAT_INFO_RQ, rq.SerializeAsString());
}

// ---------------- 心跳保活 ----------------

void ClientCore::setHeartbeatIntervalMs(int intervalMs)
{
    if (intervalMs > 0) m_hbIntervalMs = intervalMs;
}
#if defined(CLIENT_CORE_TEST_HOOKS)
void ClientCore::sendEncryptedHeartbeatProbeForTest(const std::string& marker)
{
    sendPacket(DEF_PROT_HEARTBEAT_RQ, marker);
}
#endif

void ClientCore::startHeartbeat()
{
    if (m_hbRunning.exchange(true)) return; // 已在运行
    m_hbThread = std::thread([this]() {
        while (m_hbRunning.load()) {
            // 分段睡眠，便于 stopHeartbeat 快速响应
            int waited = 0;
            while (waited < m_hbIntervalMs && m_hbRunning.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                waited += 50;
            }
            if (!m_hbRunning.load()) break;
            if (!m_transport->isOpen()) continue;

            // 超时判定：连续 3 个间隔无任何入站数据 → 判定断连
            const std::int64_t last = m_lastRecvMs.load();
            if (last > 0 && steadyNowMs() - last > 3LL * m_hbIntervalMs) {
                m_transport->close(); // 触发 onConnectionClosed
                continue;
            }
            sendPacket(DEF_PROT_HEARTBEAT_RQ, "");
        }
    });
}

void ClientCore::stopHeartbeat()
{
    if (!m_hbRunning.exchange(false)) return;
    if (m_hbThread.joinable()) m_hbThread.join();
}

void ClientCore::onHeartbeatRs(const char*, std::size_t)
{
    // 无需处理：入站包已在 transport 回调中刷新活跃时间
}

void ClientCore::onKickedOfflinePkt(const char*, std::size_t)
{
    // 被踢下线（同账号在别处登录）：通知认证 sink + UI。
    if (auto* sink = m_authSink.load()) sink->onKicked(0);
    if (auto* ev = m_events.load()) ev->onKickedOffline(0);
}

// ---------------- 会话状态 ----------------

int ClientCore::myId() const { return m_myId; }
std::string ClientCore::myNick() const { return m_nick; }
std::string ClientCore::myFeeling() const { return m_feeling; }
int ClientCore::myIconId() const { return m_iconId; }

// ---------------- 协议处理 ----------------
// 注意：m_events/m_storage 为原子指针，先 load 到局部变量再调用，
// 避免"判空"与"解引用"两次独立 load 之间的 TOCTOU。

void ClientCore::onRegisterRs(const char* data, std::size_t len)
{
    im::proto::RegisterRs rs;
    if (!parsePayload(data, len, rs)) return;
    if (auto* ev = m_events.load()) ev->onRegisterResult(rs.result());
}

void ClientCore::onLoginRs(const char* data, std::size_t len)
{
    im::proto::LoginRs rs;
    if (!parsePayload(data, len, rs)) return;
    if (rs.result() == LOGIN_SUCCESS) {
        m_myId = rs.userid();
        m_accessToken = rs.access_token(); // HTTP 文件服务鉴权用，跟 socket 侧同一枚 token
    }
    // P5-T06：优先回调认证 sink（AccountSession 编排）；否则走旧的 UI 直连事件。
    if (auto* sink = m_authSink.load()) {
        AuthTokenPayload p;
        p.result = rs.result();
        p.userId = rs.userid();
        p.accessToken = rs.access_token();
        p.refreshToken = rs.refresh_token();
        p.accessExpireAt = rs.access_token_expire_at();
        p.refreshExpireAt = rs.refresh_token_expire_at();
        p.sessionId = rs.session_id();
        sink->onLoginRs(p);
    }
    if (auto* ev = m_events.load()) ev->onLoginResult(rs.result(), rs.userid());
}

void ClientCore::onTokenLoginRs(const char* data, std::size_t len)
{
    im::proto::TokenLoginRs rs;
    if (!parsePayload(data, len, rs)) return;
    if (rs.result() == LOGIN_SUCCESS) m_myId = rs.userid();
    if (auto* sink = m_authSink.load()) {
        sink->onTokenLoginRs(rs.result(), rs.userid(), rs.access_token_expire_at());
    }
    // 兼容旧 UI：token 登录成功也当作登录成功上抛
    if (auto* ev = m_events.load()) ev->onLoginResult(rs.result(), rs.userid());
}

void ClientCore::onRefreshTokenRs(const char* data, std::size_t len)
{
    im::proto::RefreshTokenRs rs;
    if (!parsePayload(data, len, rs)) return;
    if (auto* sink = m_authSink.load()) {
        AuthTokenPayload p;
        p.result = rs.result();
        p.accessToken = rs.access_token();
        p.refreshToken = rs.refresh_token();
        p.accessExpireAt = rs.access_token_expire_at();
        p.refreshExpireAt = rs.refresh_token_expire_at();
        p.sessionId = rs.session_id();
        sink->onRefreshTokenRs(p);
    }
    // 刷新成功时同步更新 HTTP 鉴权用 access_token
    if (rs.result() == 0 && !rs.access_token().empty()) m_accessToken = rs.access_token();
}

void ClientCore::onLogoutRs(const char* data, std::size_t len)
{
    im::proto::LogoutRs rs;
    if (!parsePayload(data, len, rs)) return;
    if (auto* sink = m_authSink.load()) sink->onLogoutRs(rs.result());
}

void ClientCore::onFriendInfoPkt(const char* data, std::size_t len)
{
    im::proto::FriendInfo info;
    if (!parsePayload(data, len, info)) return;

    if (info.userid() == m_myId) {
        // 自己的资料：更新会话状态
        m_iconId = info.iconid();
        m_nick = info.nick();
        m_feeling = info.feeling();

        UserInfo self;
        self.id = m_myId;
        self.iconId = m_iconId;
        self.nick = m_nick;
        self.feeling = m_feeling;
        if (auto* st = m_storage.load()) st->saveSelfInfo(self);
        if (auto* ev = m_events.load()) ev->onSelfInfo(self);
    } else {
        im::FriendInfo fri; // 显式限定，避免与 pb 消息 im::proto::FriendInfo 冲突
        fri.id = info.userid();
        fri.iconId = info.iconid();
        fri.status = info.status();
        fri.nick = info.nick();
        fri.feeling = info.feeling();
        if (auto* st = m_storage.load()) st->saveFriend(fri);
        if (auto* ev = m_events.load()) ev->onFriendInfo(fri);
    }
}

void ClientCore::onChatInfoRq(const char* data, std::size_t len)
{
    im::proto::ChatInfoRq rq;
    if (!parsePayload(data, len, rq)) return;

    // 文件/图片卡片统一回调（rq.myid 是发送方）：字节不再随包下发，UI 按需 downloadMedia()
    if (rq.type() == im::proto::FILE || rq.type() == im::proto::IMAGE) {
        if (auto* ev = m_events.load()) {
            ev->onFileCard(rq.myid(), rq.file_id(), rq.file_name(), rq.file_size(), rq.msg_id(),
                           rq.content_type(), rq.sha256(), rq.type() == im::proto::IMAGE,
                           rq.image_width(), rq.image_height());
        }
        return;
    }

    // 本地持久化：收到的文本消息
    if (m_myId > 0) {
        if (auto* st = m_storage.load()) {
            st->saveChatMessage(m_myId, rq.myid(), false, rq.msg(),
                                static_cast<std::int64_t>(std::time(nullptr)));
        }
    }
    if (auto* ev = m_events.load()) ev->onChatMessage(rq.myid(), rq.msg());
}

void ClientCore::onChatInfoRs(const char* data, std::size_t len)
{
    im::proto::ChatInfoRs rs;
    if (!parsePayload(data, len, rs)) return;
    // 回复中 myid 是消息接收方（朋友），friid 是自己
    if (auto* ev = m_events.load()) ev->onChatSendResult(rs.myid(), rs.result());
}

void ClientCore::onAddFriRq(const char* data, std::size_t len)
{
    im::proto::AddFriendRq rq;
    if (!parsePayload(data, len, rq)) return;
    if (auto* ev = m_events.load()) ev->onAddFriendRequest(rq.myid(), rq.mynick());
}

void ClientCore::onAddFriRs(const char* data, std::size_t len)
{
    im::proto::AddFriendRs rs;
    if (!parsePayload(data, len, rs)) return;
    if (auto* ev = m_events.load()) ev->onAddFriendResult(rs.result(), rs.mynick());
}

void ClientCore::onFriendOfflinePkt(const char* data, std::size_t len)
{
    im::proto::FriendOffline pkt;
    if (!parsePayload(data, len, pkt)) return;
    if (auto* ev = m_events.load()) ev->onFriendOffline(pkt.offlineid());
}

// 把 pb ChatInfoRq 转成对外 RoamMessage 条目（漫游会话列表/历史分页共用）
static RoamMessage toRoamMessage(const im::proto::ChatInfoRq& c)
{
    RoamMessage rm;
    rm.fromId = c.myid();
    rm.toId = c.friid();
    rm.type = c.type() == im::proto::IMAGE ? 1 : (c.type() == im::proto::FILE ? 2 : 0);
    rm.text = c.msg();
    rm.fileId = c.file_id();
    rm.fileName = c.file_name();
    rm.fileSize = c.file_size();
    rm.contentType = c.content_type();
    rm.imgW = c.image_width();
    rm.imgH = c.image_height();
    rm.msgId = c.msg_id();
    rm.ts = c.ts();
    rm.seq = c.seq();
    return rm;
}

void ClientCore::onRoamConvRs(const char* data, std::size_t len)
{
    im::proto::RoamConvRs rs;
    if (!parsePayload(data, len, rs)) return;
    std::vector<RoamMessage> convs;
    convs.reserve(rs.convs_size());
    for (const auto& c : rs.convs()) convs.push_back(toRoamMessage(c));
    if (auto* ev = m_events.load()) ev->onRoamConversations(convs);
}

void ClientCore::onRoamMsgRs(const char* data, std::size_t len)
{
    im::proto::RoamMsgRs rs;
    if (!parsePayload(data, len, rs)) return;
    std::vector<RoamMessage> msgs;
    msgs.reserve(rs.msgs_size());
    for (const auto& c : rs.msgs()) msgs.push_back(toRoamMessage(c));
    if (auto* ev = m_events.load()) {
        ev->onRoamMessages(rs.peer_id(), msgs, rs.has_more(), rs.min_seq());
    }
}

} // namespace im
