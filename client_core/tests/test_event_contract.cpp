// P7-G1：事件契约自检（dbVersion 失效通知 + 有界完成登记表）。
//
// 验收点（v2 §3 P7-G1）：
//   - state 可重放快照，事件只是 invalidation，携带单调 dbVersion；
//   - 同 domain 可合并为最大 dbVersion；版本跳跃时观察者重新 query 后能收敛；
//   - operation completion 按 operationId exactly-once 终结，可查询直到消费或 TTL；
//   - **禁止把业务完成事件静默 drop**：容量满且无可淘汰记录时必须拒绝而非丢弃。

#include "client_core/runtime/CompletionRegistry.h"
#include "client_core/runtime/InvalidationBus.h"
#include "client_core/testing/DeterministicClock.h"

#include <iostream>
#include <string>

using namespace im::runtime;
using im::testing::DeterministicClock;

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
    std::cout << "=== test_event_contract ===" << std::endl;

    // [1] dbVersion 必须单调递增
    {
        InvalidationBus bus;
        check(bus.publish(InvalidationBus::Domain::Messages, 1), "publish(1) 成功");
        check(!bus.publish(InvalidationBus::Domain::Messages, 1), "重复 publish(1) 被拒（非单调）");
        check(!bus.publish(InvalidationBus::Domain::Messages, 0), "回退 publish(0) 被拒");
        check(bus.publish(InvalidationBus::Domain::Messages, 2), "publish(2) 成功");
        check(bus.current(InvalidationBus::Domain::Messages) == 2, "当前版本为 2");
    }

    // [2] 同 domain 多次 publish 合并为最大版本
    {
        InvalidationBus bus;
        bus.publish(InvalidationBus::Domain::Conversations, 1);
        bus.publish(InvalidationBus::Domain::Conversations, 5);
        bus.publish(InvalidationBus::Domain::Conversations, 9);
        check(bus.current(InvalidationBus::Domain::Conversations) == 9, "合并后为最大版本 9");
        check(bus.consume(InvalidationBus::Domain::Conversations) == 9,
              "consume 返回合并后的 9（一次而非三次）");
        check(bus.consume(InvalidationBus::Domain::Conversations) == 0, "再次 consume 无新变更");
    }

    // [3] 版本跳跃：collector 暂停后仍能靠最大版本补查收敛
    {
        InvalidationBus bus;
        bus.publish(InvalidationBus::Domain::Messages, 10);
        bus.consume(InvalidationBus::Domain::Messages); // 观察者消费到 10
        // 模拟 collector 暂停期间发生多次变更（中间版本被跳过）
        bus.publish(InvalidationBus::Domain::Messages, 11);
        bus.publish(InvalidationBus::Domain::Messages, 15);
        bus.publish(InvalidationBus::Domain::Messages, 20);
        const std::int64_t v = bus.consume(InvalidationBus::Domain::Messages);
        check(v == 20, "版本跳跃后 consume 得到最新 20（观察者据此重新 query 收敛）");
    }

    // [4] stale 状态与 domain 隔离
    {
        InvalidationBus bus;
        check(!bus.isStale(InvalidationBus::Domain::Friends), "初始非 stale");
        bus.publish(InvalidationBus::Domain::Friends, 3);
        check(bus.isStale(InvalidationBus::Domain::Friends), "publish 后 stale");
        bus.consume(InvalidationBus::Domain::Friends);
        check(!bus.isStale(InvalidationBus::Domain::Friends), "consume 后非 stale");

        // domain 隔离：Friends 的变更不影响 Transfers
        bus.publish(InvalidationBus::Domain::Friends, 4);
        check(bus.current(InvalidationBus::Domain::Transfers) == 0, "Transfers 不受 Friends 影响");
        check(!bus.isStale(InvalidationBus::Domain::Transfers), "Transfers 非 stale");
    }

    // [5] 换账号/登出后 reset，避免跨账号版本混淆
    {
        InvalidationBus bus;
        bus.publish(InvalidationBus::Domain::Messages, 50);
        bus.reset();
        check(bus.current(InvalidationBus::Domain::Messages) == 0, "reset 后版本归零");
        check(bus.publish(InvalidationBus::Domain::Messages, 1), "reset 后可从小版本重新开始");
    }

    // [6] CompletionRegistry：exactly-once 终结
    {
        DeterministicClock clock;
        CompletionRegistry reg(16, &clock);
        check(reg.complete("op1", CompletionRegistry::Result::Ok), "op1 首次终结成功");
        check(!reg.complete("op1", CompletionRegistry::Result::Failed),
              "op1 重复终结被拒（exactly-once）");

        CompletionRegistry::Record rec;
        check(reg.peek("op1", &rec) && rec.result == CompletionRegistry::Result::Ok,
              "终态保持首次结果 Ok（未被第二次覆盖）");
        check(!reg.complete("", CompletionRegistry::Result::Ok), "空 operationId 被拒");
    }

    // [7] 有界 + 禁止静默 drop：容量满且无可淘汰时拒绝登记
    {
        DeterministicClock clock;
        CompletionRegistry reg(2, &clock);
        check(reg.complete("op1", CompletionRegistry::Result::Ok), "op1 登记");
        check(reg.complete("op2", CompletionRegistry::Result::Ok), "op2 登记");
        check(reg.size() == 2, "达到容量 2");
        check(!reg.complete("op3", CompletionRegistry::Result::Ok),
              "op3 被拒（容量满且全未消费 → 不静默丢弃）");
        check(reg.unconsumedCount() == 2, "两条未消费记录仍在（完成事件未丢）");

        // 消费一条后可淘汰，新登记成功
        check(reg.consume("op1"), "消费 op1");
        check(reg.complete("op3", CompletionRegistry::Result::Ok), "消费后 op3 可登记（淘汰 op1）");
        check(!reg.peek("op1", nullptr), "op1 已被淘汰");
        check(reg.peek("op3", nullptr), "op3 在登记表中");
    }

    // [8] TTL 淘汰：过期记录可被清理，未过期且未消费的不得被淘汰
    {
        DeterministicClock clock(1000);
        CompletionRegistry reg(16, &clock);
        reg.complete("old", CompletionRegistry::Result::Ok); // createdAt = 1000

        // 推进时间后，再登记一条新的（这样它才算"未过期"）
        clock.advance(5000);                                   // now = 6000
        reg.complete("fresh", CompletionRegistry::Result::Ok);  // createdAt = 6000

        // TTL=1000：old 已过 5000ms → 过期；fresh 刚创建 → 未过期且未消费 → 不淘汰
        const std::size_t evicted = reg.evictExpired(1000);
        check(evicted == 1, "只淘汰 1 条过期记录");
        check(!reg.peek("old", nullptr), "过期记录已淘汰");
        check(reg.peek("fresh", nullptr), "未过期未消费记录仍在（禁止静默 drop）");
        check(reg.unconsumedCount() == 1, "未消费记录数为 1");
    }

    // [9] 消费过的记录即使未过期也可被淘汰（TTLive 之前释放空间）
    {
        DeterministicClock clock(1000);
        CompletionRegistry reg(16, &clock);
        reg.complete("op1", CompletionRegistry::Result::Ok);
        reg.consume("op1");
        const std::size_t evicted = reg.evictExpired(1000000); // TTL 远未到
        check(evicted == 1, "已消费记录在 TTL 前也可淘汰");
        check(reg.size() == 0, "登记表已空");
    }

    // [10] 容量满时的背压语义：调用方可据此落持久队列而非丢事件
    {
        DeterministicClock clock;
        CompletionRegistry reg(1, &clock);
        check(reg.complete("op1", CompletionRegistry::Result::Ok), "容量 1：op1 登记");
        check(!reg.complete("op2", CompletionRegistry::Result::Ok),
              "op2 被拒（调用方应背压或落持久队列，而非丢弃）");
        check(reg.unconsumedCount() == 1, "op1 完成事件仍可查询，未被丢弃");
    }

    if (g_failures == 0) {
        std::cout << "test_event_contract PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_event_contract FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
