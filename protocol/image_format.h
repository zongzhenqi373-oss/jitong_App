#pragma once
// 图片格式嗅探（header-only），客户端 client_core 与服务端 im_server 共用。
// 通过文件头魔数（magic bytes）判断真实格式，返回带点的扩展名；
// 识别不了的二进制数据兜底返回 ".bin"，避免张冠李戴。

#include <string>

namespace im {

inline std::string imageExtForBytes(const std::string& bytes)
{
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::size_t n = bytes.size();

    // PNG: 89 50 4E 47 0D 0A 1A 0A
    if (n >= 8 && p[0] == 0x89 && p[1] == 0x50 && p[2] == 0x4E && p[3] == 0x47 &&
        p[4] == 0x0D && p[5] == 0x0A && p[6] == 0x1A && p[7] == 0x0A)
        return ".png";
    // JPEG: FF D8 FF
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF)
        return ".jpg";
    // GIF: 47 49 46 38 ("GIF8")
    if (n >= 4 && p[0] == 0x47 && p[1] == 0x49 && p[2] == 0x46 && p[3] == 0x38)
        return ".gif";
    // BMP: 42 4D ("BM")
    if (n >= 2 && p[0] == 0x42 && p[1] == 0x4D)
        return ".bmp";
    // WebP: "RIFF" .... "WEBP"
    if (n >= 12 && p[0] == 0x52 && p[1] == 0x49 && p[2] == 0x46 && p[3] == 0x46 &&
        p[8] == 0x57 && p[9] == 0x45 && p[10] == 0x42 && p[11] == 0x50)
        return ".webp";

    return ".bin";
}

} // namespace im
