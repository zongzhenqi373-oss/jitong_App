#pragma once
// 文件/媒体路径与格式的纯校验函数，Dispatcher（旧 socket 分片路径，逐步淘汰中）与
// HttpFileServer（新 HTTP 文件服务）共用，避免同样的两个小函数抄两份。

#include <cctype>
#include <string>

namespace imsrv {

// file_id（== msg_id）只允许安全字符集，防止用作路径片段时发生穿越
inline bool isSafeFileId(const std::string& id)
{
    if (id.empty()) return false;
    for (char c : id) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-')) return false;
    }
    if (id.find("..") != std::string::npos) return false;
    return true;
}

inline bool isSha256Hex(const std::string& value)
{
    if (value.size() != 64) return false;
    for (unsigned char c : value) {
        if (!std::isxdigit(c)) return false;
    }
    return true;
}

} // namespace imsrv
