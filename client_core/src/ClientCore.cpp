#include "client_core/ClientCore.h"
#include "client_core/IStorage.h"
#include "TcpTransport.h"
#include "im.pb.h"
#include "sha256.h"
#include "httplib.h"

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
    //开发阶段如果证书SAN是 IP:127.0.0.1，serverName可以暂时传 "127.0.0.1"。
    //不能使用 verify_none 或永远返回true的验证回调。
    m_transport = std::make_unique<TcpTransport>(
        std::move(config.tlsServerName),
        std::move(config.caFile)
    );

    initFunArr();

    m_transport->setPacketHandler([this](const char* data, std::size_t len) {
        // 任何入站包都刷新活跃时间（含心跳回复）
        m_lastRecvMs.store(steadyNowMs());
        dispatchPacket(data, len);
    });
    m_transport->setCloseHandler([this]() {
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

// ---------------- 连接管理 ----------------

bool ClientCore::connectToServer(const std::string& ip, std::uint16_t port)
{
    if (!m_transport->connect(ip, port)) return false;
    m_host = ip;
    if (m_httpPort == 0) m_httpPort = static_cast<std::uint16_t>(port + 1);
    m_lastRecvMs.store(steadyNowMs());
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
}

void ClientCore::dispatchPacket(const char* data, std::size_t len)
{
    // 包体 = [4B 小端协议号][pb payload]
    if (!data || len < sizeof(protType)) return;

    const protType type = decodeType32(data);

    // 范围校验（防越界访问函数指针数组）
    if (type < DEF_BASE) return;
    const std::size_t index = type - DEF_BASE;
    if (index >= static_cast<std::size_t>(DEF_PROT_COUNT)) return;

    DealFun pFun = m_dealFunArr[index];
    if (pFun) (this->*pFun)(data + sizeof(protType), len - sizeof(protType));
}

void ClientCore::sendPacket(protType type, const std::string& payload)
{
    std::string body;
    body.resize(sizeof(protType) + payload.size());
    encodeType32(type, body.data());
    std::memcpy(body.data() + sizeof(protType), payload.data(), payload.size());
    m_transport->send(body.data(), body.size());
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
    rq.set_tel(utf8Truncate(tel, USER_TEL_LEN - 1));
    // 对齐 QQNT：传输的是密码哈希，而非明文
    rq.set_pass(sha256Hex(pass));
    rq.set_device_id(m_deviceId);
    rq.set_device_name("C++ ClientCore");
    rq.set_client_version("client-core-0.5.0");
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
    // 被踢下线（同账号在别处登录）：通知 UI，由 UI 决定提示与收尾
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
    if (auto* ev = m_events.load()) ev->onLoginResult(rs.result(), rs.userid());
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
