// 同步服务（P7-G4）。
//
// 职责：把纯逻辑状态机 SyncTracker 与持久化（sync_gaps 表）编排成一个账号级服务。
// 每个会话懒创建独立 SyncTracker；observeSeq/fillGap/markGapFailed 更新内存状态后，
// 立即把派生的缺洞（含退避信息）全量同步到 sync_gaps 表（影子落库，供快照/诊断）。
//
// 语义（v2 §3 P7-G4）：
//   - seq 连续推进水位；跳跃到达留下缺洞；漫游补齐后水位自动推进；
//   - 无 roam_range_v1 时用分页逐页追平，受页数/退避预算约束，不形成请求风暴；
//   - 网络收发由上层 transport/SDK 接线驱动；本类只做水位/缺洞决策与影子持久化。

#ifndef CLIENT_CORE_SYNC_SYNC_SERVICE_H
#define CLIENT_CORE_SYNC_SYNC_SERVICE_H

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "client_core/storage/NativeRepository.h"
#include "client_core/sync/SyncTracker.h"
#include "client_core/testing/DeterministicClock.h"

namespace im {
namespace sync {

class SyncService {
public:
    SyncService(std::int64_t ownerId, im::storage::NativeRepository* repo,
                im::testing::IClock* clock)
        : m_ownerId(ownerId), m_repo(repo), m_clock(clock) {}

    /** 观察一个 seq（来自 Push / Ack / 漫游）。 */
    bool observeSeq(std::int64_t conversationId, std::int64_t seq,
                    std::string* err = nullptr);

    /** 漫游补齐 [from,to]：填平缺洞并推进水位。 */
    bool fillGap(std::int64_t conversationId, std::int64_t from, std::int64_t to,
                 std::string* err = nullptr);

    /** 记录最前缺洞一次补洞失败：指数退避 + 持久化。 */
    bool markGapFailed(std::int64_t conversationId, std::string* err = nullptr);

    /** 当前可重试的缺洞（过滤退避期，受 maxGapsPerPass 约束）。 */
    std::vector<SyncTracker::Gap> pendingGaps(std::int64_t conversationId) const;

    /** 连续水位 / 已见最大 seq / 是否有缺洞。 */
    std::int64_t contiguousSeq(std::int64_t conversationId) const;
    std::int64_t maxSeenSeq(std::int64_t conversationId) const;
    bool hasGaps(std::int64_t conversationId) const;
    /** 全部缺洞中最早的退避截止时间；0 表示无缺洞或可立即重试。 */
    std::int64_t nextRetryAtMs(std::int64_t conversationId) const;

private:
    SyncTracker& trackerFor(std::int64_t conversationId);
    void persistGaps(std::int64_t conversationId);

    std::int64_t m_ownerId;
    im::storage::NativeRepository* m_repo;
    im::testing::IClock* m_clock;

    mutable std::mutex m_mutex;
    std::map<std::int64_t, SyncTracker> m_trackers; // conversationId → tracker
};

} // namespace sync
} // namespace im

#endif // CLIENT_CORE_SYNC_SYNC_SERVICE_H
