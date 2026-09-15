// 通用 Golden runner（P7-G1 Harness）。
//
// 契约：JSON 是**唯一真相源**，Legacy(Kotlin) 与 Native(C++) 读取同一份用例，
// 各自执行后由本 runner 做逐字段 diff，失败时输出**首个字段差异**。
//
// 用例格式（约定）：
// ```json
// {
//   "id": "send-text-001",
//   "input":    { ... 由各领域自定义 ... },
//   "expected": { "messages[0].content": "hello", "messages[0].status": "1" }
// }
// ```
// `expected` 是展平后的 字段路径 → 期望值；执行器返回同样展平的实际字段映射。
//
// 这样 G2～G8 只需注册 Executor，无需各自再写一套比较逻辑。

#ifndef CLIENT_CORE_TESTING_GOLDEN_RUNNER_H
#define CLIENT_CORE_TESTING_GOLDEN_RUNNER_H

#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "client_core/testing/Diff.h"
#include "client_core/testing/MiniJson.h"

namespace im {
namespace testing {

class GoldenRunner {
public:
    struct CaseResult {
        std::string caseId;
        bool passed = false;
        std::size_t diffCount = 0;
        std::string firstDiff;            // 首个字段差异（人读）
        std::vector<FieldDiff> diffs;     // 全部差异（调试用）
        std::string error;                // 执行器/解析错误
    };

    /** 执行器：输入用例的 input，返回展平后的实际字段映射。 */
    using Executor = std::function<std::map<std::string, std::string>(const JsonValue& input)>;

    /** 读取整个文件；失败返回 false。 */
    static bool readFile(const std::string& path, std::string& out)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::ostringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    }

    /**
     * 加载用例集合。根为数组，或对象的 "cases" 字段为数组。
     */
    static bool loadCases(const std::string& path, std::vector<JsonValue>& out, std::string* err)
    {
        std::string text;
        if (!readFile(path, text)) {
            if (err) *err = "无法读取用例文件: " + path;
            return false;
        }
        JsonValue root;
        if (!JsonValue::parse(text, root, err)) return false;

        if (root.isArray()) {
            for (std::size_t i = 0; i < root.size(); ++i) out.push_back(root.at(i));
            return true;
        }
        if (root.isObject()) {
            const JsonValue* cases = root.find("cases");
            if (cases && cases->isArray()) {
                for (std::size_t i = 0; i < cases->size(); ++i) out.push_back(cases->at(i));
                return true;
            }
        }
        if (err) *err = "用例文件根必须是数组，或对象含 cases 数组";
        return false;
    }

    /**
     * 执行单个用例。
     * @param c    用例 JSON（含 id / input / expected）
     * @param exec 领域执行器
     */
    static CaseResult runCase(const JsonValue& c, const Executor& exec)
    {
        CaseResult r;
        r.caseId = c.str("id", "<unnamed>");

        const JsonValue* expected = c.find("expected");
        if (!expected || !expected->isObject()) {
            r.error = "用例缺少 expected 对象";
            return r;
        }

        std::map<std::string, std::string> expectedFields = flattenFields(*expected);

        std::map<std::string, std::string> actualFields;
        try {
            const JsonValue* input = c.find("input");
            actualFields = exec(input ? *input : JsonValue());
        } catch (const std::exception& e) {
            r.error = std::string("执行器异常: ") + e.what();
            return r;
        }

        r.diffs = diffFields(expectedFields, actualFields);
        r.diffCount = r.diffs.size();
        r.passed = r.diffs.empty();
        if (!r.passed) r.firstDiff = firstDifference(r.diffs);
        return r;
    }

    /**
     * 把 JSON 对象展平为 字段路径 → 稳定字符串。
     * 嵌套对象用 "." 连接，数组用 "[i]" 连接，与 DTO 的 fields(prefix) 约定一致。
     */
    static std::map<std::string, std::string> flattenFields(const JsonValue& obj,
                                                            const std::string& prefix = "")
    {
        std::map<std::string, std::string> out;
        if (obj.isObject()) {
            for (const auto& kv : obj.object()) {
                const std::string key = prefix.empty() ? kv.first : prefix + "." + kv.first;
                const auto sub = flattenFields(kv.second, key);
                out.insert(sub.begin(), sub.end());
            }
        } else if (obj.isArray()) {
            for (std::size_t i = 0; i < obj.size(); ++i) {
                const std::string key = prefix + "[" + std::to_string(i) + "]";
                const auto sub = flattenFields(obj.at(i), key);
                out.insert(sub.begin(), sub.end());
            }
        } else {
            out[prefix] = scalarToString(obj);
        }
        return out;
    }

    /** 标量的稳定字符串表示（与 Diff.h 的 stableValue 语义保持一致）。 */
    static std::string scalarToString(const JsonValue& v)
    {
        if (v.isNull()) return "<null>";
        if (v.isBool()) return v.asBool() ? "true" : "false";
        if (v.isNumber()) {
            // 整数不带小数点，避免 "1" vs "1.0" 的伪差异
            const double d = v.asNumber();
            const std::int64_t i = v.asInt();
            if (std::abs(d - static_cast<double>(i)) < 1e-9) return std::to_string(i);
            std::ostringstream os;
            os << d;
            return os.str();
        }
        return v.asString();
    }

    /** 汇总输出；返回失败用例数。 */
    static std::size_t report(const std::vector<CaseResult>& results)
    {
        std::size_t failed = 0;
        for (const auto& r : results) {
            if (r.passed) continue;
            ++failed;
            if (!r.error.empty()) {
                std::cout << "  [FAIL] " << r.caseId << ": " << r.error << std::endl;
            } else {
                std::cout << "  [FAIL] " << r.caseId << ": " << r.firstDiff << std::endl;
                if (r.diffCount > 1) {
                    std::cout << allDifferences(r.diffs);
                }
            }
        }
        return failed;
    }
};

} // namespace testing
} // namespace im

#endif // CLIENT_CORE_TESTING_GOLDEN_RUNNER_H
