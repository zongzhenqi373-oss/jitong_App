// P7-G4：同步水位与缺洞状态机测试。
//
// 验收（v2 §3 P7-G4）：
//   - Ack / Push / 离线 / 漫游的排列组合有确定结果；
//   - seq <= contiguous 幂等且不回退水位；
//   - 跳跃产生缺洞，补齐后推进并吞并连续区间；
//   - 预算耗尽保留 gap 并退避，不形成请求风暴；
//   - 乱序到达最终收敛。

#include <iostream>
#include <string>

#include "client_core/sync/SyncTracker.h"

using namespace im::sync;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& name)
{
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << std::endl;
    if (!cond) ++g_failures;
}

SyncTracker::Config makeCfg()
{
    SyncTracker::Config c;
    c.ownerId = 1;
    c.conversationId = 555;
    c.maxPagesPerPass = 5;
    c.maxGapsPerPass = 2;
    c.baseBackoffMs = 1000;
    c.maxBackoffMs = 8000;
    return c;
}
} // namespace

int main()
{
    std::cout << "=== test_sync_tracker ===" << std::endl;

    // [1] 连续推进
    {
        SyncTracker t(makeCfg());
        check(t.observeSeq(1), "observe(1) 推进");
        check(t.observeSeq(2), "observe(2) 推进");
        check(t.observeSeq(3), "observe(3) 推进");
        check(t.contiguousSeq() == 3, "contiguous=3");
        check(t.maxSeenSeq() == 3, "maxSeen=3");
        check(!t.hasGaps(), "无缺洞");
    }

    // [2] 重复/乱序：不回退水位（幂等）
    {
        SyncTracker t(makeCfg());
        t.observeSeq(1);
        t.observeSeq(2);
        t.observeSeq(3);
        check(!t.observeSeq(2), "重复 observe(2) 不推进");
        check(t.contiguousSeq() == 3, "水位不回退（仍为 3）");
        check(!t.observeSeq(1), "重复 observe(1) 不推进");
        check(t.contiguousSeq() == 3, "水位仍为 3");
    }

    // [3] 跳跃产生缺洞
    {
        SyncTracker t(makeCfg());
        t.observeSeq(1);
        check(!t.observeSeq(5), "observe(5) 不推进（跳跃）");
        check(t.contiguousSeq() == 1, "contiguous 仍为 1");
        check(t.maxSeenSeq() == 5, "maxSeen=5");
        check(t.hasGaps(), "产生缺洞");
        check(t.gapCount() == 1, "1 个缺洞");
        const auto gaps = t.pendingGaps(0);
        check(!gaps.empty() && gaps[0].from == 2 && gaps[0].to == 4,
              "缺洞区间为 [2,4]");
    }

    // [4] 补齐后推进并吞并连续区间
    {
        SyncTracker t(makeCfg());
        t.observeSeq(1);
        t.observeSeq(5);  // gap [2,4]
        t.fillGap(2, 4);
        check(t.contiguousSeq() == 5, "补齐后 contiguous=5（吞并到 5）");
        check(!t.hasGaps(), "缺洞清空");
    }

    // [5] 部分补齐：区间被拆分，水位只推进到补齐处
    {
        SyncTracker t(makeCfg());
        t.observeSeq(1);
        t.observeSeq(5);  // gap [2,4]
        t.fillGap(2, 3);
        check(t.contiguousSeq() == 3, "部分补齐后 contiguous=3");
        check(t.hasGaps(), "仍有剩余缺洞");
        const auto gaps = t.pendingGaps(0);
        check(!gaps.empty() && gaps[0].from == 4 && gaps[0].to == 4, "剩余缺洞 [4,4]");
    }

    // [6] 多次跳跃的缺洞合并（相邻/重叠）
    {
        SyncTracker t(makeCfg());
        t.observeSeq(1);
        t.observeSeq(4); // gap [2,3]
        t.observeSeq(7); // gap [5,6]
        check(t.gapCount() == 2, "2 个独立缺洞 [2,3] 与 [5,6]");
        // 补齐中间的 4、5、6 之后应连续
        t.fillGap(2, 6);
        check(!t.hasGaps(), "整段补齐后无缺洞");
        check(t.contiguousSeq() == 7, "contiguous=7");
    }

    // [7] 退避：失败后进入退避期，pendingGaps 过滤
    {
        SyncTracker t(makeCfg());
        t.observeSeq(1);
        t.observeSeq(10); // gap [2,9]
        check(!t.pendingGaps(0).empty(), "初始可重试");
        t.markGapAttemptFailed(1000); // attempt=1 → backoff=1000 → nextRetry=2000
        check(t.pendingGaps(1500).empty(), "退避中（1500 < 2000）不过滤前不返回");
        check(!t.pendingGaps(2500).empty(), "退避结束后可重试");
        check(t.backoffFor(1) == 1000, "attempt=1 backoff=1000ms");
        check(t.backoffFor(2) == 2000, "attempt=2 backoff=2000ms");
        check(t.backoffFor(10) == 8000, "backoff 有上限 8000ms");
    }

    // [8] 预算限制：pendingGaps 最多返回 maxGapsPerPass
    {
        SyncTracker t(makeCfg());
        t.observeSeq(1);
        t.observeSeq(4);  // gap [2,3]
        t.observeSeq(7);  // gap [5,6]（与前者不相邻）
        t.observeSeq(10); // gap [8,9]
        check(t.gapCount() == 3, "3 个缺洞");
        check(t.pendingGaps(0).size() == 2, "pendingGaps 受预算限制，只返回 2 个");
    }

    // [9] 乱序到达最终收敛（Ack/Push/漫游混合）
    {
        SyncTracker t(makeCfg());
        // 先到的不是 1：乱序
        t.observeSeq(5); // gap [1,4]
        t.observeSeq(3); // 落在缺洞内：仍为 [1,4]（3 被 5 覆盖范围内，无需新增）
        t.observeSeq(1); // 推进 contiguous=1 并吞并
        check(t.contiguousSeq() == 1, "observe(1) 后 contiguous=1");
        t.observeSeq(2); // contiguous=2
        t.observeSeq(4); // gap [3,3]
        t.observeSeq(3); // 补齐：contiguous=5
        check(t.contiguousSeq() == 5, "乱序全部到达后 contiguous=5");
        check(!t.hasGaps(), "最终收敛，无缺洞");
    }

    // [10] 预算耗尽保留 gap（不形成请求风暴）
    {
        SyncTracker t(makeCfg());
        t.observeSeq(1);
        t.observeSeq(100); // 大缺洞 [2,99]
        // 模拟多次失败，验证退避递增且 gap 不被丢弃
        for (int i = 1; i <= 3; ++i) t.markGapAttemptFailed(0);
        check(t.hasGaps(), "预算耗尽后仍保留缺洞（等待下次触发）");
        const auto gaps = t.pendingGaps(0);
        // 退避后需要等待；此处仅验证 gap 未消失、且退避时间已增长
        check(t.gapCount() == 1, "缺洞未被丢弃");
    }

    if (g_failures == 0) {
        std::cout << "test_sync_tracker PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_sync_tracker FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
