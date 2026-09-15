// 失效通知总线（P7-G1 事件契约）。
//
// 契约（对应 v2 §3 P7-G1 事件契约）：
//   1. **状态是可重放快照，事件只是 invalidation**：观察者收到通知后应重新 query，
//      而不是把事件当作增量数据来"应用"；
//   2. **invalidation 携带单调 dbVersion**：版本跳跃（错过中间版本）时观察者必须重新 query；
//   3. **同 domain 的 invalidation 可合并**：只保留最大 dbVersion，避免通知风暴；
//   4. **网络 IO 与 DB Writer 永不等待 Kotlin collector**：publish 是非阻塞的记账操作；
//   5. **进度类事件允许合并或丢弃，但业务完成事件禁止静默 drop**（由 CompletionRegistry 保证）。

#ifndef CLIENT_CORE_RUNTIME_INVALIDATION_BUS_H
#define CLIENT_CORE_RUNTIME_INVALIDATION_BUS_H

#include <cstdint>
#include <map>
#include <mutex>

namespace im {
namespace runtime {

class InvalidationBus {
public:
    enum class Domain : std::int32_t {
        Conversations = 0,
        Messages = 1,
        Friends = 2,
        Transfers = 3,
        Account = 4,
    };

    /** 当前（最新）dbVersion；未发布过返回 0。 */
    std::int64_t current(Domain d) const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_current.find(d);
        return it == m_current.end() ? 0 : it->second;
    }

    /**
     * 发布一次失效通知。
     * @return false 表示 dbVersion 未单调（回退或重复），此时**不更新**
     */
    bool publish(Domain d, std::int64_t dbVersion)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_current.find(d);
        const std::int64_t cur = (it == m_current.end()) ? 0 : it->second;
        if (dbVersion <= cur) return false; // 非单调：拒绝
        m_current[d] = dbVersion;
        return true;
    }

    /**
     * 消费并返回自上次消费以来的最新 dbVersion（合并多次 publish）。
     * @return 0 表示无新变更；>0 表示观察者应重新 query
     */
    std::int64_t consume(Domain d)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_current.find(d);
        if (it == m_current.end()) return 0;
        auto cit = m_consumed.find(d);
        const std::int64_t last = (cit == m_consumed.end()) ? 0 : cit->second;
        if (it->second <= last) return 0; // 无新变更
        m_consumed[d] = it->second;
        return it->second;
    }

    /** 观察者是否落后（存在未消费的变更）。 */
    bool isStale(Domain d) const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_current.find(d);
        if (it == m_current.end()) return false;
        auto cit = m_consumed.find(d);
        const std::int64_t last = (cit == m_consumed.end()) ? 0 : cit->second;
        return it->second > last;
    }

    /** 重置（换账号/登出时必须调用，避免跨账号版本混淆）。 */
    void reset()
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_current.clear();
        m_consumed.clear();
    }

private:
    mutable std::mutex m_mutex;
    std::map<Domain, std::int64_t> m_current;
    std::map<Domain, std::int64_t> m_consumed;
};

} // namespace runtime
} // namespace im

#endif // CLIENT_CORE_RUNTIME_INVALIDATION_BUS_H
