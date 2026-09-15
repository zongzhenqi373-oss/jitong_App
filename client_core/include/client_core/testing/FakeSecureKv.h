// 安全 KV 的 Fake（P7-G1 Harness）。
//
// 目的：模拟 ADR-03 要求的「journal 状态与 payload 存 Native DB，同时在安全 KV
// 保存带 MAC 的最小镜像」。可注入镜像缺失/不一致/篡改，验证 fail-close 与恢复矩阵。

#ifndef CLIENT_CORE_TESTING_FAKE_SECURE_KV_H
#define CLIENT_CORE_TESTING_FAKE_SECURE_KV_H

#include <map>
#include <string>

namespace im {
namespace testing {

class FakeSecureKv {
public:
    // ---- 基本 KV ----
    bool put(const std::string& key, const std::string& value)
    {
        m_kv[key] = value;
        return true;
    }

    bool get(const std::string& key, std::string* out) const
    {
        auto it = m_kv.find(key);
        if (it == m_kv.end()) return false;
        if (out) *out = it->second;
        return true;
    }

    bool remove(const std::string& key) { return m_kv.erase(key) > 0; }
    bool contains(const std::string& key) const { return m_kv.count(key) > 0; }

    // ---- MAC 镜像语义（journal 双写） ----
    /** 写入带 MAC 的镜像：value 与 mac 分开存，便于单独损坏任一项。 */
    bool putMirrored(const std::string& key, const std::string& value, const std::string& mac)
    {
        m_kv[key] = value;
        m_kv[key + kMacSuffix] = mac;
        return true;
    }

    /**
     * 读取并校验镜像。
     * @return Ok / Missing（键或 MAC 缺失）/ MacMismatch（MAC 与计算值不符）
     */
    enum class MirrorResult { Ok, Missing, MacMismatch };

    MirrorResult getMirrored(const std::string& key, const std::string& expectedMac,
                             std::string* out) const
    {
        auto v = m_kv.find(key);
        auto m = m_kv.find(key + kMacSuffix);
        if (v == m_kv.end() || m == m_kv.end()) return MirrorResult::Missing;
        if (m->second != expectedMac) return MirrorResult::MacMismatch;
        if (out) *out = v->second;
        return MirrorResult::Ok;
    }

    // ---- 故障注入 ----
    /** 模拟镜像整体丢失（如 KV 被清）。 */
    void dropMirror(const std::string& key)
    {
        m_kv.erase(key);
        m_kv.erase(key + kMacSuffix);
    }

    /** 只损坏 MAC，保留值（模拟篡改/半写）。 */
    void corruptMac(const std::string& key) { m_kv[key + kMacSuffix] = "deadbeef"; }

    void clear() { m_kv.clear(); }
    std::size_t size() const { return m_kv.size(); }

    static constexpr const char* kMacSuffix = ".mac";

private:
    std::map<std::string, std::string> m_kv;
};

} // namespace testing
} // namespace im

#endif // CLIENT_CORE_TESTING_FAKE_SECURE_KV_H
