#include "client_core/message/MessageService.h"

#include <atomic>
#include <chrono>
#include <openssl/rand.h>
#include <stdexcept>
#include <algorithm>
#include <limits>
#include "client_core/Protocol.h"

namespace im {
namespace message {

namespace {

/** 128-bit CSPRNG identity; independent of process restart and wall clock. */
std::string defaultMsgId()
{
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) throw std::runtime_error("msg_id entropy unavailable");
    const char* hex = "0123456789abcdef";
    std::string id;
    for (auto b : bytes) { id += hex[b >> 4]; id += hex[b & 15]; }
    return id;
}

} // namespace

SendResult MessageService::prepareOutgoing(const im::dto::MessageDto& intent)
{
    SendResult out;
    if (!m_repo) {
        out.error = "Repository 为空";
        return out;
    }
    if (intent.ownerId <= 0 || intent.ownerId>std::numeric_limits<std::int32_t>::max() ||
        intent.peerId<=0 || intent.peerId>std::numeric_limits<std::int32_t>::max() || intent.conversationId <= 0) {
        out.error = "ownerId/conversationId 非法";
        return out;
    }

    std::int64_t localOrder = 0;
    std::string msgId;
    try { msgId = !intent.msgId.empty() ? intent.msgId : (m_idGen ? m_idGen() : defaultMsgId()); }
    catch (const std::exception& e) { out.error = e.what(); return out; }

    im::dto::MessageDto m = intent;
    m.msgId = msgId;
    m.localOrder = localOrder;
    m.seq = 0;     // 未确认：conversation_seq = 0
    m.ts = 0;      // 未确认：server_time = 0
    m.status = 0;  // Sending
    m.fromMe = true;
    // Preserve identity even when commit times out; callers must query/retry this same msgId.
    out.msgId = msgId;

    std::string err;
    if (!m_repo->commitOutgoingDraft(m, &err, &localOrder)) {
        out.error = err;
        return out;
    }

    out.ok = true;
    out.msgId = msgId;
    out.localOrder = localOrder;
    return out;
}

bool MessageService::onSendAck(std::int64_t ownerId, const std::string& msgId,
                               std::int64_t serverTime, std::int64_t conversationSeq,
                               std::int32_t status, bool* updated, std::string* err)
{
    if (!m_repo) {
        if (err) *err = "Repository 为空";
        return false;
    }
    return m_repo->commitAck(ownerId, msgId, serverTime, conversationSeq, status, updated, err);
}

IncomingResult MessageService::onIncoming(const im::dto::MessageDto& m,
                                          const im::storage::IncomingContext& ctx)
{
    IncomingResult out;
    if (!m_repo) {
        out.error = "Repository 为空";
        return out;
    }
    std::string err;
    if (!m_repo->commitIncomingMessage(m, &out.inserted, &err, ctx)) {
        out.error = err;
        return out;
    }
    out.ok = true;
    return out;
}

bool MessageService::markRead(std::int64_t ownerId,std::int64_t conversationId,
                              std::int64_t readSeq,std::string* err)
{
    return m_repo&&m_repo->markConversationRead(ownerId,conversationId,readSeq,err);
}

bool MessageService::onProtocolAck(std::int64_t ownerId,const std::string& msgId,
    int result,std::int64_t seq,std::int64_t attempt,std::int64_t now,std::string* err,
    std::int64_t* retryAt)
{
    if(retryAt)*retryAt=0;
    if(!m_repo || now<0 || now>std::numeric_limits<std::int64_t>::max()-90) return false;
    switch(result){
        case im::proto::CHAT_RESULT_SUCC:
        case im::proto::CHAT_RESULT_FAIL:
            if(seq<=0) { if(err)*err="accepted ACK missing seq"; return false; }
            // ChatInfoRs has no timestamp. Never fabricate a server timestamp from receipt time.
            return m_repo->commitAck(ownerId,msgId,0,seq,
                result==im::proto::CHAT_RESULT_SUCC?1:3,nullptr,err);
        case im::proto::CHAT_RESULT_NOT_FRIEND:
        case im::proto::CHAT_RESULT_FILE_NOT_OWNED:
            return m_repo->finishOutboxAttempt(ownerId,msgId,attempt,true,0,
                result==im::proto::CHAT_RESULT_NOT_FRIEND?"not_friend":"file_not_owned",err);
        case im::proto::CHAT_RESULT_SERVER_ERROR: {
            if(attempt<=0) return false;
            const auto delay=std::min<std::int64_t>(60,1LL << std::min<std::int64_t>(attempt-1,6));
            unsigned char jitter=0;
            if(RAND_bytes(&jitter,1)!=1) {if(err)*err="retry entropy unavailable";return false;}
            const auto next=now+delay+(jitter%(delay/4+1));
            if(retryAt)*retryAt=next;
            return m_repo->finishOutboxAttempt(ownerId,msgId,attempt,false,
                next,"server_error",err);
        }
        default: if(err)*err="unknown ChatInfoRs.result"; return false;
    }
}

} // namespace message
} // namespace im
