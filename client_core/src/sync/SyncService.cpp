#include "client_core/sync/SyncService.h"

#include <chrono>

namespace im {
namespace sync {

namespace {
std::int64_t nowMs(im::testing::IClock* clock)
{
    if(clock)return clock->nowMs();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
}

SyncTracker& SyncService::trackerFor(std::int64_t conversationId)
{
    auto it = m_trackers.find(conversationId);
    if (it == m_trackers.end()) {
        SyncTracker::Config cfg;
        cfg.ownerId = m_ownerId;
        cfg.conversationId = conversationId;
        it = m_trackers.emplace(conversationId, SyncTracker(cfg)).first;
        if(m_repo){
            std::vector<im::storage::MessageSeqRange> ranges;
            std::vector<im::storage::SyncGapRow> persisted;
            if(m_repo->loadMessageSeqRanges(m_ownerId,conversationId,&ranges)&&
               m_repo->loadSyncGaps(m_ownerId,conversationId,&persisted)){
                std::vector<std::pair<std::int64_t,std::int64_t>> arrived;
                std::vector<SyncTracker::Gap> gaps;
                for(const auto& r:ranges)arrived.emplace_back(r.from,r.to);
                for(const auto& g:persisted)gaps.push_back({g.gapFrom,g.gapTo,g.attempt,g.nextRetryAtMs});
                it->second.restore(arrived,gaps);
            }
        }
    }
    return it->second;
}

void SyncService::persistGaps(std::int64_t conversationId)
{
    if (!m_repo) return;
    const SyncTracker& tr = trackerFor(conversationId);
    std::vector<im::storage::SyncGapRow> rows;
    for (const auto& g : tr.gaps()) {
        im::storage::SyncGapRow r;
        r.gapFrom = g.from;
        r.gapTo = g.to;
        r.attempt = g.attempt;
        r.nextRetryAtMs = g.nextRetryAtMs;
        rows.push_back(r);
    }
    std::string err;
    m_repo->replaceSyncGaps(m_ownerId, conversationId, rows, &err);
}

bool SyncService::observeSeq(std::int64_t conversationId, std::int64_t seq, std::string* err)
{
    if (conversationId <= 0 || seq <= 0) {
        if (err) *err = "invalid observe seq";
        return false;
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    trackerFor(conversationId).observeSeq(seq);
    persistGaps(conversationId);
    return true;
}

bool SyncService::fillGap(std::int64_t conversationId, std::int64_t from, std::int64_t to,
                          std::string* err)
{
    if (conversationId <= 0 || from <= 0 || to < from) {
        if (err) *err = "invalid fill gap";
        return false;
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    trackerFor(conversationId).fillGap(from, to);
    persistGaps(conversationId);
    return true;
}

bool SyncService::markGapFailed(std::int64_t conversationId, std::string* err)
{
    if (conversationId <= 0) {
        if (err) *err = "invalid conversation";
        return false;
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    const std::int64_t now = nowMs(m_clock);
    trackerFor(conversationId).markGapAttemptFailed(now);
    persistGaps(conversationId);
    return true;
}

std::vector<SyncTracker::Gap> SyncService::pendingGaps(std::int64_t conversationId) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if(conversationId<=0)return {};
    auto& tracker=const_cast<SyncService*>(this)->trackerFor(conversationId);
    const std::int64_t now = nowMs(m_clock);
    return tracker.pendingGaps(now);
}

std::int64_t SyncService::contiguousSeq(std::int64_t conversationId) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if(conversationId<=0)return 0;
    return const_cast<SyncService*>(this)->trackerFor(conversationId).contiguousSeq();
}

std::int64_t SyncService::maxSeenSeq(std::int64_t conversationId) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if(conversationId<=0)return 0;
    return const_cast<SyncService*>(this)->trackerFor(conversationId).maxSeenSeq();
}

bool SyncService::hasGaps(std::int64_t conversationId) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if(conversationId<=0)return false;
    return const_cast<SyncService*>(this)->trackerFor(conversationId).hasGaps();
}

std::int64_t SyncService::nextRetryAtMs(std::int64_t conversationId) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if(conversationId<=0)return 0;
    const auto gaps=const_cast<SyncService*>(this)->trackerFor(conversationId).gaps();
    std::int64_t earliest=0;
    for(const auto& gap:gaps)
        if(gap.nextRetryAtMs>0&&(earliest==0||gap.nextRetryAtMs<earliest))earliest=gap.nextRetryAtMs;
    return earliest;
}

} // namespace sync
} // namespace im
