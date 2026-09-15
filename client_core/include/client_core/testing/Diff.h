// 结构化 diff（P7-G1 Golden Harness）。
//
// 目的：Legacy 与 Native 对同一输入产生的领域对象，必须能**逐字段**比较，
// 失败时输出**首个字段差异**，而不是"两个大 blob 不相等"。
//
// 约定：参与 diff 的对象提供 `fields()`，返回 字段名 → 稳定字符串表示。
// 稳定表示意味着：不受 map 遍历顺序、浮点格式、平台差异影响。

#ifndef CLIENT_CORE_TESTING_DIFF_H
#define CLIENT_CORE_TESTING_DIFF_H

#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace im {
namespace testing {

struct FieldDiff {
    std::string path;     // 字段路径，如 "messages[3].content"
    std::string expected; // Legacy 侧值
    std::string actual;   // Native 侧值
};

/** 逐字段比较；按字段名排序保证输出稳定可复现。 */
inline std::vector<FieldDiff> diffFields(const std::map<std::string, std::string>& expected,
                                         const std::map<std::string, std::string>& actual)
{
    std::vector<FieldDiff> out;
    auto eit = expected.begin();
    auto ait = actual.begin();
    while (eit != expected.end() && ait != actual.end()) {
        if (eit->first < ait->first) {
            out.push_back({eit->first, eit->second, /*actual=*/"<missing>"});
            ++eit;
        } else if (ait->first < eit->first) {
            out.push_back({ait->first, /*expected=*/"<missing>", ait->second});
            ++ait;
        } else {
            if (eit->second != ait->second) {
                out.push_back({eit->first, eit->second, ait->second});
            }
            ++eit;
            ++ait;
        }
    }
    for (; eit != expected.end(); ++eit) {
        out.push_back({eit->first, eit->second, "<missing>"});
    }
    for (; ait != actual.end(); ++ait) {
        out.push_back({ait->first, "<missing>", ait->second});
    }
    return out;
}

/** 首个差异的可读描述；无差异返回空串。 */
inline std::string firstDifference(const std::vector<FieldDiff>& diffs)
{
    if (diffs.empty()) return {};
    const FieldDiff& d = diffs.front();
    std::ostringstream os;
    os << "首个差异 " << d.path << ": expected=[" << d.expected << "] actual=[" << d.actual << "]";
    if (diffs.size() > 1) os << "（共 " << diffs.size() << " 处差异）";
    return os.str();
}

/** 全部差异的可读描述（调试用）。 */
inline std::string allDifferences(const std::vector<FieldDiff>& diffs)
{
    std::ostringstream os;
    for (const auto& d : diffs) {
        os << "  - " << d.path << ": expected=[" << d.expected << "] actual=[" << d.actual << "]\n";
    }
    return os.str();
}

// ---- 稳定值格式化辅助（避免浮点/布尔的平台差异） ----
template <typename T>
std::string stableValue(T v)
{
    std::ostringstream os;
    os << v;
    return os.str();
}

inline std::string stableValue(bool v) { return v ? "true" : "false"; }

} // namespace testing
} // namespace im

#endif // CLIENT_CORE_TESTING_DIFF_H
