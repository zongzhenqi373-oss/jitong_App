#include "native_sdk_handle.h"

#include <exception>
#include <utility>

#include <android/log.h>

namespace jt {
namespace {

// 句柄表：id → 句柄。放在匿名 namespace 内，只允许通过下面几个函数访问，
// 避免 JNI 层任何地方拿到裸指针后跨调用保存。
std::mutex g_tableMutex;
std::unordered_map<jlong, std::shared_ptr<NativeSdkHandle>> g_handles;
jlong g_nextId = 1; // 0 保留为「无效句柄」

/**
 * 真正执行销毁：把资源移出句柄后在锁外析构。
 * 只允许被 releaseHandle 调用一次（由 destroying 的 CAS 保证）。
 */
bool destroyHandleLocked(const std::shared_ptr<NativeSdkHandle>& handle)
{
    if (!handle) return false;

    bool expected = false;
    if (!handle->destroying.compare_exchange_strong(expected, true,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
        return false; // 已由其他线程销毁
    }

    std::shared_ptr<im::ClientCore> core;
    std::shared_ptr<JniObserver> observer;

    {
        std::lock_guard<std::mutex> lk(handle->mutex);
        core = std::move(handle->core);
        observer = std::move(handle->observer);
    }

    // 在锁外停止并析构。ClientCore 内部保存的是非拥有的 IClientEvents*：
    // 必须先禁止它取得新的 observer，再停网络/心跳并等待相关线程退出，最后才能
    // DeleteGlobalRef。反过来先析构 observer，Transport 关闭回调会访问悬空指针。
    if (core) {
        core->setEventSink(nullptr);
        core->disconnect();
    }
    core.reset();
    observer.reset();

    return true;
}

} // namespace

/** base64（标准字母表，忽略空白与 '=' 填充）解码。 */
bool base64Decode(const std::string& in, std::vector<unsigned char>& out)
{
    static const std::string kChars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    out.clear();
    int val = 0;
    int valBits = 0;
    for (const char c : in) {
        if (c == '=' || c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        const auto pos = kChars.find(c);
        if (pos == std::string::npos) return false;
        val = (val << 6) | static_cast<int>(pos);
        valBits += 6;
        if (valBits >= 8) {
            valBits -= 8;
            out.push_back(static_cast<unsigned char>((val >> valBits) & 0xFF));
        }
    }
    return true;
}

jlong createHandle(const std::string& serverName,
                   const std::map<std::uint32_t, std::string>& identityKeys)
{
    auto handle = std::make_shared<NativeSdkHandle>();

    try {
        im::ClientConfig cfg;
        cfg.tlsServerName = serverName;

        // 信任根必须在建连**之前**注入：少了它，首次连接必然 BadKeyId 失败。
        // 任一 key 非法就整体失败，不做"跳过这一条继续"的降级。
        for (const auto& kv : identityKeys) {
            std::vector<unsigned char> pub;
            if (!base64Decode(kv.second, pub) || pub.size() != 32) {
                __android_log_print(ANDROID_LOG_ERROR, "JitongKernel",
                                    "identityKeys[%u] 非法：需要 32 字节的 base64 公钥", kv.first);
                return 0;
            }
            cfg.identityKeys[kv.first] = pub;
        }
        if (cfg.identityKeys.empty()) {
            __android_log_print(ANDROID_LOG_ERROR, "JitongKernel",
                                "未提供任何 identityKeys：应用层握手将必然失败");
            return 0;
        }

        // 构造只初始化状态，不建连接；连接由 connectToServer 触发（P3 之后）
        handle->core = std::make_shared<im::ClientCore>(cfg);
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, "JitongKernel", "createHandle failed: %s", e.what());
        return 0;
    } catch (...) {
        __android_log_print(ANDROID_LOG_ERROR, "JitongKernel", "createHandle failed: unknown");
        return 0;
    }

    std::lock_guard<std::mutex> lk(g_tableMutex);
    const jlong id = g_nextId++;
    g_handles.emplace(id, std::move(handle));
    return id;
}

std::shared_ptr<NativeSdkHandle> lookupHandle(jlong id)
{
    if (id == 0) return nullptr;
    std::lock_guard<std::mutex> lk(g_tableMutex);
    const auto it = g_handles.find(id);
    return it == g_handles.end() ? nullptr : it->second;
}

bool releaseHandle(jlong id)
{
    std::shared_ptr<NativeSdkHandle> handle;
    {
        std::lock_guard<std::mutex> lk(g_tableMutex);
        const auto it = g_handles.find(id);
        if (it == g_handles.end()) return false; // 幂等：已释放或 id 无效
        handle = it->second;
        g_handles.erase(it);
    }
    // 表锁已释放，再执行可能较慢的销毁动作
    return destroyHandleLocked(handle);
}

std::size_t liveHandleCount()
{
    std::lock_guard<std::mutex> lk(g_tableMutex);
    return g_handles.size();
}

} // namespace jt
