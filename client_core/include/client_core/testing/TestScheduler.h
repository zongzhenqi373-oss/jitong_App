// 确定性调度器（P7-G1 Harness）。
//
// 目的：替代真实线程 sleep / 定时器。任务按**虚拟时间**到期执行，
// 测试通过 advance() 精确控制推进，不产生真实等待，也不依赖线程时序。
//
// 语义：
//   - post(delay, fn)：在 nowMs() + delay 到期；
//   - advance(delta)：把时钟推进 delta，并**按到期时间顺序**执行到期任务；
//   - 任务内再 post 的新任务若到期时间 <= 当前虚拟时间，会在同一次 advance 内被执行；
//   - cancel(id)：取消尚未执行的任务。

#ifndef CLIENT_CORE_TESTING_TEST_SCHEDULER_H
#define CLIENT_CORE_TESTING_TEST_SCHEDULER_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "client_core/testing/DeterministicClock.h"

namespace im {
namespace testing {

class TestScheduler {
public:
    explicit TestScheduler(DeterministicClock* clock) : m_clock(clock) {}

    /** 延迟 delayMs 执行；返回任务 id（可用于 cancel）。 */
    std::uint64_t postDelayed(std::int64_t delayMs, std::function<void()> fn)
    {
        const std::uint64_t id = m_nextId++;
        const std::int64_t due = m_clock->nowMs() + (delayMs > 0 ? delayMs : 0);
        m_tasks.push_back(Task{id, due, m_seq++, std::move(fn)});
        return id;
    }

    std::uint64_t post(std::function<void()> fn) { return postDelayed(0, std::move(fn)); }

    bool cancel(std::uint64_t id)
    {
        auto it = std::find_if(m_tasks.begin(), m_tasks.end(),
                               [id](const Task& t) { return t.id == id; });
        if (it == m_tasks.end()) return false;
        m_tasks.erase(it);
        return true;
    }

    /** 推进虚拟时间 deltaMs，并执行所有到期任务（按 due 升序、同 due 按入队顺序）。 */
    void advance(std::int64_t deltaMs)
    {
        if (deltaMs > 0) m_clock->advance(deltaMs);
        runDue();
    }

    /** 不推进时间，只执行当前已到期的任务（含任务内新 post 的立即任务）。 */
    void runDue()
    {
        // 循环：任务内可能 post 新的到期任务
        for (int guard = 0; guard < 10000; ++guard) {
            std::vector<Task> due;
            const std::int64_t now = m_clock->nowMs();
            auto it = std::partition(m_tasks.begin(), m_tasks.end(),
                                     [now](const Task& t) { return t.due > now; });
            if (it == m_tasks.end()) break;
            due.assign(it, m_tasks.end());
            m_tasks.erase(it, m_tasks.end());
            std::stable_sort(due.begin(), due.end(), [](const Task& a, const Task& b) {
                if (a.due != b.due) return a.due < b.due;
                return a.seq < b.seq;
            });
            for (auto& t : due) {
                if (t.fn) t.fn();
            }
        }
    }

    std::size_t pendingCount() const { return m_tasks.size(); }

    /** 下一个任务的到期时间；无任务返回 -1。 */
    std::int64_t nextDueMs() const
    {
        if (m_tasks.empty()) return -1;
        return std::min_element(m_tasks.begin(), m_tasks.end(),
                                [](const Task& a, const Task& b) { return a.due < b.due; })
            ->due;
    }

private:
    struct Task {
        std::uint64_t id;
        std::int64_t due;
        std::uint64_t seq;
        std::function<void()> fn;
    };

    DeterministicClock* m_clock;
    std::vector<Task> m_tasks;
    std::uint64_t m_nextId = 1;
    std::uint64_t m_seq = 0;
};

} // namespace testing
} // namespace im

#endif // CLIENT_CORE_TESTING_TEST_SCHEDULER_H
