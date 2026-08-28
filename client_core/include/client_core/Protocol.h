#pragma once
// IM 协议常量与编码工具（纯 C++17，无 Qt / 无平台依赖）
// 阶段 P2：包体已迁移 protobuf（定义见 protocol/im.proto），本文件只保留：
//   - 协议号常量（protType，线格式 [4B 大端包长][4B 小端协议号][pb payload]）
//   - 业务结果码 / 字段软上限
//   - UTF-8 安全截断、协议号小端编解码工具
// 注意：协议号与结果码必须与服务端 def.h 保持一致。

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>

namespace im {
namespace proto {

using protType = std::uint32_t; // 线上为小端 4 字节
static_assert(sizeof(protType) == 4, "protType must be 4 bytes");

// TCP 端口（与服务端 def.h 一致）
constexpr std::uint16_t TCP_PORT = 24563;

// 协议类型基值与数量（函数指针数组边界，越界校验用）
constexpr protType DEF_BASE = 1000;
constexpr int DEF_PROT_COUNT = 36;

// 单个包体最大长度（字节），防止异常/恶意超大包导致 OOM/DoS
// 含 4 字节协议号 + pb payload
constexpr std::size_t MAX_PACK_LEN = 10 * 1024 * 1024; // 10MB

// ---------------- 协议号 ----------------
constexpr protType DEF_PROT_REGISTER_RQ         = DEF_BASE + 0; // 注册请求
constexpr protType DEF_PROT_REGISTER_RS         = DEF_BASE + 1; // 注册回复
constexpr protType DEF_PROT_LOGIN_RQ            = DEF_BASE + 2; // 登录请求
constexpr protType DEF_PROT_LOGIN_RS            = DEF_BASE + 3; // 登录回复
constexpr protType DEF_PROT_FRIEND_INFO         = DEF_BASE + 4; // 用户/好友信息
constexpr protType DEF_PROT_CHAT_INFO_RQ        = DEF_BASE + 5; // 聊天请求
constexpr protType DEF_PROT_CHAT_INFO_RS        = DEF_BASE + 6; // 聊天回复
constexpr protType DEF_PROT_ADD_FRIEND_RQ       = DEF_BASE + 7; // 添加好友请求
constexpr protType DEF_PROT_ADD_FRIEND_RS       = DEF_BASE + 8; // 添加好友回复
constexpr protType DEF_PROT_FRIEND_OFFLINE      = DEF_BASE + 9; // 下线通知
constexpr protType DEF_PROT_HEARTBEAT_RQ        = DEF_BASE + 10; // 心跳请求（C→S，空 payload）
constexpr protType DEF_PROT_HEARTBEAT_RS        = DEF_BASE + 11; // 心跳回复（S→C，空 payload）
constexpr protType DEF_PROT_KICKED_OFFLINE      = DEF_BASE + 12; // 被踢下线通知（S→C，空 payload）
constexpr protType DEF_PROT_ROAM_CONV_RQ        = DEF_BASE + 13; // 会话列表漫游请求（C→S，登录后每会话末条）
constexpr protType DEF_PROT_ROAM_CONV_RS        = DEF_BASE + 14; // 会话列表漫游响应（S→C）
constexpr protType DEF_PROT_ROAM_MSG_RQ         = DEF_BASE + 15; // 会话历史分页请求（C→S）
constexpr protType DEF_PROT_ROAM_MSG_RS         = DEF_BASE + 16; // 会话历史分页响应（S→C）
// DEF_BASE+17..22 曾用于文件分片协议（FileOfferRq/FileChunkRq/FileCompleteRq/
// FileProgressRs/FileDownloadRq），已整体迁移到 HTTP 文件服务（HttpFileServer），
// 协议号作废不再回收复用，避免旧客户端/文档里的编号被挪作他用造成混淆。
constexpr protType DEF_PROT_TOKEN_LOGIN_RQ      = DEF_BASE + 23; // token登录请求
constexpr protType DEF_PROT_TOKEN_LOGIN_RS      = DEF_BASE + 24; // token登录响应
constexpr protType DEF_PROT_TOKEN_REFRESH_RQ    = DEF_BASE + 25; // token刷新请求
constexpr protType DEF_PROT_TOKEN_REFRESH_RS    = DEF_BASE + 26; // token刷新响应
constexpr protType DEF_PROT_LOGOUT_RQ           = DEF_BASE + 27; // 注销请求
constexpr protType DEF_PROT_LOGOUT_RS           = DEF_BASE + 28; // 注销响应
constexpr protType DEF_PROT_AI_REPLY_RQ         = DEF_BASE + 29; // AI候选回复请求
constexpr protType DEF_PROT_AI_REPLY_RS         = DEF_BASE + 30; // AI候选回复响应
constexpr protType DEF_PROT_AI_CANCEL_RQ        = DEF_BASE + 31; // 取消AI请求（尽力而为）
constexpr protType DEF_PROT_DELETE_FRIEND_RQ    = DEF_BASE + 32; // 删除好友请求
constexpr protType DEF_PROT_DELETE_FRIEND_RS    = DEF_BASE + 33; // 删除好友响应
constexpr protType DEF_PROT_FRIEND_REQUEST_LIST_RQ = DEF_BASE + 34;
constexpr protType DEF_PROT_FRIEND_REQUEST_LIST_RS = DEF_BASE + 35;

// ---------------- 字段软上限（字节，UTF-8） ----------------
// 与原定长 struct 语义一致；pb 字符串不再定长，由应用层截断保护
constexpr std::size_t USER_NICK_LEN    = 30;
constexpr std::size_t USER_TEL_LEN     = 15;
constexpr std::size_t USER_PASS_LEN    = 20;
constexpr std::size_t USER_FEELING_LEN = 100;
constexpr std::size_t CHAT_MSG_LEN     = 1024 * 8; // 8KB

// ---------------- 结果码 ----------------
constexpr int REGISTER_SUCC      = 1;
constexpr int REGISTER_NICK_EXIT = 2;
constexpr int REGISTER_TEL_EXIT  = 3;
constexpr int REGISTER_INVALID   = 4;

constexpr int LOGIN_SUCCESS   = 0;
constexpr int LOGIN_NOTEXIT   = 1;
constexpr int LOGIN_PASSERROR = 2;
constexpr int LOGIN_INVALID   = 3;

constexpr int STATUS_ONLINE  = 0;
constexpr int STATUS_OFFLINE = 1;

constexpr int CHAT_RESULT_SUCC = 0;
constexpr int CHAT_RESULT_FAIL = 1;
constexpr int CHAT_RESULT_NOT_FRIEND = 2;
constexpr int CHAT_RESULT_SERVER_ERROR = 3;
constexpr int CHAT_RESULT_FILE_NOT_OWNED = 4; // type=FILE/IMAGE 的 file_id 不属于当前发送者（见 SECURITY_REVIEW.md #8/#42）

constexpr int ADD_FRIEND_AGREE   = 0;
constexpr int ADD_FRIEND_REJECT  = 1;
constexpr int ADD_FRIEND_OFFLINE = 2;
constexpr int ADD_FRIEND_NOTEXIT = 3;
constexpr int ADD_FRIEND_SELF = 4;
constexpr int ADD_FRIEND_ALREADY = 5;
constexpr int ADD_FRIEND_PENDING = 6;
constexpr int ADD_FRIEND_DB_ERROR = 7;

constexpr int DELETE_FRIEND_SUCCESS = 0;
constexpr int DELETE_FRIEND_DB_ERROR = 1;
constexpr int DELETE_FRIEND_NOT_FRIEND = 2;
constexpr int DELETE_FRIEND_INVALID = 3;

// 文件传输：分片协议已废弃（迁移到 HTTP 文件服务），仅保留通用媒体大小上限，
// 供 HttpFileServer 的上传端点和 client_core 的上传封装共用同一个软上限。
constexpr std::int64_t FILE_MAX_SIZE  = 100LL * 1024 * 1024; // 100MB

// ---------------- 编码工具 ----------------

// 协议号小端编解码（线上固定小端，与主机序无关）
inline void encodeType32(protType v, char* out)
{
    out[0] = static_cast<char>(v & 0xFF);
    out[1] = static_cast<char>((v >> 8) & 0xFF);
    out[2] = static_cast<char>((v >> 16) & 0xFF);
    out[3] = static_cast<char>((v >> 24) & 0xFF);
}

inline protType decodeType32(const char* p)
{
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return static_cast<protType>(u[0]) |
           (static_cast<protType>(u[1]) << 8) |
           (static_cast<protType>(u[2]) << 16) |
           (static_cast<protType>(u[3]) << 24);
}

// UTF-8 安全截断：不超过 maxBytes 且不切断多字节字符
inline std::string utf8Truncate(const std::string& s, std::size_t maxBytes)
{
    if (s.size() <= maxBytes) return s;
    std::size_t n = maxBytes;
    // 回退到字符边界（continuation byte 形如 10xxxxxx）
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}

} // namespace proto
} // namespace im
