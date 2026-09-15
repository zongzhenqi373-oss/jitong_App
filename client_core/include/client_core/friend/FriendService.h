// 好友服务（P7-G4）。
//
// 职责：好友列表、申请列表、同意/拒绝、删除、资料幂等落库。
//
// 关键设计（v2 §3 P7-G4）：
//   - 好友**事实**（nick/tel/avatar/signature/sex）落库 friends 表，服务端资料为权威；
//   - **Presence（在线/离线）只表达路由状态，不是好友事实源**——onPresence 只更新
//     内存在线表，绝不写 friends 表；loadFriends 时把在线状态合并到返回结果。
//     这样"好友事实与在线事件分离"，不会因一条 Presence 抖动而污染事实数据。
//   - 同意申请在 Repository 内**同一事务**完成「申请状态→Accepted + 好友落库」，
//     避免半事务可见（申请已同意但好友未落库）。
//   - 网络收发由上层 transport/SDK 接线驱动；本类只做本地状态决策与落库编排，
//     不持有 socket、不拼接裸 SQL。

#ifndef CLIENT_CORE_FRIEND_FRIEND_SERVICE_H
#define CLIENT_CORE_FRIEND_FRIEND_SERVICE_H

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "client_core/dto/Dtos.h"
#include "client_core/storage/NativeRepository.h"

namespace im {
namespace friend_service {

class FriendService {
public:
    /** 绑定账号与 Repository（一个实例对应一个账号）。 */
    FriendService(std::int64_t ownerId, im::storage::NativeRepository* repo)
        : m_ownerId(ownerId), m_repo(repo) {}

    // ---------------- 好友事实（服务端资料为权威，幂等落库） ----------------

    bool onFriendInfo(const im::dto::FriendDto& f, std::string* err = nullptr);

    // ---------------- 好友申请 ----------------

    /** 申请幂等落库（request_id 唯一）；@return inserted 是否首次入库。 */
    bool onFriendRequest(const im::dto::FriendRequestDto& r, bool* inserted = nullptr,
                         std::string* err = nullptr);

    /** 同意申请：同事务 状态→Accepted + 好友落库。 */
    bool acceptFriendRequest(const std::string& requestId, const im::dto::FriendDto& accepted,
                             std::string* err = nullptr);

    /** 拒绝申请：状态→Rejected。 */
    bool rejectFriendRequest(const std::string& requestId, std::string* err = nullptr);

    // ---------------- 删除 ----------------

    bool deleteFriend(std::int64_t friendId, bool* deleted = nullptr,
                      std::string* err = nullptr);

    // ---------------- Presence（在线事件，不入库） ----------------

    /** 在线/离线事件：只更新内存路由状态，不落库。 */
    void onPresence(std::int64_t friendId, bool online);

    // ---------------- 查询 ----------------

    /** 好友列表（事实 + 合并内存在线状态）。 */
    bool loadFriends(std::vector<im::dto::FriendDto>* out, std::string* err = nullptr);

    /** 申请列表。 */
    bool loadFriendRequests(std::vector<im::dto::FriendRequestDto>* out,
                            std::string* err = nullptr);

private:
    std::int64_t m_ownerId;
    im::storage::NativeRepository* m_repo;

    mutable std::mutex m_presenceMutex;
    std::unordered_map<std::int64_t, bool> m_online; // friendId → online
};

} // namespace friend_service
} // namespace im

#endif // CLIENT_CORE_FRIEND_FRIEND_SERVICE_H
