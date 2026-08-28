#pragma once
// client_core 对外数据类型（纯 C++，与 UI 无关）

#include <cstdint>
#include <string>

namespace im {

// 用户自己信息
struct UserInfo {
    int id = 0;
    int iconId = 0;
    std::string nick;    // UTF-8
    std::string feeling; // UTF-8
};

// 好友信息
struct FriendInfo {
    int id = 0;
    int iconId = 0;
    int status = 1; // 0=在线 1=离线（与 proto::STATUS_* 一致）
    std::string nick;    // UTF-8
    std::string feeling; // UTF-8
};

// 聊天记录（本地存储用）
struct ChatMessage {
    std::int64_t id = 0;   // 本地自增 id
    int peerId = 0;        // 对端用户 id
    bool outgoing = false; // true=我发出的，false=收到的
    std::string content;   // UTF-8
    std::int64_t ts = 0;   // unix 秒
};

// 漫游消息条目（会话列表末条 / 历史分页共用）。M7 起图片/文件统一走 HTTP 文件服务，
// 这里只带元数据，UI 按需调用 ClientCore::downloadMedia(fileId, ...) 下载。
struct RoamMessage {
    int fromId = 0;         // 发送方 id
    int toId = 0;           // 接收方 id
    int type = 0;           // 0=文本 1=图片 2=文件（与 proto::MsgType 一致）
    std::string text;       // 文本正文（UTF-8），type=0 时使用
    std::string fileId;     // type=1/2 时：HTTP 文件服务下载标识
    std::string fileName;   // type=2 时：文件名
    std::int64_t fileSize = 0; // type=1/2 时：字节数
    std::string contentType;   // MIME 类型（服务端上传时记录，漫游历史可能为空，见已知简化）
    int imgW = 0;
    int imgH = 0;
    std::string msgId;
    std::int64_t ts = 0;    // unix 秒
    std::int64_t seq = 0;   // 会话级序列号
};

// 文件协商结果（含断点续传水位线）：旧分片协议专用，随分片协议一起废弃，
// 保留结构体定义仅为兼容尚未清理的旧引用，新代码不应再使用
struct FileOfferInfo {
    std::string msgId;
    std::string fileId;
    int receivedChunks = 0;
    int result = 0;
};

} // namespace im
