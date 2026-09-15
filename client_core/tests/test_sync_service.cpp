// P7-G4：SyncService（同步水位/缺洞编排）测试。
//
// 覆盖 v2 §3 P7-G4 同步部分：
//   - 连续 seq 推进水位；
//   - 跳跃到达留下缺洞（水位不回退）；
//   - 漫游补齐 fillGap 填平缺洞并推进水位；
//   - 补洞失败指数退避（pendingGaps 过滤退避期）；
//   - sync_gaps 影子落库 + 补洞后清除；
//   - 多会话水位/缺洞隔离。

#include <iostream>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "client_core/storage/NativeDatabase.h"
#include "client_core/storage/NativeRepository.h"
#include "client_core/sync/SyncService.h"
#include "client_core/testing/DeterministicClock.h"

using namespace im::storage;
using im::sync::SyncService;
using im::testing::DeterministicClock;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& name)
{
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << std::endl;
    if (!cond) ++g_failures;
}

const char* kDir = "/tmp/test_sync_service";
const std::int64_t kOwner = 1;

std::vector<unsigned char> testKey() { return std::vector<unsigned char>(32, 0x77); }

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
} // namespace

int main()
{
    std::cout << "=== test_sync_service ===" << std::endl;

    ::system(("rm -rf " + std::string(kDir)).c_str());
    ::system(("mkdir -p " + std::string(kDir)).c_str());

    DeterministicClock clock;
    NativeDatabase db;
    std::string err;
    check(db.open(kDir, kOwner, testKey(), &err), "打开库 " + err);
    NativeRepository repo(&db);
    SyncService sync(kOwner, &repo, &clock);

    // [1] 连续 seq 推进水位
    {
        check(sync.observeSeq(100, 1, &err), "会话100 observe 1 " + err);
        check(sync.observeSeq(100, 2, &err), "会话100 observe 2 " + err);
        check(sync.observeSeq(100, 3, &err), "会话100 observe 3 " + err);
        check(sync.contiguousSeq(100) == 3, "连续水位=3");
        check(sync.maxSeenSeq(100) == 3, "maxSeen=3");
        check(!sync.hasGaps(100), "无缺洞");
    }

    // [2] 跳跃产生缺洞（水位不回退）
    {
        check(sync.observeSeq(101, 1, &err), "101 observe 1");
        check(sync.observeSeq(101, 2, &err), "101 observe 2");
        check(sync.observeSeq(101, 5, &err), "101 observe 5（跳跃）");
        check(sync.contiguousSeq(101) == 2, "跳跃后水位停在 2");
        check(sync.maxSeenSeq(101) == 5, "maxSeen=5");
        check(sync.hasGaps(101), "有缺洞");
        auto gaps = sync.pendingGaps(101);
        check(gaps.size() == 1, "1 个缺洞");
        if (!gaps.empty()) check(gaps[0].from == 3 && gaps[0].to == 4, "缺洞 [3,4]");
    }

    // [3] 漫游补齐推进水位
    {
        check(sync.fillGap(101, 3, 4, &err), "补齐 [3,4] " + err);
        check(sync.contiguousSeq(101) == 5, "补洞后水位推进到 5");
        check(!sync.hasGaps(101), "缺洞已补");
        check(sync.maxSeenSeq(101) == 5, "maxSeen 保持 5");
    }

    // [4] 补洞失败退避 + pendingGaps 过滤
    {
        check(sync.observeSeq(102, 1, &err), "102 observe 1");
        check(sync.observeSeq(102, 2, &err), "102 observe 2");
        check(sync.observeSeq(102, 5, &err), "102 observe 5");
        check(sync.pendingGaps(102).size() == 1, "退避前有 1 个可重试缺洞");

        check(sync.markGapFailed(102, &err), "记录补洞失败 " + err);
        check(sync.pendingGaps(102).empty(), "退避期内无可重试缺洞");

        clock.advance(1001); // baseBackoffMs=1000
        check(sync.pendingGaps(102).size() == 1, "退避期后可重试");
    }

    // [5] sync_gaps 影子落库 + 补洞后清除
    {
        check(sync.observeSeq(103, 1, &err), "103 observe 1");
        check(sync.observeSeq(103, 3, &err), "103 observe 3（缺 2）");
        check(queryInt(db, "SELECT count(*) FROM sync_gaps WHERE owner_id=1 AND conversation_id=103") == 1,
              "sync_gaps 落库 1 条");
        check(queryInt(db, "SELECT gap_from FROM sync_gaps WHERE conversation_id=103") == 2,
              "gap_from=2");
        check(queryInt(db, "SELECT gap_to FROM sync_gaps WHERE conversation_id=103") == 2,
              "gap_to=2");

        check(sync.fillGap(103, 2, 2, &err), "补齐 [2,2]");
        check(queryInt(db, "SELECT count(*) FROM sync_gaps WHERE conversation_id=103") == 0,
              "补洞后 sync_gaps 清除");
    }

    // [6] 多会话隔离
    {
        check(sync.contiguousSeq(100) == 3, "会话 100 水位不受其他会话影响");
        check(sync.contiguousSeq(101) == 5, "会话 101 水位=5");
        check(sync.contiguousSeq(102) == 2, "会话 102 水位停在 2");
        check(sync.contiguousSeq(103) == 3, "会话 103 补洞后水位=3");
    }

    // [7] 进程重启后必须从真实 messages 重建，不能把 maxseen 当连续水位
    {
        for(const auto seq:{1LL,2LL,5LL}){
            im::dto::MessageDto m;m.ownerId=kOwner;m.conversationId=104;m.peerId=104;
            m.msgId="restore-"+std::to_string(seq);m.seq=seq;m.ts=seq;m.content="x";m.status=2;
            bool inserted=false;
            check(repo.commitIncomingMessage(m,&inserted,&err),"写入恢复样本 seq="+std::to_string(seq));
        }
        SyncService restored(kOwner,&repo,&clock);
        check(restored.contiguousSeq(104)==2,"重启恢复 contiguous=2，而不是 maxseen=5");
        const auto gaps=restored.pendingGaps(104);
        check(gaps.size()==1&&gaps[0].from==3&&gaps[0].to==4,"重启恢复真实缺洞 [3,4]");
        check(restored.maxSeenSeq(104)==5,"重启恢复 maxSeen=5");
    }

    db.close();
    ::system(("rm -rf " + std::string(kDir)).c_str());

    if (g_failures == 0) {
        std::cout << "test_sync_service PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_sync_service FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
