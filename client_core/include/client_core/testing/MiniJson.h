// 最小 JSON 解析器（P7-G1 Harness）。
//
// 目的：Golden 用例以 JSON 为唯一真相源（三端共享），C++ 端需要独立消费它。
// 这里不引入第三方 JSON 依赖，只实现 Golden 用例所需子集：
//   null / bool / number / string（含转义）/ array / object。
//
// 设计取舍：
//   - 只做解析与只读访问，不做序列化；
//   - 解析失败返回明确错误，绝不"尽力猜测"；
//   - 数字统一按 double 存储，另提供 asInt() 做整数取整（Golden 用例的
//     时间戳/计数都是整数，足够用；需要精确大整数时另议）。

#ifndef CLIENT_CORE_TESTING_MINI_JSON_H
#define CLIENT_CORE_TESTING_MINI_JSON_H

#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace im {
namespace testing {

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() : m_type(Type::Null) {}

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isBool() const { return m_type == Type::Bool; }
    bool isNumber() const { return m_type == Type::Number; }
    bool isString() const { return m_type == Type::String; }
    bool isArray() const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    bool asBool(bool def = false) const { return isBool() ? m_bool : def; }
    double asNumber(double def = 0.0) const { return isNumber() ? m_number : def; }
    std::int64_t asInt(std::int64_t def = 0) const
    {
        return isNumber() ? static_cast<std::int64_t>(std::llround(m_number)) : def;
    }
    const std::string& asString() const
    {
        static const std::string kEmpty;
        return isString() ? m_string : kEmpty;
    }

    // ---- object ----
    const JsonValue* find(const std::string& key) const
    {
        if (!isObject()) return nullptr;
        auto it = m_object.find(key);
        return it == m_object.end() ? nullptr : &it->second;
    }
    bool has(const std::string& key) const { return find(key) != nullptr; }

    /** 取字符串字段；不存在返回 def。 */
    std::string str(const std::string& key, const std::string& def = {}) const
    {
        const JsonValue* v = find(key);
        return (v && v->isString()) ? v->m_string : def;
    }
    std::int64_t intOr(const std::string& key, std::int64_t def = 0) const
    {
        const JsonValue* v = find(key);
        return (v && v->isNumber()) ? v->asInt() : def;
    }

    // ---- array / object 遍历 ----
    std::size_t size() const
    {
        if (isArray()) return m_array.size();
        if (isObject()) return m_object.size();
        return 0;
    }
    const JsonValue& at(std::size_t i) const
    {
        static const JsonValue kNull;
        return (isArray() && i < m_array.size()) ? m_array[i] : kNull;
    }
    const std::map<std::string, JsonValue>& object() const { return m_object; }
    const std::vector<JsonValue>& array() const { return m_array; }

    // ---- 解析 ----
    static bool parse(const std::string& text, JsonValue& out, std::string* err = nullptr);

private:
    struct Parser {
        const std::string& s;
        std::size_t i = 0;
        std::string err;
        int depth = 0;
        // 递归下降解析必须限制嵌套深度，否则深度嵌套的 JSON 会耗尽调用栈（栈溢出）。
        static constexpr int kMaxDepth = 64;

        explicit Parser(const std::string& text) : s(text) {}

        void skipWs()
        {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
        }
        bool parseValue(JsonValue& out);
        bool parseString(std::string& out);
        bool parseNumber(double& out);
        bool literal(const char* lit);
    };

    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0;
    std::string m_string;
    std::vector<JsonValue> m_array;
    std::map<std::string, JsonValue> m_object;
};

inline bool JsonValue::Parser::literal(const char* lit)
{
    const std::size_t n = std::string(lit).size();
    if (s.compare(i, n, lit) != 0) {
        err = "期望字面量 " + std::string(lit);
        return false;
    }
    i += n;
    return true;
}

inline bool JsonValue::Parser::parseString(std::string& out)
{
    if (i >= s.size() || s[i] != '"') {
        err = "期望 '\"'";
        return false;
    }
    ++i;
    out.clear();
    while (i < s.size()) {
        const char c = s[i];
        if (c == '"') {
            ++i;
            return true;
        }
        if (c == '\\') {
            ++i;
            if (i >= s.size()) {
                err = "转义字符后意外结束";
                return false;
            }
            const char e = s[i++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    // \uXXXX：Golden 用例主要用 ASCII，这里按 UTF-8 编码 BMP 字符
                    if (i + 4 > s.size()) {
                        err = "\\u 转义不完整";
                        return false;
                    }
                    unsigned code = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = s[i + k];
                        unsigned d = 0;
                        if (h >= '0' && h <= '9') d = static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') d = static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') d = static_cast<unsigned>(h - 'A' + 10);
                        else {
                            err = "\\u 转义含非法十六进制";
                            return false;
                        }
                        code = code * 16 + d;
                    }
                    i += 4;
                    if (code < 0x80) {
                        out.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default:
                    err = "未知转义字符";
                    return false;
            }
            continue;
        }
        out.push_back(c);
        ++i;
    }
    err = "字符串未闭合";
    return false;
}

inline bool JsonValue::Parser::parseNumber(double& out)
{
    // 严格 JSON 数字文法：-? (0 | [1-9][0-9]*) (. [0-9]+)? ([eE] [+-]? [0-9]+)?
    // 不接受前导 '+'，也不接受前导零后接数字（01 / 007 均非法）。
    const std::size_t start = i;
    if (i < s.size() && s[i] == '-') ++i;

    if (i >= s.size() || s[i] < '0' || s[i] > '9') {
        err = "期望数字";
        return false;
    }
    if (s[i] == '0') {
        ++i;
        if (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            err = "数字含非法前导零";
            return false;
        }
    } else {
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    }

    if (i < s.size() && s[i] == '.') {
        ++i;
        if (i >= s.size() || s[i] < '0' || s[i] > '9') {
            err = "小数点后缺数字";
            return false;
        }
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    }

    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        if (i >= s.size() || s[i] < '0' || s[i] > '9') {
            err = "指数部分缺数字";
            return false;
        }
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    }

    try {
        out = std::stod(s.substr(start, i - start));
    } catch (...) {
        err = "数字解析失败";
        return false;
    }
    return true;
}

inline bool JsonValue::Parser::parseValue(JsonValue& out)
{
    skipWs();
    if (i >= s.size()) {
        err = "意外结束";
        return false;
    }

    // 深度守卫：递归前自增、返回时自减（RAII），超限直接失败而非继续递归
    if (depth >= kMaxDepth) {
        err = "嵌套层级超过上限（" + std::to_string(kMaxDepth) + "）";
        return false;
    }
    struct DepthGuard {
        int& d;
        explicit DepthGuard(int& depth) : d(depth) { ++d; }
        ~DepthGuard() { --d; }
    } guard(depth);

    const char c = s[i];
    if (c == '{') {
        ++i;
        out.m_type = Type::Object;
        skipWs();
        if (i < s.size() && s[i] == '}') {
            ++i;
            return true;
        }
        while (true) {
            skipWs();
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (i >= s.size() || s[i] != ':') {
                err = "期望 ':'";
                return false;
            }
            ++i;
            JsonValue v;
            if (!parseValue(v)) return false;
            out.m_object[key] = v;
            skipWs();
            if (i < s.size() && s[i] == ',') {
                ++i;
                continue;
            }
            if (i < s.size() && s[i] == '}') {
                ++i;
                return true;
            }
            err = "期望 ',' 或 '}'";
            return false;
        }
    }
    if (c == '[') {
        ++i;
        out.m_type = Type::Array;
        skipWs();
        if (i < s.size() && s[i] == ']') {
            ++i;
            return true;
        }
        while (true) {
            JsonValue v;
            if (!parseValue(v)) return false;
            out.m_array.push_back(v);
            skipWs();
            if (i < s.size() && s[i] == ',') {
                ++i;
                continue;
            }
            if (i < s.size() && s[i] == ']') {
                ++i;
                return true;
            }
            err = "期望 ',' 或 ']'";
            return false;
        }
    }
    if (c == '"') {
        out.m_type = Type::String;
        return parseString(out.m_string);
    }
    if (c == 't') {
        out.m_type = Type::Bool;
        out.m_bool = true;
        return literal("true");
    }
    if (c == 'f') {
        out.m_type = Type::Bool;
        out.m_bool = false;
        return literal("false");
    }
    if (c == 'n') {
        out.m_type = Type::Null;
        return literal("null");
    }
    out.m_type = Type::Number;
    return parseNumber(out.m_number);
}

inline bool JsonValue::parse(const std::string& text, JsonValue& out, std::string* err)
{
    Parser p(text);
    if (!p.parseValue(out)) {
        if (err) *err = p.err + "（偏移 " + std::to_string(p.i) + "）";
        return false;
    }
    p.skipWs();
    if (p.i != text.size()) {
        if (err) *err = "解析结束后仍有剩余内容（偏移 " + std::to_string(p.i) + "）";
        return false;
    }
    return true;
}

} // namespace testing
} // namespace im

#endif // CLIENT_CORE_TESTING_MINI_JSON_H
