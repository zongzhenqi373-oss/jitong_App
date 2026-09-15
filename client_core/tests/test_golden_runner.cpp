// P7-G1：Golden runner 自检（JSON 唯一真相源 + 逐字段 diff + 首个差异定位）。
//
// 验收点（v2 §3 P7-G1）：
//   - Legacy/Native 读取**同一份 JSON 用例**，执行后可结构化 diff；
//   - 失败输出**首个字段差异**；
//   - JSON 解析失败必须明确报错，不"尽力猜测"。

#include "client_core/testing/GoldenRunner.h"
#include "client_core/testing/MiniJson.h"

#include <iostream>
#include <map>
#include <string>

using namespace im::testing;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& name)
{
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << std::endl;
    if (!cond) ++g_failures;
}
} // namespace

int main()
{
    std::cout << "=== test_golden_runner ===" << std::endl;

    // [1] MiniJson：各类型解析
    {
        JsonValue v;
        std::string err;
        const bool ok = JsonValue::parse(
            R"({"s":"hi","n":42,"f":1.5,"t":true,"f2":false,"nil":null,"arr":[1,2],"obj":{"k":"v"}})",
            v, &err);
        check(ok, "复合 JSON 解析成功 " + err);
        check(v.str("s") == "hi", "字符串字段");
        check(v.intOr("n") == 42, "整数字段");
        check(v.find("f")->asNumber() == 1.5, "浮点字段");
        check(v.find("t")->asBool() == true, "true 字段");
        check(v.find("f2")->asBool() == false, "false 字段");
        check(v.find("nil")->isNull(), "null 字段");
        check(v.find("arr")->size() == 2, "数组长度");
        check(v.find("arr")->at(1).asInt() == 2, "数组元素");
        check(v.find("obj")->str("k") == "v", "嵌套对象");
    }

    // [2] 转义与 Unicode
    {
        JsonValue v;
        std::string err;
        check(JsonValue::parse(R"({"a":"line\nbreak","b":"tab\t","c":"q\"uote","d":"中"})", v, &err),
              "含转义与中文的 JSON 解析成功 " + err);
        check(v.str("a") == "line\nbreak", "\\n 转义正确");
        check(v.str("b") == "tab\t", "\\t 转义正确");
        check(v.str("c") == "q\"uote", "\\\" 转义正确");
        check(v.str("d") == "\xe4\xb8\xad", "中文 UTF-8 正确");
    }

    // [3] 错误检测：必须明确报错，不猜测
    {
        const char* bad[] = {
            R"({"a":1)",          // 截断
            R"({"a":1}{"b":2})",  // 尾随内容
            R"({"a":01})",        // 非法数字（前导 0 后接数字）
            R"({'a':1})",         // 单引号（非法）
            R"({"a":"x\q"})",     // 未知转义
            R"({"a" 1})",         // 缺冒号
        };
        for (const char* b : bad) {
            JsonValue v;
            std::string err;
            const bool ok = JsonValue::parse(b, v, &err);
            check(!ok && !err.empty(), std::string("畸形 JSON 被拒绝且有错误信息: ") + b);
        }
    }

    // [4] flattenFields：嵌套与数组展平
    {
        JsonValue v;
        std::string err;
        check(JsonValue::parse(R"({"a":{"b":"1"},"list":[{"c":"2"},{"c":"3"}]})", v, &err),
              "展平用例解析 " + err);
        const auto f = GoldenRunner::flattenFields(v);
        check(f.count("a.b") == 1 && f.at("a.b") == "1", "嵌套对象展平为 a.b");
        check(f.count("list[0].c") == 1 && f.at("list[0].c") == "2", "数组展平为 list[0].c");
        check(f.count("list[1].c") == 1 && f.at("list[1].c") == "3", "数组第二项展平");
    }

    // [5] scalarToString：整数不带小数点（避免 1 vs 1.0 的伪差异）
    {
        JsonValue v;
        std::string err;
        check(JsonValue::parse(R"({"i":1,"d":1.0,"d2":2.5,"b":true,"n":null})", v, &err),
              "标量用例解析 " + err);
        check(GoldenRunner::scalarToString(*v.find("i")) == "1", "整数 1 → \"1\"");
        check(GoldenRunner::scalarToString(*v.find("d")) == "1", "1.0 → \"1\"（无伪差异）");
        check(GoldenRunner::scalarToString(*v.find("d2")) == "2.5", "2.5 保留小数");
        check(GoldenRunner::scalarToString(*v.find("b")) == "true", "布尔 → \"true\"");
        check(GoldenRunner::scalarToString(*v.find("n")) == "<null>", "null → \"<null>\"");
    }

    // [6] runCase：通过 / 失败并定位首个差异
    {
        JsonValue c;
        std::string err;
        check(JsonValue::parse(
                  R"({"id":"case-1","input":{"text":"hi"},"expected":{"content":"hi","status":"1"}})",
                  c, &err),
              "用例解析 " + err);

        // 一致 → 通过
        auto r1 = GoldenRunner::runCase(c, [](const JsonValue&) {
            return std::map<std::string, std::string>{{"content", "hi"}, {"status", "1"}};
        });
        check(r1.passed, "一致 → 通过");
        check(r1.firstDiff.empty(), "通过时无差异描述");

        // 不一致 → 失败且定位到具体字段
        auto r2 = GoldenRunner::runCase(c, [](const JsonValue&) {
            return std::map<std::string, std::string>{{"content", "ho"}, {"status", "1"}};
        });
        check(!r2.passed, "不一致 → 失败");
        check(r2.diffCount == 1, "差异数为 1");
        std::cout << "      " << r2.firstDiff << std::endl;
        check(r2.firstDiff.find("content") != std::string::npos, "首个差异定位到 content");

        // 缺失字段
        auto r3 = GoldenRunner::runCase(c, [](const JsonValue&) {
            return std::map<std::string, std::string>{{"content", "hi"}};
        });
        check(!r3.passed && r3.firstDiff.find("<missing>") != std::string::npos,
              "缺失字段报 <missing>");
    }

    // [7] 执行器异常被捕获，不导致测试崩溃
    {
        JsonValue c;
        std::string err;
        JsonValue::parse(R"({"id":"boom","expected":{"a":"1"}})", c, &err);
        auto r = GoldenRunner::runCase(c, [](const JsonValue&) -> std::map<std::string, std::string> {
            throw std::runtime_error("executor failed");
        });
        check(!r.passed, "执行器抛异常 → 用例失败");
        check(r.error.find("executor failed") != std::string::npos, "异常信息被保留");
    }

    // [8] 消费真实三端共享 golden 文件（唯一真相源）
    {
        JsonValue root;
        std::string err;
        std::string text;
        check(GoldenRunner::readFile(CLIENT_CORE_GOLDEN_JSON, text), "读取 app-security-v1.json");
        check(JsonValue::parse(text, root, &err), "解析三端共享 golden " + err);
        check(root.intOr("security_version") == 1, "security_version = 1（ADR-02 不变式）");
        check(root.str("cipher_suite") ==
                  "APP_CIPHER_X25519_ED25519_HKDF_SHA256_AES_256_GCM",
              "cipher_suite 正确");
        const JsonValue* vec = root.find("vector");
        check(vec != nullptr && vec->isObject(), "含 vector 对象");
        if (vec) {
            check(vec->str("client_nonce") ==
                      "2222222222222222222222222222222222222222222222222222222222222222",
                  "vector.client_nonce 可读取");
            check(vec->intOr("key_id") == 1, "vector.key_id = 1");
        }
    }

    // [9] 深度嵌套必须被拒绝（递归解析的栈溢出防护）
    {
        // 正常深度（10 层）仍可解析
        std::string ok10;
        for (int i = 0; i < 10; ++i) ok10 += "{\"a\":";
        ok10 += "1";
        for (int i = 0; i < 10; ++i) ok10 += "}";
        JsonValue v1;
        std::string e1;
        check(JsonValue::parse(ok10, v1, &e1), "正常深度(10 层)解析成功 " + e1);

        // 超深（200 层）被拒绝，而不是爆栈
        std::string deep;
        for (int i = 0; i < 200; ++i) deep += "{\"a\":";
        deep += "1";
        for (int i = 0; i < 200; ++i) deep += "}";
        JsonValue v2;
        std::string e2;
        const bool ok = JsonValue::parse(deep, v2, &e2);
        check(!ok, "深度嵌套(200 层)被拒绝，不递归爆栈");
        check(e2.find("嵌套层级超过上限") != std::string::npos, "错误信息指明深度超限");

        // 超深数组同样被拒
        std::string deepArr;
        for (int i = 0; i < 200; ++i) deepArr += "[";
        deepArr += "1";
        for (int i = 0; i < 200; ++i) deepArr += "]";
        JsonValue v3;
        std::string e3;
        check(!JsonValue::parse(deepArr, v3, &e3), "深度嵌套数组(200 层)被拒绝");
    }

    if (g_failures == 0) {
        std::cout << "test_golden_runner PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_golden_runner FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
