// P7-G4：FriendService（好友域）测试。
//
// 覆盖 v2 §3 P7-G4 好友部分：
//   - 好友资料幂等落库（插入 + 全量覆盖更新）；
//   - 好友申请幂等落库（request_id 唯一）；
//   - 同意申请：同事务 申请状态→Accepted + 好友落库（不重复插）；
//   - 拒绝申请：状态→Rejected，不产生好友；
//   - 删除好友；
//   - Presence（在线事件）与好友事实分离：不入库、loadFriends 时合并；
//   - 好友/申请按 ownerId 隔离。

#include <iostream>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "client_core/dto/Dtos.h"
#include "client_core/friend/FriendService.h"
#include "client_core/storage/NativeDatabase.h"
#include "client_core/storage/NativeRepository.h"

using namespace im::storage;
using im::dto::FriendDto;
using im::dto::FriendRequestDto;
using im::dto::RequestDirection;
using im::dto::RequestState;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& name)
{
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << std::endl;
    if (!cond) ++g_failures;
}

const char* kDir = "/tmp/test_friend_service";
const std::int64_t kOwner = 1;

std::vector<unsigned char> testKey() { return std::vector<unsigned char>(32, 0x55); }

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

std::string queryText(NativeDatabase& db, const std::string& sql)
{
    std::string v;
    db.withRead([&](sqlite3* d) {
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(d, sql.c_str(), -1, &s, nullptr) == SQLITE_OK && s) {
            if (sqlite3_step(s) == SQLITE_ROW) {
                const unsigned char* t = sqlite3_column_text(s, 0);
                if (t) v = reinterpret_cast<const char*>(t);
            }
        }
        if (s) sqlite3_finalize(s);
    });
    return v;
}

FriendDto makeFriend(std::int64_t id, const std::string& nick)
{
    FriendDto f;
    f.friendId = id;
    f.nick = nick;
    return f;
}

FriendRequestDto makeRequest(const std::string& id, std::int64_t from, std::int64_t createdAt)
{
    FriendRequestDto r;
    r.requestId = id;
    r.fromUserId = from;
    r.toUserId = kOwner;
    r.direction = RequestDirection::Incoming;
    r.state = RequestState::Pending;
    r.message = "hello";
    r.createdAt = createdAt;
    return r;
}
} // namespace

int main()
{
    std::cout << "=== test_friend_service ===" << std::endl;

    ::system(("rm -rf " + std::string(kDir)).c_str());
    ::system(("mkdir -p " + std::string(kDir)).c_str());

    NativeDatabase db;
    std::string err;
    check(db.open(kDir, kOwner, testKey(), &err), "打开库 " + err);
    NativeRepository repo(&db);
    im::friend_service::FriendService svc(kOwner, &repo);

    // [1] 好友资料幂等落库：插入 + 全量覆盖更新
    {
        FriendDto f = makeFriend(100, "Alice");
        f.tel = "13800000000";
        f.avatar = "http://a/100.png";
        f.signature = "hi";
        f.sex = 1;
        check(svc.onFriendInfo(f, &err), "落库好友 100 " + err);
        check(queryInt(db, "SELECT count(*) FROM friends WHERE owner_id=1") == 1,
              "friends 有 1 行");
        check(queryText(db, "SELECT nick FROM friends WHERE owner_id=1 AND friend_id=100") == "Alice",
              "nick=Alice");
        check(queryText(db, "SELECT tel FROM friends WHERE owner_id=1 AND friend_id=100") == "13800000000",
              "tel 已落库");
        check(queryInt(db, "SELECT sex FROM friends WHERE owner_id=1 AND friend_id=100") == 1,
              "sex=1");

        // 同 friendId 更新：覆盖 nick，不增行
        FriendDto f2 = makeFriend(100, "Alice2");
        check(svc.onFriendInfo(f2, &err), "更新好友 100 " + err);
        check(queryInt(db, "SELECT count(*) FROM friends WHERE owner_id=1") == 1,
              "更新不增行");
        check(queryText(db, "SELECT nick FROM friends WHERE owner_id=1 AND friend_id=100") == "Alice2",
              "nick 更新为 Alice2");
    }

    // [2] 申请幂等落库
    {
        bool ins = false;
        check(svc.onFriendRequest(makeRequest("r1", 200, 1000), &ins, &err), "落库申请 r1 " + err);
        check(ins, "首次申请 inserted=true");
        check(queryInt(db, "SELECT count(*) FROM friend_requests WHERE owner_id=1") == 1,
              "friend_requests 有 1 行");

        bool ins2 = true;
        check(svc.onFriendRequest(makeRequest("r1", 200, 1000), &ins2, &err), "重复申请 r1 " + err);
        check(!ins2, "重复申请 inserted=false");
        check(queryInt(db, "SELECT count(*) FROM friend_requests WHERE owner_id=1") == 1,
              "重复申请不增行");
        check(queryInt(db, "SELECT state FROM friend_requests WHERE request_id='r1'") == 0,
              "申请 state 仍 pending(0)");
    }

    // [3] 同意申请：同事务 状态→Accepted + 好友落库
    {
        FriendDto bob = makeFriend(200, "Bob");
        bob.sex = 0;
        check(svc.acceptFriendRequest("r1", bob, &err), "同意申请 r1 " + err);
        check(queryInt(db, "SELECT state FROM friend_requests WHERE request_id='r1'") == 1,
              "同意后申请 state=Accepted(1)");
        check(queryInt(db, "SELECT count(*) FROM friends WHERE friend_id=200") == 1,
              "同意后好友 200 落库");
        check(queryText(db, "SELECT nick FROM friends WHERE friend_id=200") == "Bob",
              "好友 200 nick=Bob");
    }

    // [4] 拒绝申请：状态→Rejected，不产生好友
    {
        check(svc.onFriendRequest(makeRequest("r2", 300, 2000), nullptr, &err), "落库申请 r2 " + err);
        check(svc.rejectFriendRequest("r2", &err), "拒绝申请 r2 " + err);
        check(queryInt(db, "SELECT state FROM friend_requests WHERE request_id='r2'") == 2,
              "拒绝后申请 state=Rejected(2)");
        check(queryInt(db, "SELECT count(*) FROM friends WHERE friend_id=300") == 0,
              "拒绝不产生好友 300");
    }

    // [5] 同意已处理申请：幂等，不重复插好友
    {
        check(svc.acceptFriendRequest("r1", makeFriend(200, "Bob"), &err),
              "再次同意 r1 " + err);
        check(queryInt(db, "SELECT count(*) FROM friends WHERE friend_id=200") == 1,
              "重复同意不重复插好友 200");
    }

    // [6] Presence 与好友事实分离
    {
        const long long before = queryInt(db, "SELECT count(*) FROM friends WHERE owner_id=1");
        svc.onPresence(100, true); // 在线事件：只更新内存，不落库
        svc.onPresence(999, true); // 未知好友的在线事件也不落库
        const long long after = queryInt(db, "SELECT count(*) FROM friends WHERE owner_id=1");
        check(before == after, "Presence 事件不改变 friends 表");

        std::vector<FriendDto> friends;
        check(svc.loadFriends(&friends, &err), "loadFriends " + err);
        bool foundAlice = false;
        for (const auto& f : friends) {
            if (f.friendId == 100) {
                foundAlice = true;
                check(f.online, "好友 100 在线状态已合并（online=true）");
            }
        }
        check(foundAlice, "loadFriends 返回好友 100");

        svc.onPresence(100, false);
        friends.clear();
        check(svc.loadFriends(&friends, &err), "再次 loadFriends " + err);
        for (const auto& f : friends) {
            if (f.friendId == 100) check(!f.online, "离线事件后 online=false");
        }
    }

    // [7] 删除好友
    {
        bool deleted = false;
        check(svc.deleteFriend(100, &deleted, &err), "删除好友 100 " + err);
        check(deleted, "删除好友 deleted=true");
        check(queryInt(db, "SELECT count(*) FROM friends WHERE friend_id=100") == 0,
              "删除后好友 100 不存在");

        bool deleted2 = true;
        check(svc.deleteFriend(100, &deleted2, &err), "再次删除好友 100 " + err);
        check(!deleted2, "再次删除 deleted=false（不存在）");
    }

    // [8] 好友/申请按 ownerId 隔离
    {
        std::vector<FriendDto> friends;
        std::vector<FriendRequestDto> reqs;
        // 用另一个 owner 的 Repository 实例查询（同一库，不同 owner）
        im::friend_service::FriendService other(2, &repo);
        check(other.loadFriends(&friends, &err), "owner2 loadFriends " + err);
        check(friends.empty(), "owner2 看不到 owner1 的好友（隔离）");
        check(other.loadFriendRequests(&reqs, &err), "owner2 loadFriendRequests " + err);
        check(reqs.empty(), "owner2 看不到 owner1 的申请（隔离）");
    }

    // [9] 申请列表读取（含排序）
    {
        std::vector<FriendRequestDto> reqs;
        check(svc.loadFriendRequests(&reqs, &err), "loadFriendRequests " + err);
        check(reqs.size() == 2, "owner1 有 2 条申请（r1、r2）");
        // 按 created_at DESC：r2(2000) 在 r1(1000) 之前
        if (reqs.size() == 2) {
            check(reqs[0].requestId == "r2", "列表按 created_at 降序（r2 在前）");
        }
    }

    db.close();
    ::system(("rm -rf " + std::string(kDir)).c_str());

    if (g_failures == 0) {
        std::cout << "test_friend_service PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_friend_service FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
