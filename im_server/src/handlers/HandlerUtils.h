#pragma once

#include "Database.h"
#include "Session.h"
#include "client_core/Protocol.h"
#include "im.pb.h"

#include <memory>
#include <string>

namespace imsrv::handlers {

template <typename T>
bool parsePayload(const std::string& payload, T& out)
{
    return out.ParseFromArray(payload.data(), static_cast<int>(payload.size()));
}

inline void sendFriendInfo(Database& db, const std::shared_ptr<Session>& to,
                           int userId, int onlineStatus)
{
    UserRecord user;
    if (!db.getUser(userId, user)) return;

    im::proto::FriendInfo info;
    info.set_userid(user.id);
    info.set_iconid(user.iconId);
    info.set_status(onlineStatus);
    info.set_nick(user.nick);
    info.set_feeling(user.feeling);
    to->deliver(im::proto::DEF_PROT_FRIEND_INFO, info.SerializeAsString());
}

// 离线补发和消息漫游共用同一套 StoredMessage -> Protobuf 映射。
inline bool fillChatInfo(im::proto::ChatInfoRq& out, const StoredMessage& message)
{
    out.set_myid(message.senderId);
    out.set_friid(message.receiverId);
    out.set_msg_id(message.msgId);
    out.set_ts(message.ts);
    out.set_seq(message.seq);
    if (message.type == 1) {
        out.set_type(im::proto::IMAGE);
        out.set_image_width(message.imgW);
        out.set_image_height(message.imgH);
        out.set_file_id(message.fileId);
        out.set_file_size(message.fileSize);
        out.set_content_type(message.contentType);
        out.set_sha256(message.sha256);
    } else if (message.type == 2) {
        out.set_type(im::proto::FILE);
        out.set_file_id(message.fileId);
        out.set_file_name(message.content);
        out.set_file_size(message.fileSize);
        out.set_content_type(message.contentType);
        out.set_sha256(message.sha256);
    } else {
        out.set_type(im::proto::TEXT);
        out.set_msg(message.content);
    }
    return true;
}

} // namespace imsrv::handlers
