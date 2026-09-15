// P7-G4：AiSuggestionService（AI 候选回复状态机）测试。
//
// 覆盖 v2 §3 P7-G4 AI 部分：
//   - 请求冻结上下文（conversationId/tone/contextVersion）+ requestId 生成；
//   - 同会话 single-flight（复用 requestId，不并发）；
//   - 成功结果落库 + loadSuggestions 恢复；
//   - 取消（Aborted）+ 取消后迟到响应抑制；
//   - 20s 超时（Error/超时码）+ 超时后迟到响应抑制；
//   - 不存在 requestId 的迟到响应抑制；
//   - invalidate（换号）后旧 in-flight 响应抑制。

#include <iostream>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "client_core/ai/AiSuggestionService.h"
#include "client_core/dto/Dtos.h"
#include "client_core/storage/NativeDatabase.h"
#include "client_core/storage/NativeRepository.h"
#include "client_core/testing/DeterministicClock.h"

using namespace im::storage;
using im::ai::AiRequestContext;
using im::ai::AiSuggestionService;
using im::dto::AiStatus;
using im::testing::DeterministicClock;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& name)
{
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << std::endl;
    if (!cond) ++g_failures;
}

const char* kDir = "/tmp/test_ai_suggestion";
const std::int64_t kOwner = 1;

std::vector<unsigned char> testKey() { return std::vector<unsigned char>(32, 0x66); }

long long queryInt(NativeDatabase& db, const std::string& sql)
{
    long long v = -1;
    db.withRead([&](sqlite3* d) {
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(d, sql.c_str(), -1, &s, nullptr) == SQLITE_OK && s) {
            if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
        }
        if (s) sqlite3_finalize(s);
    });
    return v;
}

std::string quote(const std::string& s) { return "'" + s + "'"; }
} // namespace

int main()
{
    std::cout << "=== test_ai_suggestion ===" << std::endl;

    ::system(("rm -rf " + std::string(kDir)).c_str());
    ::system(("mkdir -p " + std::string(kDir)).c_str());

    DeterministicClock clock;
    NativeDatabase db;
    std::string err;
    check(db.open(kDir, kOwner, testKey(), &err), "打开库 " + err);
    NativeRepository repo(&db);
    AiSuggestionService ai(kOwner, &repo, &clock);

    // [1] 请求冻结上下文 + single-flight
    {
        AiRequestContext ctx;
        ctx.conversationId = 100;
        ctx.peerId = 2;
        ctx.tone = "formal";
        ctx.contextVersion = "v1";
        auto r = ai.requestAiReply(ctx);
        check(r.ok, "请求成功");
        check(!r.singleFlighted, "首次请求非 single-flight");
        check(!r.requestId.empty(), "生成 requestId");

        auto r2 = ai.requestAiReply(ctx);
        check(r2.ok && r2.singleFlighted, "同会话重复请求 single-flight");
        check(r2.requestId == r.requestId, "复用同一 requestId（不并发）");

        // 不同会话可并发
        AiRequestContext ctx2;
        ctx2.conversationId = 200;
        ctx2.contextVersion = "v1";
        auto r3 = ai.requestAiReply(ctx2);
        check(r3.ok && !r3.singleFlighted, "不同会话各自 in-flight");
        check(r3.requestId != r.requestId, "不同会话 requestId 不同");
    }

    // [2] 成功结果落库 + 恢复
    {
        AiRequestContext ctx;
        ctx.conversationId = 101;
        ctx.tone = "casual";
        ctx.contextVersion = "v1";
        auto r = ai.requestAiReply(ctx);
        ai.onAiResult(r.requestId, AiStatus::Success, {"好呀", "可以"}, 0);
        check(queryInt(db, "SELECT count(*) FROM ai_suggestions WHERE request_id=" +
                                quote(r.requestId)) == 1,
              "成功结果落库 1 条");
        check(queryInt(db, "SELECT status FROM ai_suggestions WHERE request_id=" +
                                quote(r.requestId)) == 1,
              "status=Success(1)");

        std::vector<im::dto::AiSuggestionDto> out;
        check(ai.loadSuggestions(101, &out, &err), "loadSuggestions " + err);
        check(out.size() == 1, "会话 101 有 1 条建议");
        if (!out.empty()) {
            check(out[0].status == AiStatus::Success, "status=Success");
            check(out[0].suggestions.size() == 2, "2 个候选");
            check(out[0].tone == "casual", "tone 冻结值保留");
            check(out[0].contextVersion == "v1", "contextVersion 冻结值保留");
        }
    }

    // [3] 取消 + 迟到响应抑制
    {
        AiRequestContext ctx;
        ctx.conversationId = 102;
        ctx.contextVersion = "v1";
        auto r = ai.requestAiReply(ctx);
        check(ai.cancelAiReply(r.requestId), "取消成功");
        check(queryInt(db, "SELECT status FROM ai_suggestions WHERE request_id=" +
                                quote(r.requestId)) == 3,
              "取消后 status=Aborted(3)");
        // 取消后迟到响应：抑制，不覆盖 Aborted
        ai.onAiResult(r.requestId, AiStatus::Success, {"迟到"}, 0);
        check(queryInt(db, "SELECT status FROM ai_suggestions WHERE request_id=" +
                                quote(r.requestId)) == 3,
              "取消后迟到响应不覆盖（仍 Aborted）");
        check(!ai.cancelAiReply(r.requestId), "二次取消返回 false");
    }

    // [4] 超时
    {
        AiRequestContext ctx;
        ctx.conversationId = 103;
        ctx.contextVersion = "v1";
        auto r = ai.requestAiReply(ctx);
        clock.advance(AiSuggestionService::kTimeoutMs + 1);
        ai.tick();
        check(queryInt(db, "SELECT status FROM ai_suggestions WHERE request_id=" +
                                quote(r.requestId)) == 2,
              "超时后 status=Error(2)");
        check(queryInt(db, "SELECT error_code FROM ai_suggestions WHERE request_id=" +
                                quote(r.requestId)) == AiSuggestionService::kErrorTimeout,
              "超时 error_code=超时码");
        // 超时后迟到响应：抑制
        ai.onAiResult(r.requestId, AiStatus::Success, {"迟到"}, 0);
        check(queryInt(db, "SELECT status FROM ai_suggestions WHERE request_id=" +
                                quote(r.requestId)) == 2,
              "超时后迟到响应不覆盖（仍 Error）");
    }

    // [5] 不存在 requestId 的迟到响应抑制
    {
        ai.onAiResult("ai-nonexistent", AiStatus::Success, {"x"}, 0);
        check(queryInt(db, "SELECT count(*) FROM ai_suggestions WHERE request_id='ai-nonexistent'") == 0,
              "不存在 requestId 不落库");
    }

    // [6] invalidate（换号）后旧 in-flight 响应抑制
    {
        AiRequestContext ctx;
        ctx.conversationId = 104;
        ctx.contextVersion = "v1";
        auto r = ai.requestAiReply(ctx);
        ai.invalidate();
        ai.onAiResult(r.requestId, AiStatus::Success, {"旧响应"}, 0);
        check(queryInt(db, "SELECT count(*) FROM ai_suggestions WHERE request_id=" +
                                quote(r.requestId)) == 0,
              "invalidate 后迟到响应抑制（不落库）");
    }

    db.close();
    ::system(("rm -rf " + std::string(kDir)).c_str());

    if (g_failures == 0) {
        std::cout << "test_ai_suggestion PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_ai_suggestion FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
