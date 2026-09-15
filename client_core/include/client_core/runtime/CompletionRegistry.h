// 操作完成登记表（P7-G1 事件契约）。
//
// 契约（对应 v2 §3 P7-G1 事件契约）：
//   - 每个 operationId **最终只能有一个终态**（exactly-once），重复 complete 被拒绝；
//   - 容量**有界**，避免 UI 长时间不消费导致无限增长；
//   - 记录**可查询直到被确认消费或 TTL 过期**，之后才允许淘汰；
//   - **禁止静默 drop**：容量满时必须淘汰"已消费或已过期"的旧记录；
//     若全是未消费且未过期的记录，则拒绝登记并返回 false（由调用方决定背压或落持久队列），
//     而不是悄悄丢掉一个业务完成事件。

#ifndef CLIENT_CORE_RUNTIME_COMPLETION_REGISTRY_H
#define CLIENT_CORE_RUNTIME_COMPLETION_REGISTRY_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "client_core/testing/DeterministicClock.h"

namespace im {
namespace runtime {

class CompletionRegistry {
public:
    /** 终态结果（避免与 storage::CommandResult 耦合，这里用通用枚举）。 */
    enum class Result : std::int32_t {
        Ok = 0,
        Cancelled = 1,
        Failed = 2,
        Timeout = 3,
    };

    struct Record {
        std::string operationId;
        Result result = Result::Ok;
        bool consumed = false;
        std::int64_t createdAtMs = 0;
        std::string errorMessage;
    };

    CompletionRegistry(std::size_t capacity, im::testing::IClock* clock)
        : m_capacity(capacity), m_clock(clock)
    {
    }

    /**
     * 登记一个终态。
     * @return false 表示：该 operationId 已有终态（重复终结），或容量已满且无法淘汰
     */
    bool complete(const std::string& operationId, Result r, const std::string& error = {})
    {
        if (operationId.empty()) return false;
        std::lock_guard<std::mutex> lk(m_mutex);

        if (m_records.count(operationId) > 0) return false; // exactly-once：已有终态

        if (m_records.size() >= m_capacity) {
            if (!evictOneLocked()) return false; // 无可淘汰 → 拒绝，交由调用方背压
        }

        Record rec;
        rec.operationId = operationId;
        rec.result = r;
        rec.errorMessage = error;
        rec.createdAtMs = m_clock ? m_clock->nowMs() : 0;
        m_records.emplace(operationId, rec);
        return true;
    }

    /** 查询终态（不标记为已消费）。 */
    bool peek(const std::string& operationId, Record* out) const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_records.find(operationId);
        if (it == m_records.end()) return false;
        if (out) *out = it->second;
        return true;
    }

    /** 消费终态（标记为已消费，之后可被淘汰）。 */
    bool consume(const std::string& operationId, Record* out = nullptr)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_records.find(operationId);
        if (it == m_records.end()) return false;
        if (out) *out = it->second;
        it->second.consumed = true;
        return true;
    }

    /** 淘汰已消费或已过期（TTL）的记录；返回淘汰条数。 */
    std::size_t evictExpired(std::int64_t ttlMs)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        const std::int64_t now = m_clock ? m_clock->nowMs() : 0;
        std::size_t n = 0;
        for (auto it = m_records.begin(); it != m_records.end();) {
            const bool expired = (now - it->second.createdAtMs) >= ttlMs;
            if (it->second.consumed || expired) {
                it = m_records.erase(it);
                ++n;
            } else {
                ++it;
            }
        }
        return n;
    }

    std::size_t size() const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_records.size();
    }

    std::size_t capacity() const { return m_capacity; }

    /** 未消费记录数（用于断言"没有完成事件被静默丢弃"）。 */
    std::size_t unconsumedCount() const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        std::size_t n = 0;
        for (const auto& kv : m_records) {
            if (!kv.second.consumed) ++n;
        }
        return n;
    }

    void reset()
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_records.clear();
    }

private:
    /** 淘汰一条"已消费"的记录；无则返回 false（调用方需先检查容量）。 */
    bool evictOneLocked()
    {
        for (auto it = m_records.begin(); it != m_records.end(); ++it) {
            if (it->second.consumed) {
                m_records.erase(it);
                return true;
            }
        }
        return false;
    }

    mutable std::mutex m_mutex;
    std::size_t m_capacity;
    im::testing::IClock* m_clock;
    std::map<std::string, Record> m_records;
};

} // namespace runtime
} // namespace im

#endif // CLIENT_CORE_RUNTIME_COMPLETION_REGISTRY_H
