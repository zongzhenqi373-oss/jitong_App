// P7-G1：稳定 DTO 与结构化 diff 自检。
//
// 验收点（v2 §3 P7-G1）：
//   - Legacy/Native 同一输入可以结构化 diff；
//   - 失败输出**首个字段差异**，而不是"两个对象不相等"；
//   - 字段完整覆盖（P6 教训：漏 12 个缩略图字段导致对账失效）。

#include "client_core/dto/Dtos.h"
#include "client_core/testing/Diff.h"

#include <iostream>
#include <string>
#include <vector>

using namespace im::dto;
using namespace im::testing;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& name)
{
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << std::endl;
    if (!cond) ++g_failures;
}

MessageDto makeMessage(const std::string& msgId)
{
    MessageDto m;
    m.msgId = msgId;
    m.conversationId = 555;
    m.peerId = 2;
    m.seq = 10;
    m.ts = 1700000000;
    m.localOrder = 1;
    m.fromMe = false;
    m.type = 0;
    m.content = "hello";
    m.status = 1;
    m.thumbnailFileId = "thumb-1";
    m.thumbnailSize = 4096;
    m.largeThumbnailFileId = "large-1";
    m.largeThumbnailSize = 20480;
    return m;
}
} // namespace

int main()
{
    std::cout << "=== test_golden_diff ===" << std::endl;

    // [1] 字段完整性（含 12 个缩略图字段，P6 曾漏）
    {
        const MessageDto m = makeMessage("m1");
        const auto f = m.fields();
        const char* required[] = {
            "thumbnailFileId",       "thumbnailPath",       "thumbnailSize",
            "thumbnailSha256",       "thumbnailW",          "thumbnailH",
            "largeThumbnailFileId",  "largeThumbnailPath",  "largeThumbnailSize",
            "largeThumbnailSha256",  "largeThumbnailW",     "largeThumbnailH",
            "version", "msgId", "seq", "ts", "localOrder", "fromMe", "status",
            "mediaPath", "imgW", "imgH", "fileId", "sha256", "localPath", "transferred",
        };
        bool all = true;
        for (const char* k : required) {
            if (f.count(k) == 0) {
                all = false;
                std::cout << "      缺失字段: " << k << std::endl;
            }
        }
        check(all, "MessageDto 字段完整（含 12 个缩略图字段）");
        check(f.at("version") == "1", "version 字段为 1");
    }

    // [2] 相同对象 → 无差异
    {
        const auto a = makeMessage("m1").fields();
        const auto b = makeMessage("m1").fields();
        const auto d = diffFields(a, b);
        check(d.empty(), "相同 DTO 无差异");
        check(firstDifference(d).empty(), "无差异时 firstDifference 为空");
    }

    // [3] 单字段不同 → 精确定位该字段
    {
        auto expected = makeMessage("m1");
        auto actual = makeMessage("m1");
        actual.content = "world";
        const auto d = diffFields(expected.fields(), actual.fields());
        check(d.size() == 1, "仅 1 处差异");
        check(!d.empty() && d.front().path == "content", "差异定位到 content");
        const std::string msg = firstDifference(d);
        std::cout << "      " << msg << std::endl;
        check(msg.find("content") != std::string::npos, "首个差异描述含字段名");
        check(msg.find("hello") != std::string::npos && msg.find("world") != std::string::npos,
              "首个差异描述含两侧值");
    }

    // [4] 缩略图字段差异可被发现（P6 回归门禁）
    {
        auto expected = makeMessage("m1");
        auto actual = makeMessage("m1");
        actual.thumbnailFileId = "";
        actual.largeThumbnailSize = 0;
        const auto d = diffFields(expected.fields(), actual.fields());
        check(d.size() == 2, "缩略图两处差异被发现");
        std::cout << "      " << firstDifference(d) << std::endl;
    }

    // [5] 缺失字段 → <missing>
    {
        const auto a = makeMessage("m1").fields();
        auto b = a;
        b.erase("sha256");
        const auto d = diffFields(a, b);
        check(d.size() == 1 && d.front().path == "sha256", "缺失字段被报出");
        check(d.front().actual == "<missing>", "缺失侧标记为 <missing>");
    }

    // [6] 列表 diff（带索引前缀，能定位到第几条）
    {
        std::map<std::string, std::string> expected;
        std::map<std::string, std::string> actual;
        for (int i = 0; i < 3; ++i) {
            const std::string p = "messages[" + std::to_string(i) + "].";
            auto e = makeMessage("m" + std::to_string(i));
            auto a = makeMessage("m" + std::to_string(i));
            if (i == 2) a.status = 9; // 只有第 3 条不同
            const auto ef = e.fields(p);
            const auto af = a.fields(p);
            expected.insert(ef.begin(), ef.end());
            actual.insert(af.begin(), af.end());
        }
        const auto d = diffFields(expected, actual);
        check(d.size() == 1, "列表 diff 只有 1 处差异");
        check(d.front().path == "messages[2].status", "差异定位到 messages[2].status");
        std::cout << "      " << firstDifference(d) << std::endl;
    }

    // [7] 多差异时提示总数
    {
        auto expected = makeMessage("m1");
        auto actual = makeMessage("m1");
        actual.content = "x";
        actual.seq = 99;
        actual.status = 2;
        const auto d = diffFields(expected.fields(), actual.fields());
        check(d.size() == 3, "3 处差异");
        check(firstDifference(d).find("共 3 处差异") != std::string::npos, "提示差异总数");
    }

    // [8] 各 DTO 均带 version 且可 diff
    {
        ConversationDto c;
        c.conversationId = 1;
        c.unread = 5;
        check(c.fields().count("version") == 1, "ConversationDto 带 version");
        check(c.fields().at("unread") == "5", "ConversationDto unread 稳定表示");

        FriendDto fr;
        fr.friendId = 7;
        fr.online = true;
        check(fr.fields().count("version") == 1, "FriendDto 带 version");
        check(diffFields(fr.fields(), fr.fields()).empty(), "FriendDto 自比较无差异");

        FriendRequestDto rq;
        rq.requestId = "r1";
        rq.state = RequestState::Pending;
        check(rq.fields().count("version") == 1, "FriendRequestDto 带 version");

        MediaTaskDto t;
        t.taskId = "t1";
        t.state = TaskState::Running;
        check(t.fields().count("version") == 1, "MediaTaskDto 带 version");

        AiSuggestionDto ai;
        ai.requestId = "a1";
        ai.status = AiStatus::Success;
        ai.contextVersion = "ctx-1";
        check(ai.fields().count("version") == 1, "AiSuggestionDto 带 version");
        check(ai.fields().at("contextVersion") == "ctx-1", "AI 上下文版本参与 diff");
    }

    // [9] SearchHit 高亮单位明确（禁止跨端歧义）
    {
        SearchHit h;
        h.msgId = "m1";
        h.highlightUnit = SearchHit::HighlightUnit::Utf16;
        h.highlightRanges = {{0, 2}, {5, 7}};
        const auto f = h.fields();
        check(f.at("highlightUnit") == "0", "highlightUnit 显式编码（0=utf16）");
        check(f.at("highlightRanges") == "0:2,5:7", "高亮区间稳定序列化");
    }

    // [10] AI 取消后旧响应不展示：contextVersion 变化即视为差异
    {
        AiSuggestionDto fresh;
        fresh.requestId = "a1";
        fresh.contextVersion = "ctx-1";
        fresh.status = AiStatus::Pending;

        AiSuggestionDto stale = fresh;
        stale.contextVersion = "ctx-2"; // 换会话/上下文已变
        stale.status = AiStatus::Success;

        const auto d = diffFields(fresh.fields(), stale.fields());
        check(!d.empty(), "上下文版本变化被 diff 检出（旧响应不得展示）");
    }

    if (g_failures == 0) {
        std::cout << "test_golden_diff PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_golden_diff FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
