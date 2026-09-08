#pragma once
// 平台原子能力接口（P2-T03）。
//
// 定位：**只提供操作系统能力，不含任何业务语义**。
//
// 判定标准（V3 计划 §3.3）：
//   - 接口不得出现 Token、消息、会话、秒传、缩略图等业务概念；
//   - 接口不得做策略决定（例如「是否生成缩略图」「是否重试」都由 C++ 内核决定）；
//   - 平台侧只回答「能不能做」和「做的结果」，不回答「该不该做」。
//
// 例如 IPlatformImage.encodeAvif() 只负责编码；编码质量、目标尺寸、
// 是否生成缩略图由 C++ ThumbnailPipeline 决定。
//
// 首阶段只定义接口与生命周期，具体 JNI 实现随 P5（Keystore）/P10（Image/File）
// 逐步接入，避免一次性引入大量未验证代码。

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jt {

using Bytes = std::vector<std::uint8_t>;

/** 平台文件句柄：Android 侧为 fd 或已打开的本地路径，内核不解释其含义。 */
struct FileHandle {
    int fd = -1;            // >=0 时有效
    std::string localPath;  // fd 不可用时的退化路径

    bool valid() const { return fd >= 0 || !localPath.empty(); }
};

/** 图片基本信息。 */
struct ImageInfo {
    int width = 0;
    int height = 0;
    /** EXIF 方向（1..8），1 表示无需旋转。 */
    int orientation = 1;
    /** 色彩空间："srgb" | "display-p3" | "unknown" */
    std::string colorSpace = "unknown";
    std::string mime;
};

/** 解码选项：目标像素格式与色彩空间由内核决定，平台只执行。 */
struct DecodeOptions {
    int targetWidth = 0;   // 0 表示不缩放
    int targetHeight = 0;
    /** 强制转换到 sRGB（内核统一使用 SDR sRGB，避免跨设备色偏）。 */
    bool convertToSrgb = true;
};

/** 像素缓冲区：RGBA8，行主序，长度 = width * height * 4。 */
struct PixelBuffer {
    int width = 0;
    int height = 0;
    Bytes rgba8;

    bool valid() const {
        return width > 0 && height > 0 && rgba8.size() == static_cast<std::size_t>(width) * height * 4;
    }
};

struct EncodeOptions {
    /** AVIF 质量 0..100；由内核的 MediaConfig 下发，平台不自行决定。 */
    int quality = 80;
};

// ---------------------------------------------------------------------------
// 1. Keystore：只做「签名」与「取公钥」
// ---------------------------------------------------------------------------
class IPlatformKeystore {
public:
    virtual ~IPlatformKeystore() = default;

    /** 密钥别名，平台自行定义取值。 */
    using KeyAlias = std::string;

    /** 返回 SPKI 编码的公钥；不存在时返回空。 */
    virtual Bytes publicKey(const KeyAlias& alias) = 0;

    /**
     * 异步签名。
     * 必须异步：Android Keystore 的签名可能触发用户认证/生物识别，
     * 阻塞调用方线程（尤其是网络线程）会造成 ANR。
     */
    virtual void signAsync(const KeyAlias& alias, Bytes payload,
                           std::function<void(bool ok, Bytes signature)> cb) = 0;
};

// ---------------------------------------------------------------------------
// 2. 文件：只做 open / rename / 原子替换
// ---------------------------------------------------------------------------
class IPlatformFile {
public:
    virtual ~IPlatformFile() = default;

    /** 打开 content:// 或文件路径用于读取。 */
    virtual FileHandle openRead(const std::string& uri) = 0;

    /** 打开本地路径用于写入（截断）。 */
    virtual FileHandle openWrite(const std::string& path) = 0;

    /** 原子改名，用于「下载到 .part 后替换为正式文件」。 */
    virtual bool atomicRename(const std::string& from, const std::string& to) = 0;

    /** 读取 [offset, offset+length) 区间；秒传 PoP 需要按 challenge 读指定片段。 */
    virtual bool readRange(const FileHandle& file, std::int64_t offset, std::int64_t length,
                           Bytes& out) = 0;

    virtual void close(FileHandle& file) = 0;
};

// ---------------------------------------------------------------------------
// 3. 图片：只做 probe / decode / encode
// ---------------------------------------------------------------------------
class IPlatformImage {
public:
    virtual ~IPlatformImage() = default;

    /** 只解析元信息，不解码像素。 */
    virtual ImageInfo probe(const FileHandle& file) = 0;

    /** 按内核给定的参数解码。 */
    virtual PixelBuffer decode(const FileHandle& file, const DecodeOptions& options) = 0;

    /** 按内核给定的质量编码为 AVIF。 */
    virtual bool encodeAvif(const PixelBuffer& pixels, const EncodeOptions& options,
                            const FileHandle& output) = 0;
};

// ---------------------------------------------------------------------------
// 4. 安全 KV：opaque 字节存取，不解释内容
// ---------------------------------------------------------------------------
class ISecureKv {
public:
    virtual ~ISecureKv() = default;

    /** 平台返回的是「密文 blob」，内核负责序列化与加密，平台不理解字段含义。 */
    virtual bool load(const std::string& key, Bytes& out) = 0;
    virtual bool save(const std::string& key, const Bytes& blob) = 0;
    virtual void erase(const std::string& key) = 0;
};

// ---------------------------------------------------------------------------
// 5. 网络状态：只上报「通断/类型变化」，不做重连决策
// ---------------------------------------------------------------------------
class INetworkObserver {
public:
    virtual ~INetworkObserver() = default;

    enum class State { Unknown, Available, Lost };

    virtual State current() = 0;
    virtual void setCallback(std::function<void(State)> cb) = 0;
    virtual void close() = 0;
};

/** 内核使用的平台能力集合；未接入的能力为 nullptr，调用方必须判空。 */
struct PlatformServices {
    IPlatformKeystore* keystore = nullptr;
    IPlatformFile* file = nullptr;
    IPlatformImage* image = nullptr;
    ISecureKv* secureKv = nullptr;
    INetworkObserver* network = nullptr;
};

} // namespace jt
