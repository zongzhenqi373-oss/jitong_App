// 同步水位与缺洞状态机（P7-G4）。
//
// 职责：维护每个会话的 contiguousSeq（连续水位）、maxSeenSeq（已见最大 seq）与缺洞区间。
//
// 数据模型（关键设计）：
//   以**已到达区间 arrived** 为主结构，缺洞是其相邻区间之间的空隙。
//   早期版本以"缺失区间"为主，会导致错误：当 observeSeq(3) 落在 gap[2,4] 内时，
//   会把尚未到达的 4 一并当作已补齐而推进水位。以 arrived 为主可天然避免该问题。
//
// 语义（v2 §3 P7-G4）：
//   - `seq <= contiguousSeq`：幂等，**不回退水位**；
//   - 跳跃到达：在 arrived 中留下空隙，即缺洞；
//   - 补齐（漫游返回）后空隙被填平，水位自动推进；
//   - 无 `roam_range_v1` 时用 beforeSeq 分页逐页追平，受**页数/时长/退避**预算约束；
//     预算耗尽保留 gap 并等待下次触发，**不形成请求风暴**。
//
// 本类是纯逻辑状态机（不直接访问数据库），便于单元测试与 Golden 复现；
// 持久化由调用方按 sync_gaps 表落地。

#ifndef CLIENT_CORE_SYNC_SYNC_TRACKER_H
#define CLIENT_CORE_SYNC_SYNC_TRACKER_H

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

namespace im {
namespace sync {

class SyncTracker {
public:
    /** 缺洞区间（闭区间），由 arrived 派生。 */
    struct Gap {
        std::int64_t from = 0;
        std::int64_t to = 0;
        int attempt = 0;                 // 已尝试次数（指数退避）
        std::int64_t nextRetryAtMs = 0;  // 下次可重试时间（0 = 立即可试）
    };

    struct Config {
        std::int64_t ownerId = 0;
        std::int64_t conversationId = 0;
        int maxPagesPerPass = 5;
        int maxGapsPerPass = 3;
        std::int64_t baseBackoffMs = 1000;
        std::int64_t maxBackoffMs = 60 * 1000;
    };

    explicit SyncTracker(const Config& cfg) : m_cfg(cfg) {}

    /** 用消息表重建的已到达区间恢复；持久 gap 只提供 attempt/退避元数据。 */
    void restore(const std::vector<std::pair<std::int64_t,std::int64_t>>& arrived,
                 const std::vector<Gap>& persistedGaps)
    {
        m_arrived.clear();m_retries.clear();m_maxSeen=0;
        for(const auto& r:arrived)if(r.first>0&&r.second>=r.first){
            addArrived(r.first,r.second);m_maxSeen=std::max(m_maxSeen,r.second);
        }
        for(const auto& g:persistedGaps)if(g.from>0&&g.to>=g.from&&g.attempt>0)
            m_retries[g.from]={g.attempt,g.nextRetryAtMs};
    }

    /** 观察一个 seq（来自 Push / 漫游 / Ack）。@return 是否推进了连续水位 */
    bool observeSeq(std::int64_t seq)
    {
        if (seq <= 0) return false;
        const std::int64_t before = contiguousSeq();
        if (seq > m_maxSeen) m_maxSeen = seq;
        addArrived(seq, seq);
        return contiguousSeq() > before;
    }

    /** 标记一段区间已到达（漫游补齐后调用）。 */
    void fillGap(std::int64_t from, std::int64_t to)
    {
        if (from > to) return;
        if (to > m_maxSeen) m_maxSeen = to;
        addArrived(from, to);
    }

    /** 记录最前一个待补缺洞的一次失败：增加 attempt 并按指数退避。 */
    void markGapAttemptFailed(std::int64_t nowMs)
    {
        std::vector<Gap> gs = computeGaps();
        if (gs.empty()) return;
        const Gap& g = gs.front();
        RetryInfo& ri = m_retries[g.from];
        ++ri.attempt;
        ri.nextRetryAtMs = nowMs + backoffFor(ri.attempt);
    }

    /** 当前可重试的缺洞（过滤退避期），最多返回 maxGapsPerPass 个。 */
    std::vector<Gap> pendingGaps(std::int64_t nowMs) const
    {
        std::vector<Gap> out;
        for (const auto& g : gapsWithRetry()) {
            if (g.nextRetryAtMs > nowMs) continue; // 退避中
            out.push_back(g);
            if (static_cast<int>(out.size()) >= m_cfg.maxGapsPerPass) break;
        }
        return out;
    }

    /** 全部缺洞（含退避中的）。 */
    std::vector<Gap> gaps() const { return gapsWithRetry(); }

    bool hasGaps() const { return !computeGaps().empty(); }
    std::size_t gapCount() const { return computeGaps().size(); }

    /** 连续水位：从 1 起算的最大连续已到达 seq。 */
    std::int64_t contiguousSeq() const
    {
        if (m_arrived.empty()) return 0;
        if (m_arrived.front().from != 1) return 0; // 起点未到达 → 尚未连续
        return m_arrived.front().to;
    }

    std::int64_t maxSeenSeq() const { return m_maxSeen; }

    std::int64_t backoffFor(int attempt) const
    {
        std::int64_t v = m_cfg.baseBackoffMs;
        for (int i = 1; i < attempt && v < m_cfg.maxBackoffMs; ++i) v *= 2;
        return std::min(v, m_cfg.maxBackoffMs);
    }

    const Config& config() const { return m_cfg; }

private:
    struct Range {
        std::int64_t from = 0;
        std::int64_t to = 0;
    };
    struct RetryInfo {
        int attempt = 0;
        std::int64_t nextRetryAtMs = 0;
    };

    /** 插入 [from,to] 并与相邻/重叠区间合并（保持有序、不重叠）。 */
    void addArrived(std::int64_t from, std::int64_t to)
    {
        if (from > to) return;
        // 找到第一个 to >= from-1 的区间（可合并：相邻或重叠）
        auto it = std::lower_bound(m_arrived.begin(), m_arrived.end(), from - 1,
                                   [](const Range& r, std::int64_t v) { return r.to < v; });
        if (it != m_arrived.end() && it->from <= to + 1) {
            it->from = std::min(it->from, from);
            it->to = std::max(it->to, to);
            auto next = it + 1;
            while (next != m_arrived.end() && next->from <= it->to + 1) {
                it->to = std::max(it->to, next->to);
                next = m_arrived.erase(next);
            }
            return;
        }
        m_arrived.insert(it, Range{from, to});
    }

    /** 由 arrived 派生缺洞：相邻区间之间的空隙 + [1, 首区间) 的前导空隙。 */
    std::vector<Gap> computeGaps() const
    {
        std::vector<Gap> out;
        if (m_arrived.empty()) return out;
        // 前导：1 .. 首区间.from-1
        if (m_arrived.front().from > 1) out.push_back(Gap{1, m_arrived.front().from - 1, 0, 0});
        for (std::size_t i = 0; i + 1 < m_arrived.size(); ++i) {
            const std::int64_t gf = m_arrived[i].to + 1;
            const std::int64_t gt = m_arrived[i + 1].from - 1;
            if (gf <= gt) out.push_back(Gap{gf, gt, 0, 0});
        }
        return out;
    }

    /** 派生缺洞并附加退避信息（按缺洞起点匹配）。 */
    std::vector<Gap> gapsWithRetry() const
    {
        std::vector<Gap> out = computeGaps();
        for (auto& g : out) {
            auto it = m_retries.find(g.from);
            if (it != m_retries.end()) {
                g.attempt = it->second.attempt;
                g.nextRetryAtMs = it->second.nextRetryAtMs;
            }
        }
        return out;
    }

    Config m_cfg;
    std::int64_t m_maxSeen = 0;
    std::vector<Range> m_arrived;          // 已到达区间（有序、不重叠）
    std::map<std::int64_t, RetryInfo> m_retries; // 按缺洞起点记录退避
};

} // namespace sync
} // namespace im

#endif // CLIENT_CORE_SYNC_SYNC_TRACKER_H
