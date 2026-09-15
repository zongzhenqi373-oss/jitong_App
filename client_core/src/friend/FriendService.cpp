#include "client_core/friend/FriendService.h"

namespace im {
namespace friend_service {

bool FriendService::onFriendInfo(const im::dto::FriendDto& f, std::string* err)
{
    if (!m_repo) {
        if (err) *err = "Repository 为空";
        return false;
    }
    return m_repo->upsertFriend(m_ownerId, f, err);
}

bool FriendService::onFriendRequest(const im::dto::FriendRequestDto& r, bool* inserted,
                                    std::string* err)
{
    if (!m_repo) {
        if (err) *err = "Repository 为空";
        return false;
    }
    return m_repo->upsertFriendRequest(m_ownerId, r, inserted, err);
}

bool FriendService::acceptFriendRequest(const std::string& requestId,
                                        const im::dto::FriendDto& accepted, std::string* err)
{
    if (!m_repo) {
        if (err) *err = "Repository 为空";
        return false;
    }
    return m_repo->acceptFriendRequest(m_ownerId, requestId, accepted, err);
}

bool FriendService::rejectFriendRequest(const std::string& requestId, std::string* err)
{
    if (!m_repo) {
        if (err) *err = "Repository 为空";
        return false;
    }
    return m_repo->setFriendRequestState(m_ownerId, requestId,
                                         im::dto::RequestState::Rejected, err);
}

bool FriendService::deleteFriend(std::int64_t friendId, bool* deleted, std::string* err)
{
    if (!m_repo) {
        if (err) *err = "Repository 为空";
        return false;
    }
    const bool ok = m_repo->deleteFriend(m_ownerId, friendId, deleted, err);
    if (ok) {
        std::lock_guard<std::mutex> lk(m_presenceMutex);
        m_online.erase(friendId);
    }
    return ok;
}

void FriendService::onPresence(std::int64_t friendId, bool online)
{
    std::lock_guard<std::mutex> lk(m_presenceMutex);
    m_online[friendId] = online;
}

bool FriendService::loadFriends(std::vector<im::dto::FriendDto>* out, std::string* err)
{
    if (!m_repo || !out) {
        if (err) *err = "Repository 为空";
        return false;
    }
    if (!m_repo->loadFriends(m_ownerId, out, err)) return false;
    std::lock_guard<std::mutex> lk(m_presenceMutex);
    for (auto& f : *out) {
        auto it = m_online.find(f.friendId);
        if (it != m_online.end()) f.online = it->second;
    }
    return true;
}

bool FriendService::loadFriendRequests(std::vector<im::dto::FriendRequestDto>* out,
                                       std::string* err)
{
    if (!m_repo || !out) {
        if (err) *err = "Repository 为空";
        return false;
    }
    return m_repo->loadFriendRequests(m_ownerId, out, err);
}

} // namespace friend_service
} // namespace im
