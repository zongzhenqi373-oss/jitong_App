#include "handlers/MessageHandler.h"

#include "db/Database.h"
#include "media/HttpFileServer.h"
#include "common/Log.h"
#include "session/Presence.h"
#include "session/Session.h"
#include "handlers/HandlerUtils.h"
#include "im.pb.h"
#include "sha256.h"

#include <atomic>
#include <ctime>

namespace imsrv {
using namespace im::proto;

namespace {
std::int64_t nowSec() { return static_cast<std::int64_t>(std::time(nullptr)); }

std::string fallbackMsgId(int sender, int receiver, const std::string& content)
{
    static std::atomic<std::uint64_t> counter{0};
    return im::sha256Hex(std::to_string(sender) + "|" + std::to_string(receiver) + "|" +
                         std::to_string(nowSec()) + "|" +
                         std::to_string(counter.fetch_add(1)) + "|" + content);
}

void deliverFailure(const std::shared_ptr<Session>& session, const ChatInfoRq& rq, int result)
{
    ChatInfoRs rs;
    rs.set_myid(rq.friid());
    rs.set_friid(rq.myid());
    rs.set_msg_id(rq.msg_id());
    rs.set_result(result);
    rs.set_seq(0);
    session->deliver(DEF_PROT_CHAT_INFO_RS, rs.SerializeAsString());
}
} // namespace

MessageHandler::MessageHandler(Database& db, Presence& presence, HttpFileServer& files)
    : m_db(db), m_presence(presence), m_files(files)
{
}

void MessageHandler::onChat(const std::shared_ptr<Session>& session, const std::string& payload)
{
    ChatInfoRq rq;
    if (!handlers::parsePayload(payload, rq)) return;

    const int userId = session->userId();
    if (userId <= 0) return;
    rq.set_myid(userId);
    if (!m_db.isFriend(userId, rq.friid())) {
        deliverFailure(session, rq, CHAT_RESULT_NOT_FRIEND);
        return;
    }

    std::string mediaPath;
    std::int64_t fileSize = 0;
    HttpFileServer::UploadRecord thumbnailRecord;
    bool hasThumbnail = false;
    HttpFileServer::UploadRecord largeThumbnailRecord;
    bool hasLargeThumbnail = false;
    if (rq.type() == IMAGE || rq.type() == im::proto::FILE) {
        HttpFileServer::UploadRecord record;
        if (!m_files.findUploadRecord(rq.file_id(), record) ||
            record.uploaderId != userId || record.receiverId != rq.friid()) {
            deliverFailure(session, rq, CHAT_RESULT_FILE_NOT_OWNED);
            return;
        }
        mediaPath = record.mediaPath;
        fileSize = record.size;
        rq.set_file_size(record.size);
        rq.set_content_type(record.contentType);
        rq.set_sha256(record.sha256);
        if (rq.type() == IMAGE && !rq.thumbnail_file_id().empty()) {
            if (!m_files.findUploadRecord(rq.thumbnail_file_id(), thumbnailRecord) ||
                thumbnailRecord.uploaderId != userId || thumbnailRecord.receiverId != rq.friid() ||
                thumbnailRecord.contentType.rfind("image/", 0) != 0) {
                deliverFailure(session, rq, CHAT_RESULT_FILE_NOT_OWNED);
                return;
            }
            hasThumbnail = true;
            rq.set_thumbnail_size(thumbnailRecord.size);
            rq.set_thumbnail_sha256(thumbnailRecord.sha256);
        }
        if (rq.type() == IMAGE && !rq.large_thumbnail_file_id().empty()) {
            if (!m_files.findUploadRecord(rq.large_thumbnail_file_id(), largeThumbnailRecord) ||
                largeThumbnailRecord.uploaderId != userId ||
                largeThumbnailRecord.receiverId != rq.friid() ||
                largeThumbnailRecord.contentType.rfind("image/", 0) != 0) {
                deliverFailure(session, rq, CHAT_RESULT_FILE_NOT_OWNED);
                return;
            }
            hasLargeThumbnail = true;
            rq.set_large_thumbnail_size(largeThumbnailRecord.size);
            rq.set_large_thumbnail_sha256(largeThumbnailRecord.sha256);
        }
    }

    StoredMessage message;
    message.msgId = rq.msg_id().empty()
        ? fallbackMsgId(rq.myid(), rq.friid(), rq.msg()) : rq.msg_id();
    message.senderId = rq.myid();
    message.receiverId = rq.friid();
    message.type = rq.type() == IMAGE ? 1 : (rq.type() == im::proto::FILE ? 2 : 0);
    message.content = rq.type() == im::proto::FILE ? rq.file_name() : rq.msg();
    message.mediaPath = mediaPath;
    message.imgW = rq.image_width();
    message.imgH = rq.image_height();
    message.fileId = rq.file_id();
    message.fileSize = fileSize;
    message.contentType = rq.content_type();
    message.sha256 = rq.sha256();
    message.thumbnailFileId = rq.thumbnail_file_id();
    message.thumbnailPath = hasThumbnail ? thumbnailRecord.mediaPath : "";
    message.thumbnailSize = rq.thumbnail_size();
    message.thumbnailSha256 = rq.thumbnail_sha256();
    message.thumbnailW = rq.thumbnail_width();
    message.thumbnailH = rq.thumbnail_height();
    message.largeThumbnailFileId = rq.large_thumbnail_file_id();
    message.largeThumbnailPath = hasLargeThumbnail ? largeThumbnailRecord.mediaPath : "";
    message.largeThumbnailSize = rq.large_thumbnail_size();
    message.largeThumbnailSha256 = rq.large_thumbnail_sha256();
    message.largeThumbnailW = rq.large_thumbnail_width();
    message.largeThumbnailH = rq.large_thumbnail_height();
    message.ts = nowSec();

    auto target = m_presence.get(rq.friid());
    if (!m_db.saveMessage(message, static_cast<bool>(target))) {
        deliverFailure(session, rq, CHAT_RESULT_SERVER_ERROR);
        log("[消息] 保存失败 msg_id=", rq.msg_id());
        return;
    }
    if (message.type == 1 || message.type == 2) {
        m_files.eraseUploadRecord(message.fileId);
        if (hasThumbnail) m_files.eraseUploadRecord(message.thumbnailFileId);
        if (hasLargeThumbnail) m_files.eraseUploadRecord(message.largeThumbnailFileId);
    }

    rq.set_msg_id(message.msgId);
    rq.set_ts(message.ts);
    rq.set_seq(message.seq);
    ChatInfoRs rs;
    rs.set_myid(rq.friid());
    rs.set_friid(rq.myid());
    rs.set_msg_id(message.msgId);
    rs.set_seq(message.seq);
    if (target) {
        target->deliver(DEF_PROT_CHAT_INFO_RQ, rq.SerializeAsString());
        rs.set_result(CHAT_RESULT_SUCC);
    } else {
        rs.set_result(CHAT_RESULT_FAIL);
    }
    session->deliver(DEF_PROT_CHAT_INFO_RS, rs.SerializeAsString());
}

} // namespace imsrv
