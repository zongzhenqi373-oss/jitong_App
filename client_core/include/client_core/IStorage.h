#pragma once
// 本地存储抽象接口：业务层只依赖此接口，后续可替换 wcdb / sqlcipher 实现

#include <string>
#include <vector>
#include "Types.h"

namespace im {

class IStorage {
public:
    virtual ~IStorage() = default;

    // 打开/关闭数据库
    virtual bool open(const std::string& dbPath) = 0;
    virtual void close() = 0;

    // 保存自己的资料（存在则更新）
    virtual bool saveSelfInfo(const UserInfo& info) = 0;

    // 保存好友资料（存在则更新）
    virtual bool saveFriend(const FriendInfo& info) = 0;

    // 读取全部好友
    virtual std::vector<FriendInfo> loadFriends() = 0;

    // 追加一条聊天记录（ownerId 为当前登录用户 id）
    virtual bool saveChatMessage(int ownerId, int peerId, bool outgoing,
                                 const std::string& contentUtf8, std::int64_t ts) = 0;

    // 读取与某好友的聊天记录（按时间升序，limit<=0 表示全部）
    virtual std::vector<ChatMessage> loadChatHistory(int ownerId, int peerId, int limit) = 0;
};

} // namespace im
