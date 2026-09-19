#include "client_core/storage/NativeRepository.h"
#include "client_core/storage/MessageMediaColumns.h"
#include "client_core/Protocol.h"
#include "im.pb.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <limits>

namespace im {
namespace storage {

namespace {

std::string outgoingPayload(const im::dto::MessageDto& m)
{
    im::proto::ChatInfoRq r;
    r.set_myid(m.ownerId); r.set_friid(m.peerId); r.set_msg_id(m.msgId);
    r.set_msg(m.content); r.set_type(static_cast<im::proto::MsgType>(m.type));
    r.set_image_width(m.imgW); r.set_image_height(m.imgH);
    r.set_file_name(m.fileName); r.set_file_size(m.fileSize); r.set_file_id(m.fileId);
    r.set_content_type(m.contentType); r.set_sha256(m.sha256);
    r.set_thumbnail_file_id(m.thumbnailFileId); r.set_thumbnail_width(m.thumbnailW);
    r.set_thumbnail_height(m.thumbnailH); r.set_thumbnail_size(m.thumbnailSize);
    r.set_thumbnail_sha256(m.thumbnailSha256);
    r.set_large_thumbnail_file_id(m.largeThumbnailFileId);
    r.set_large_thumbnail_width(m.largeThumbnailW); r.set_large_thumbnail_height(m.largeThumbnailH);
    r.set_large_thumbnail_size(m.largeThumbnailSize); r.set_large_thumbnail_sha256(m.largeThumbnailSha256);
    // No transport sequence, token or encrypted record is persisted across sessions.
    return r.SerializeAsString();
}

void bindText(sqlite3_stmt* s, int i, const std::string& v)
{
    sqlite3_bind_text(s, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
}

bool readMessage(sqlite3* db, std::int64_t owner, const std::string& id, im::dto::MessageDto* out)
{
    if (!out) return false;
    const auto sql=std::string("SELECT owner_id,msg_id,conversation_id,peer_id,conversation_seq,")+
        "server_time,local_order,from_me,type,content,status,"+mediaSelectColumns()+
        ",COALESCE((SELECT pinyin FROM message_fts_identity f WHERE f.owner_id=messages.owner_id AND f.msg_id=messages.msg_id),''),"
        "COALESCE((SELECT initials FROM message_fts_identity f WHERE f.owner_id=messages.owner_id AND f.msg_id=messages.msg_id),'') "
        "FROM messages WHERE owner_id=? AND msg_id=?";
    sqlite3_stmt* s=nullptr;
    if(sqlite3_prepare_v2(db,sql.c_str(),-1,&s,nullptr)!=SQLITE_OK) return false;
    sqlite3_bind_int64(s,1,owner); bindText(s,2,id);
    const bool found=sqlite3_step(s)==SQLITE_ROW;
    if(found){
        auto text=[&](int i){const auto* p=sqlite3_column_text(s,i);return p?std::string(reinterpret_cast<const char*>(p),sqlite3_column_bytes(s,i)):std::string();};
        im::dto::MessageDto m;
        m.ownerId=sqlite3_column_int64(s,0); m.msgId=text(1); m.conversationId=sqlite3_column_int64(s,2);
        m.peerId=sqlite3_column_int64(s,3); m.seq=sqlite3_column_int64(s,4); m.ts=sqlite3_column_int64(s,5);
        m.localOrder=sqlite3_column_int64(s,6); m.fromMe=sqlite3_column_int(s,7)!=0;
        m.type=sqlite3_column_int(s,8); m.content=text(9); m.status=sqlite3_column_int(s,10);
        readMediaColumns(s,11,m); m.pinyin=text(33); m.initials=text(34);
        *out=std::move(m);
    }
    sqlite3_finalize(s); return found;
}

bool execSql(sqlite3* db, const char* sql)
{
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (err) {
        sqlite3_free(err);
    }
    return rc == SQLITE_OK;
}

bool compatibleIdentity(sqlite3* db, const im::dto::MessageDto& m)
{
    sqlite3_stmt* s = nullptr;
    const char* sql = "SELECT conversation_id,peer_id,from_me,type,conversation_seq FROM messages "
                      "WHERE owner_id=? AND msg_id=?";
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(s,1,m.ownerId); bindText(s,2,m.msgId);
    const int rc = sqlite3_step(s);
    const bool ok = rc == SQLITE_DONE || (rc == SQLITE_ROW &&
        sqlite3_column_int64(s,0)==m.conversationId && sqlite3_column_int64(s,1)==m.peerId &&
        sqlite3_column_int(s,2)==(m.fromMe?1:0) && sqlite3_column_int(s,3)==m.type &&
        (m.seq==0 || sqlite3_column_int64(s,4)==0 || sqlite3_column_int64(s,4)==m.seq));
    sqlite3_finalize(s);
    return ok;
}

bool finishProjection(sqlite3* db, const im::dto::MessageDto& m, bool draft,
                      const IncomingContext& ctx = {})
{
    const auto displayTime = draft ? std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() : m.ts;
    sqlite3_stmt* s = nullptr;
    // Called after the basic conversation upsert. Stable ties use seq/order/id;
    // ACK only refreshes its own preview and never overwrites a newer message.
    const std::string query =
        "UPDATE conversations SET last_msg_id=?1,last_message=?2,last_message_time=MAX(last_message_time,?3) "
        "WHERE owner_id=?4 AND conversation_id=?5 AND (?6=1 OR (last_msg_id='' AND ?3>=last_message_time) OR last_msg_id=?1 OR "
        "?3>last_message_time OR (?3=last_message_time AND (?7,?8,?1)>"
        "(COALESCE((SELECT conversation_seq FROM messages WHERE owner_id=?4 AND msg_id=conversations.last_msg_id),0),"
        "COALESCE((SELECT local_order FROM messages WHERE owner_id=?4 AND msg_id=conversations.last_msg_id),0),last_msg_id)))";
    if (sqlite3_prepare_v2(db, query.c_str(), -1, &s, nullptr) != SQLITE_OK) return false;
    bindText(s,1,m.msgId); bindText(s,2,m.content); sqlite3_bind_int64(s,3,displayTime);
    sqlite3_bind_int64(s,4,m.ownerId); sqlite3_bind_int64(s,5,m.conversationId);
    sqlite3_bind_int(s,6,draft?1:0); sqlite3_bind_int64(s,7,m.seq); sqlite3_bind_int64(s,8,m.localOrder);
    bool ok = sqlite3_step(s)==SQLITE_DONE;
    sqlite3_finalize(s);
    if (!ok) return false;
    if (!m.fromMe && !ctx.isRoaming && ctx.activeConversationId==m.conversationId && m.seq>0) {
        const char* read = "UPDATE conversations SET read_seq=MAX(read_seq,?1),unread="
            "(SELECT COUNT(*) FROM messages WHERE owner_id=?2 AND conversation_id=?3 "
            "AND from_me=0 AND conversation_seq>MAX(conversations.read_seq,?1)) "
            "WHERE owner_id=?2 AND conversation_id=?3";
        if (sqlite3_prepare_v2(db,read,-1,&s,nullptr)!=SQLITE_OK) return false;
        sqlite3_bind_int64(s,1,m.seq); sqlite3_bind_int64(s,2,m.ownerId); sqlite3_bind_int64(s,3,m.conversationId);
        ok=sqlite3_step(s)==SQLITE_DONE; sqlite3_finalize(s);
    }
    return ok;
}

/**
 * 推进 maxseen 水位（**单调递增**）。
 *
 * 语义：maxseen 表示"已见过的最大 seq"，只用于判断是否见过更新的消息；
 * 它**不代表** 1..maxseen 已连续（连续性由 SyncTracker 的 contiguous 维护）。
 * 先收到 100 再收到 90 不得回退到 90，因此用 MAX 而非直接替换。
 */
bool bumpMaxSeen(sqlite3* db, std::int64_t ownerId, std::int64_t peerId, std::int64_t seq,
                 std::int64_t ts)
{
    sqlite3_stmt* sw = nullptr;
    const char* sws =
        "INSERT INTO sync_watermarks(owner_id,domain,peer_id,synced_seq,updated_at) "
        "VALUES(?,'maxseen',?,?,?) "
        "ON CONFLICT(owner_id,domain,peer_id) DO UPDATE SET "
        "synced_seq = MAX(sync_watermarks.synced_seq, excluded.synced_seq), "
        "updated_at = excluded.updated_at";
    if (sqlite3_prepare_v2(db, sws, -1, &sw, nullptr) != SQLITE_OK || !sw) return false;
    sqlite3_bind_int64(sw, 1, ownerId);
    sqlite3_bind_int64(sw, 2, peerId);
    sqlite3_bind_int64(sw, 3, seq);
    sqlite3_bind_int64(sw, 4, ts);
    const bool ok = sqlite3_step(sw) == SQLITE_DONE;
    sqlite3_finalize(sw);
    return ok;
}

/** 插入/更新 FTS 与 identity（与消息同事务维护，保证搜索与消息一致）。 */
bool upsertFts(sqlite3* db, const im::dto::MessageDto& m)
{
    // FTS4 主表
    {
        sqlite3_stmt* s = nullptr;
        const char* sql =
            "INSERT INTO message_fts(content,pinyin,initials,msg_id) VALUES(?,?,?,?)";
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK || !s) return false;
        bindText(s, 1, m.content);
        bindText(s, 2, m.pinyin);
        bindText(s, 3, m.initials);
        bindText(s, 4, m.msgId);
        const bool ok = sqlite3_step(s) == SQLITE_DONE;
        sqlite3_finalize(s);
        if (!ok) return false;
    }
    // identity 镜像（002 引入，用于按 msg_id O(1) 校验/去重）
    {
        sqlite3_stmt* s = nullptr;
        const char* sql = "INSERT OR REPLACE INTO message_fts_identity"
                          "(owner_id,msg_id,content,pinyin,initials) VALUES(?,?,?,?,?)";
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK || !s) return false;
        sqlite3_bind_int64(s, 1, m.ownerId);
        bindText(s, 2, m.msgId);
        bindText(s, 3, m.content);
        bindText(s, 4, m.pinyin);
        bindText(s, 5, m.initials);
        const bool ok = sqlite3_step(s) == SQLITE_DONE;
        sqlite3_finalize(s);
        if (!ok) return false;
    }
    return true;
}

// AI 候选列表序列化：US(0x1F) 分隔，候选文本极少含该控制字符。
std::string joinSuggestions(const std::vector<std::string>& items)
{
    std::string out;
    for (const auto& it : items) {
        if (!out.empty()) out += '\x1F';
        out += it;
    }
    return out;
}

std::vector<std::string> splitSuggestions(const std::string& s)
{
    std::vector<std::string> out;
    if (s.empty()) return out;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = s.find('\x1F', start);
        if (pos == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

} // namespace

bool NativeRepository::runTx(const DbCommandQueue::WriteFn& fn, std::string* err)
{
    if (!m_db) {
        if (err) *err = "db 为空";
        return false;
    }
    bool timedOut = false;
    const CommandResult r = m_db->submitBusinessSync(fn, std::chrono::seconds(60), &timedOut);
    if (timedOut) {
        if (err) *err = "等待 Writer 超时，操作状态未确定";
        return false;
    }
    if (r != CommandResult::Ok) {
        if (err) *err = std::string("写事务失败：") + toString(r);
        return false;
    }
    return true;
}

bool NativeRepository::beginDownloadTask(std::int64_t ownerId,const std::string& taskId,
    const std::string& msgId,const std::string& fileId,const std::string& localPath,
    std::int64_t generation,std::string* err)
{
    if(ownerId<=0||generation<=0||taskId.empty()||msgId.empty()||fileId.empty()||
       localPath.empty()||taskId.size()>128||fileId.size()>128){
        if(err)*err="invalid download task";return false;
    }
    return runTx([&](sqlite3* db){
        // 只有本账号已落库消息引用的媒体索引才能进入恢复队列。
        sqlite3_stmt* check=nullptr;
        const char* checkSql="SELECT CASE WHEN ?=file_id THEN sha256 "
            "WHEN ?=thumbnail_file_id THEN thumbnail_sha256 "
            "WHEN ?=large_thumbnail_file_id THEN large_thumbnail_sha256 ELSE '' END "
            "FROM messages WHERE owner_id=? AND msg_id=? LIMIT 1";
        if(sqlite3_prepare_v2(db,checkSql,-1,&check,nullptr)!=SQLITE_OK)return false;
        for(int i=1;i<=3;++i)bindText(check,i,fileId);
        sqlite3_bind_int64(check,4,ownerId);bindText(check,5,msgId);
        bool authorized=false;
        if(sqlite3_step(check)==SQLITE_ROW&&sqlite3_column_bytes(check,0)==64){
            const auto* p=reinterpret_cast<const char*>(sqlite3_column_text(check,0));
            authorized=p&&std::all_of(p,p+64,[](unsigned char c){return std::isxdigit(c)!=0;});
        }
        sqlite3_finalize(check);if(!authorized)return false;

        sqlite3_stmt* old=nullptr;
        const char* supersede="UPDATE transfer_tasks SET state=5,terminal_state=1,updated_at=strftime('%s','now') "
            "WHERE owner_id=? AND direction=1 AND file_id=? AND terminal_state=0";
        if(sqlite3_prepare_v2(db,supersede,-1,&old,nullptr)!=SQLITE_OK)return false;
        sqlite3_bind_int64(old,1,ownerId);bindText(old,2,fileId);
        const bool oldOk=sqlite3_step(old)==SQLITE_DONE;sqlite3_finalize(old);
        if(!oldOk)return false;
        sqlite3_stmt* st=nullptr;
        const char* sql="INSERT INTO transfer_tasks(task_id,owner_id,msg_id,file_id,direction,"
            "local_path,state,generation,phase,terminal_state,updated_at) "
            "VALUES(?,?,?,?,1,?,0,?,0,0,strftime('%s','now'))";
        if(sqlite3_prepare_v2(db,sql,-1,&st,nullptr)!=SQLITE_OK)return false;
        bindText(st,1,taskId);sqlite3_bind_int64(st,2,ownerId);bindText(st,3,msgId);
        bindText(st,4,fileId);bindText(st,5,localPath);sqlite3_bind_int64(st,6,generation);
        const bool ok=sqlite3_step(st)==SQLITE_DONE;sqlite3_finalize(st);return ok;
    },err);
}

bool NativeRepository::finishDownloadTask(std::int64_t ownerId,const std::string& taskId,
    std::int64_t generation,int state,std::int64_t transferred,std::string* err)
{
    if(ownerId<=0||taskId.empty()||generation<=0||transferred<0||
       (state!=1&&state!=2&&state!=3&&state!=4&&state!=5)){
        if(err)*err="invalid download transition";return false;
    }
    return runTx([&](sqlite3* db){
        sqlite3_stmt* st=nullptr;
        const char* sql="UPDATE transfer_tasks SET state=?,transferred=?,offset_bytes=?,"
            "terminal_state=?,updated_at=strftime('%s','now') WHERE task_id=? AND owner_id=? "
            "AND direction=1 AND generation=? AND terminal_state=0";
        if(sqlite3_prepare_v2(db,sql,-1,&st,nullptr)!=SQLITE_OK)return false;
        sqlite3_bind_int(st,1,state);sqlite3_bind_int64(st,2,transferred);
        sqlite3_bind_int64(st,3,transferred);sqlite3_bind_int(st,4,(state>=3)?1:0);
        bindText(st,5,taskId);sqlite3_bind_int64(st,6,ownerId);sqlite3_bind_int64(st,7,generation);
        const bool ok=sqlite3_step(st)==SQLITE_DONE&&sqlite3_changes(db)==1;
        sqlite3_finalize(st);return ok;
    },err);
}

bool NativeRepository::listRecoverableDownloads(std::int64_t ownerId,
    std::vector<DownloadTaskRow>* out,std::string* err)
{
    if(out)out->clear();
    if(!m_db||!out||ownerId<=0){if(err)*err="invalid download query";return false;}
    bool queryOk=false;
    const auto result=m_db->withRead([&](sqlite3* db){
        sqlite3_stmt* st=nullptr;
        const char* sql="SELECT t.task_id,t.msg_id,t.file_id,t.local_path,t.transferred,"
            "t.generation,CASE WHEN t.file_id=m.file_id THEN m.sha256 "
            "WHEN t.file_id=m.thumbnail_file_id THEN m.thumbnail_sha256 "
            "ELSE m.large_thumbnail_sha256 END,"
            "CASE WHEN t.file_id=m.file_id THEN m.file_size "
            "WHEN t.file_id=m.thumbnail_file_id THEN m.thumbnail_size "
            "ELSE m.large_thumbnail_size END "
            "FROM transfer_tasks t JOIN messages m ON m.owner_id=t.owner_id AND m.msg_id=t.msg_id "
            "WHERE t.owner_id=? AND t.direction=1 AND t.terminal_state=0 AND t.state IN(0,1,2) "
            "AND (t.file_id=m.file_id OR t.file_id=m.thumbnail_file_id OR "
            "t.file_id=m.large_thumbnail_file_id) ORDER BY t.updated_at LIMIT 128";
        if(sqlite3_prepare_v2(db,sql,-1,&st,nullptr)!=SQLITE_OK)return;
        sqlite3_bind_int64(st,1,ownerId);int rc=SQLITE_OK;
        while((rc=sqlite3_step(st))==SQLITE_ROW){
            auto str=[&](int i){const auto* p=sqlite3_column_text(st,i);
                return p?std::string(reinterpret_cast<const char*>(p),sqlite3_column_bytes(st,i)):std::string();};
            DownloadTaskRow row;row.taskId=str(0);row.msgId=str(1);row.fileId=str(2);
            row.localPath=str(3);row.transferred=sqlite3_column_int64(st,4);
            row.generation=sqlite3_column_int64(st,5);row.expectedSha256=str(6);
            row.totalSize=sqlite3_column_int64(st,7);out->push_back(std::move(row));
        }
        queryOk=rc==SQLITE_DONE;sqlite3_finalize(st);
    });
    if(result!=ReadResult::Ok||!queryOk){out->clear();if(err)*err="download query unavailable";return false;}
    return true;
}

bool NativeRepository::findMessage(std::int64_t ownerId, const std::string& msgId, im::dto::MessageDto* out)
{
    if(!m_db || !out) return false;
    bool found=false;
    m_db->withRead([&](sqlite3* db){found=readMessage(db,ownerId,msgId,out);});
    return found;
}

bool NativeRepository::listConversation(std::int64_t ownerId, std::int64_t conversationId,
                                        const MessageCursor* cursor, int limit, MessagePage* out,
                                        std::string* err)
{
    if (out) *out = {};
    if (err) err->clear();
    if (!m_db || !out || ownerId <= 0 || conversationId <= 0 || limit <= 0 || limit > 200 ||
        (cursor && cursor->msgId.empty())) {
        if (err) *err = "invalid history query";
        return false;
    }
    const MessageCursor key = cursor ? *cursor : MessageCursor{};
    bool queryOk = false;
    const ReadResult result = m_db->withRead([&](sqlite3* db) {
        const auto sql = std::string("SELECT owner_id,msg_id,conversation_id,peer_id,conversation_seq,") +
            "server_time,local_order,from_me,type,content,status," + mediaSelectColumns() +
            ",COALESCE((SELECT pinyin FROM message_fts_identity f WHERE f.owner_id=messages.owner_id AND f.msg_id=messages.msg_id),''),"
            "COALESCE((SELECT initials FROM message_fts_identity f WHERE f.owner_id=messages.owner_id AND f.msg_id=messages.msg_id),'') "
            "FROM messages WHERE owner_id=?1 AND conversation_id=?2 AND "
            "(?3=0 OR (server_time,conversation_seq,local_order,msg_id)<(?4,?5,?6,?7)) "
            "ORDER BY server_time DESC,conversation_seq DESC,local_order DESC,msg_id DESC LIMIT ?8";
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) != SQLITE_OK) return;
        sqlite3_bind_int64(s, 1, ownerId); sqlite3_bind_int64(s, 2, conversationId);
        sqlite3_bind_int(s, 3, cursor ? 1 : 0); sqlite3_bind_int64(s, 4, key.serverTime);
        sqlite3_bind_int64(s, 5, key.conversationSeq); sqlite3_bind_int64(s, 6, key.localOrder);
        bindText(s, 7, key.msgId); sqlite3_bind_int(s, 8, limit + 1);
        int rc = SQLITE_OK;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            auto text = [&](int i) { const auto* p=sqlite3_column_text(s,i); return p ?
                std::string(reinterpret_cast<const char*>(p),sqlite3_column_bytes(s,i)) : std::string(); };
            im::dto::MessageDto m;
            m.ownerId=sqlite3_column_int64(s,0); m.msgId=text(1); m.conversationId=sqlite3_column_int64(s,2);
            m.peerId=sqlite3_column_int64(s,3); m.seq=sqlite3_column_int64(s,4); m.ts=sqlite3_column_int64(s,5);
            m.localOrder=sqlite3_column_int64(s,6); m.fromMe=sqlite3_column_int(s,7)!=0;
            m.type=sqlite3_column_int(s,8); m.content=text(9); m.status=sqlite3_column_int(s,10);
            readMediaColumns(s,11,m); m.pinyin=text(33); m.initials=text(34);
            out->messages.push_back(std::move(m));
        }
        queryOk = rc == SQLITE_DONE;
        sqlite3_finalize(s);
    });
    if (result != ReadResult::Ok || !queryOk) {
        out->messages.clear();
        if (err) *err = "history database unavailable";
        return false;
    }
    if (out->messages.size() > static_cast<std::size_t>(limit)) {
        out->hasMore = true;
        out->messages.resize(static_cast<std::size_t>(limit));
    }
    if (!out->messages.empty()) {
        const auto& last = out->messages.back();
        out->next = {last.ts, last.seq, last.localOrder, last.msgId};
    }
    return true;
}

bool NativeRepository::loadConversations(std::int64_t ownerId,
    std::vector<im::dto::ConversationDto>* out,std::string* err)
{
    if(out)out->clear();if(err)err->clear();
    if(!m_db||!out||ownerId<=0){if(err)*err="invalid conversations query";return false;}
    bool queryOk=false;
    const auto result=m_db->withRead([&](sqlite3* db){
        sqlite3_stmt* s=nullptr;
        const char* sql="SELECT conversation_id,owner_id,peer_id,COALESCE(last_message,''),"
                        "last_message_time,unread FROM conversations WHERE owner_id=? "
                        "ORDER BY last_message_time DESC,conversation_id DESC";
        if(sqlite3_prepare_v2(db,sql,-1,&s,nullptr)!=SQLITE_OK)return;
        sqlite3_bind_int64(s,1,ownerId);int rc=SQLITE_OK;
        while((rc=sqlite3_step(s))==SQLITE_ROW){
            im::dto::ConversationDto c;c.conversationId=sqlite3_column_int64(s,0);
            c.ownerId=sqlite3_column_int64(s,1);c.peerId=sqlite3_column_int64(s,2);
            const auto* text=sqlite3_column_text(s,3);
            if(text)c.lastMsg.assign(reinterpret_cast<const char*>(text),sqlite3_column_bytes(s,3));
            c.lastTs=sqlite3_column_int64(s,4);c.unread=sqlite3_column_int64(s,5);
            out->push_back(std::move(c));
        }
        queryOk=rc==SQLITE_DONE;sqlite3_finalize(s);
    });
    if(result!=ReadResult::Ok||!queryOk){out->clear();if(err)*err="conversations database unavailable";return false;}
    return true;
}

bool NativeRepository::claimOutbox(std::int64_t ownerId, std::int64_t now, std::int64_t lease,
                                  int limit, std::vector<OutboxAttempt>* out, std::string* err)
{
    if(out) out->clear();
    if(!out || ownerId<=0 || now<0 || lease<=0 || lease>3600 || limit<=0 || limit>64 ||
       now>std::numeric_limits<std::int64_t>::max()-lease) return false;
    auto attempts=std::make_shared<std::vector<OutboxAttempt>>();
    const bool ok=runTx([=](sqlite3* db){
        sqlite3_stmt* s=nullptr;
        const char* sql="SELECT msg_id,local_order,retry_count,payload_version FROM outbox "
            "WHERE owner_id=? AND next_retry_at<=? AND retry_count<2147483647 ORDER BY local_order LIMIT ?";
        if(sqlite3_prepare_v2(db,sql,-1,&s,nullptr)!=SQLITE_OK) return false;
        sqlite3_bind_int64(s,1,ownerId); sqlite3_bind_int64(s,2,now); sqlite3_bind_int(s,3,limit);
        int rc;
        while((rc=sqlite3_step(s))==SQLITE_ROW){
            const auto* p=sqlite3_column_text(s,0);
            OutboxAttempt a;
            a.msgId=p?std::string(reinterpret_cast<const char*>(p),sqlite3_column_bytes(s,0)):std::string();
            a.localOrder=sqlite3_column_int64(s,1); a.attempt=sqlite3_column_int64(s,2)+1;
            if(sqlite3_column_int(s,3)>1){sqlite3_finalize(s);return false;}
            attempts->push_back(std::move(a));
        }
        sqlite3_finalize(s); if(rc!=SQLITE_DONE) return false;
        for(auto& a:*attempts){
            im::dto::MessageDto m;
            if(!readMessage(db,ownerId,a.msgId,&m) || !m.fromMe) return false;
            a.packetType=im::proto::DEF_PROT_CHAT_INFO_RQ; a.payload=outgoingPayload(m);
            const char* update="UPDATE outbox SET retry_count=?,next_retry_at=?,payload_version=1,packet_type=?,payload=? "
                               "WHERE owner_id=? AND msg_id=?";
            if(sqlite3_prepare_v2(db,update,-1,&s,nullptr)!=SQLITE_OK) return false;
            sqlite3_bind_int64(s,1,a.attempt); sqlite3_bind_int64(s,2,now+lease); sqlite3_bind_int(s,3,a.packetType);
            sqlite3_bind_blob(s,4,a.payload.data(),static_cast<int>(a.payload.size()),SQLITE_TRANSIENT);
            sqlite3_bind_int64(s,5,ownerId); bindText(s,6,a.msgId);
            const bool updated=sqlite3_step(s)==SQLITE_DONE; sqlite3_finalize(s); if(!updated) return false;
        }
        return true;
    },err);
    if(ok) *out=std::move(*attempts);
    return ok;
}

bool NativeRepository::finishOutboxAttempt(std::int64_t ownerId,const std::string& msgId,
    std::int64_t attempt,bool terminal,std::int64_t retryAt,const std::string& error,std::string* err)
{
    if(ownerId<=0 || msgId.empty() || attempt<=0 || retryAt<0) return false;
    return runTx([=](sqlite3* db){
        sqlite3_stmt* s=nullptr;
        const char* sql="UPDATE outbox SET last_error=?1,next_retry_at=?2,finished_attempt=?5 WHERE owner_id=?3 AND msg_id=?4 "
            "AND retry_count=?5 AND finished_attempt<?5 AND next_retry_at<>9223372036854775807";
        if(sqlite3_prepare_v2(db,sql,-1,&s,nullptr)!=SQLITE_OK) return false;
        bindText(s,1,error.substr(0,1024)); sqlite3_bind_int64(s,2,terminal?std::numeric_limits<std::int64_t>::max():retryAt);
        sqlite3_bind_int64(s,3,ownerId); bindText(s,4,msgId); sqlite3_bind_int64(s,5,attempt);
        const bool ok=sqlite3_step(s)==SQLITE_DONE; sqlite3_finalize(s); return ok;
    },err);
}

bool NativeRepository::cancelOutbox(std::int64_t ownerId,const std::string& msgId,std::string* err)
{
    return runTx([=](sqlite3* db){
        sqlite3_stmt* s=nullptr;
        if(sqlite3_prepare_v2(db,"UPDATE outbox SET next_retry_at=9223372036854775807,last_error='cancelled' "
            "WHERE owner_id=? AND msg_id=?",-1,&s,nullptr)!=SQLITE_OK) return false;
        sqlite3_bind_int64(s,1,ownerId); bindText(s,2,msgId);
        const bool ok=sqlite3_step(s)==SQLITE_DONE; sqlite3_finalize(s);return ok;
    },err);
}

bool NativeRepository::commitIncomingMessage(const im::dto::MessageDto& m, bool* inserted,
                                             std::string* err, const IncomingContext& ctx)
{
    if (inserted) *inserted = false;
    if (m.ownerId <= 0 || m.msgId.empty() || m.conversationId <= 0 || m.peerId <= 0) {
        if (err) *err = "invalid incoming identity";
        return false;
    }
    auto msg = std::make_shared<im::dto::MessageDto>(m); // 值拷贝：不捕获调用方引用
    auto ins = std::make_shared<bool>(false);

    const bool ok = runTx([msg, ins, ctx](sqlite3* db) {
        return mergeIncoming(db, *msg, ins.get(), ctx);
    }, err);
    if (inserted) *inserted = ok && *ins;
    return ok;
}

bool NativeRepository::mergeIncoming(sqlite3* db, const im::dto::MessageDto& m,
                                      bool* inserted, const IncomingContext& ctx)
{
    if (m.ownerId<=0 || m.msgId.empty() || m.peerId<=0 || m.conversationId<=0) return false;
    const auto* msg = &m;
    auto* ins = inserted;
        if (!compatibleIdentity(db,*msg)) return false;
        sqlite3_stmt* s = nullptr;
        const char* sql =
            "INSERT INTO messages(owner_id,msg_id,conversation_id,peer_id,conversation_seq,"
            "server_time,local_order,from_me,type,content,status) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(owner_id,msg_id) DO NOTHING";
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK || !s) return false;
        sqlite3_bind_int64(s, 1, msg->ownerId);
        bindText(s, 2, msg->msgId);
        sqlite3_bind_int64(s, 3, msg->conversationId);
        sqlite3_bind_int64(s, 4, msg->peerId);
        sqlite3_bind_int64(s, 5, msg->seq);
        sqlite3_bind_int64(s, 6, msg->ts);
        sqlite3_bind_int64(s, 7, msg->localOrder);
        sqlite3_bind_int(s, 8, msg->fromMe ? 1 : 0);
        sqlite3_bind_int(s, 9, msg->type);
        bindText(s, 10, msg->content);
        sqlite3_bind_int(s, 11, msg->status);
        const bool stepped = sqlite3_step(s) == SQLITE_DONE;
        sqlite3_finalize(s);
        if (!stepped) return false;

        *ins = sqlite3_changes(db) > 0;
        if (!fillMissingMedia(db, *msg)) return false;

        const bool shouldCountUnread = !msg->fromMe && !ctx.isRoaming &&
                                       (ctx.activeConversationId != msg->conversationId);

        if (!*ins) {
            // 已存在：补齐既有记录缺失的 seq / server_time / status（幂等，不报错）。
            // 只补齐"从 0 到非 0"或"状态更前进"的方向，不回退既有值（F06）。
            sqlite3_stmt* up = nullptr;
            const char* ups =
                "UPDATE messages SET "
                "conversation_seq = CASE WHEN conversation_seq = 0 AND ?3 > 0 "
                "    THEN ?3 ELSE conversation_seq END, "
                "server_time = CASE WHEN server_time = 0 AND ?4 > 0 "
                "    THEN ?4 ELSE server_time END, "
                "status = CASE WHEN status=0 AND ?5 > 0 THEN ?5 ELSE status END "
                "WHERE owner_id = ?1 AND msg_id = ?2";
            if (sqlite3_prepare_v2(db, ups, -1, &up, nullptr) != SQLITE_OK || !up) return false;
            sqlite3_bind_int64(up, 1, msg->ownerId);
            bindText(up, 2, msg->msgId);
            sqlite3_bind_int64(up, 3, msg->seq);
            sqlite3_bind_int64(up, 4, msg->ts);
            sqlite3_bind_int(up, 5, msg->status);
            const bool okc = sqlite3_step(up) == SQLITE_DONE;
            sqlite3_finalize(up);
            if (!okc) return false;
            if (!finishProjection(db,*msg,false,ctx)) return false;
            if (msg->fromMe && msg->seq>0 && msg->status>0) {
                sqlite3_stmt* done=nullptr;
                if (sqlite3_prepare_v2(db,"DELETE FROM outbox WHERE owner_id=? AND msg_id=?",-1,&done,nullptr)!=SQLITE_OK) return false;
                sqlite3_bind_int64(done,1,msg->ownerId); bindText(done,2,msg->msgId);
                const bool removed=sqlite3_step(done)==SQLITE_DONE; sqlite3_finalize(done);
                if (!removed) return false;
            }

            // 重复消息同样要推进 maxseen（单调递增，不回退）
            return bumpMaxSeen(db, msg->ownerId, msg->peerId, msg->seq, msg->ts);
        }

        if (!upsertFts(db, *msg)) return false;

        // 会话 preview：仅当消息**更新**时才覆盖 last_message（避免历史消息倒挂）；
        // 未读仅在 非本人 && 非漫游 && 非当前前台会话 && seq > read_seq 时 +1。
        {
            sqlite3_stmt* st = nullptr;
            const char* cs =
                "INSERT INTO conversations(conversation_id,owner_id,peer_id,"
                "last_message,last_message_time,unread) VALUES(?,?,?,?,?,?) "
                "ON CONFLICT(conversation_id) DO UPDATE SET "
                "unread = conversations.unread + "
                "    (CASE WHEN ?7 = 1 AND ?8 > conversations.read_seq THEN 1 ELSE 0 END)";
            if (sqlite3_prepare_v2(db, cs, -1, &st, nullptr) != SQLITE_OK || !st) return false;
            sqlite3_bind_int64(st, 1, msg->conversationId);
            sqlite3_bind_int64(st, 2, msg->ownerId);
            sqlite3_bind_int64(st, 3, msg->peerId);
            bindText(st, 4, msg->content);
            sqlite3_bind_int64(st, 5, msg->ts);
            sqlite3_bind_int64(st, 6, shouldCountUnread ? 1 : 0); // 首次插入的未读基线
            sqlite3_bind_int(st, 7, shouldCountUnread ? 1 : 0);   // 冲突时是否累加
            sqlite3_bind_int64(st, 8, msg->seq);
            const bool ok2 = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
            if (!ok2) return false;
        }

        // 水位：maxseen 单调递增（先收 100 再收 90 不回退）
        if (!finishProjection(db,*msg,false,ctx)) return false;
        return bumpMaxSeen(db, msg->ownerId, msg->peerId, msg->seq, msg->ts);

}

bool NativeRepository::commitOutgoingDraft(const im::dto::MessageDto& m, std::string* err,
                                          std::int64_t* allocatedOrder)
{
    if (allocatedOrder) *allocatedOrder = 0;
    if (m.ownerId <= 0 || m.msgId.empty()) {
        if (err) *err = "invalid draft identity";
        return false;
    }
    auto msg = std::make_shared<im::dto::MessageDto>(m);
    const bool committed = runTx([msg](sqlite3* db) -> bool {
        if (!compatibleIdentity(db,*msg)) return false;
        sqlite3_stmt* existing = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT local_order FROM messages WHERE owner_id=? AND msg_id=?",
                               -1, &existing, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_int64(existing, 1, msg->ownerId);
        bindText(existing, 2, msg->msgId);
        const int found = sqlite3_step(existing);
        if (found == SQLITE_ROW) msg->localOrder = sqlite3_column_int64(existing, 0);
        sqlite3_finalize(existing);
        if (found == SQLITE_ROW) return true;
        if (found != SQLITE_DONE) return false;

        // Seed from both tables as importers may add rows after schema migration.
        sqlite3_stmt* alloc = nullptr;
        const char* allocate =
            "INSERT INTO local_sequence(owner_id,last_order) "
            "VALUES(?1,MAX(?2,MAX(COALESCE((SELECT MAX(local_order) FROM messages),0),"
            "COALESCE((SELECT MAX(local_order) FROM outbox),0))+1)) "
            "ON CONFLICT(owner_id) DO UPDATE SET "
            "last_order=MAX(local_sequence.last_order+1,excluded.last_order) "
            "WHERE local_sequence.last_order < 9223372036854775807";
        if (sqlite3_prepare_v2(db, allocate, -1, &alloc, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_int64(alloc, 1, msg->ownerId);
        sqlite3_bind_int64(alloc, 2, msg->localOrder);
        const bool allocated = sqlite3_step(alloc) == SQLITE_DONE && sqlite3_changes(db) == 1;
        sqlite3_finalize(alloc);
        if (!allocated) return false;
        if (sqlite3_prepare_v2(db, "SELECT last_order FROM local_sequence WHERE owner_id=?",
                               -1, &alloc, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_int64(alloc, 1, msg->ownerId);
        const bool gotOrder = sqlite3_step(alloc) == SQLITE_ROW;
        if (gotOrder) msg->localOrder = sqlite3_column_int64(alloc, 0);
        sqlite3_finalize(alloc);
        if (!gotOrder || msg->localOrder <= 0) return false;
        sqlite3_stmt* s = nullptr;
        const char* sql =
            "INSERT INTO messages(owner_id,msg_id,conversation_id,peer_id,conversation_seq,"
            "server_time,local_order,from_me,type,content,status) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(owner_id,msg_id) DO NOTHING";
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK || !s) return false;
        sqlite3_bind_int64(s, 1, msg->ownerId);
        bindText(s, 2, msg->msgId);
        sqlite3_bind_int64(s, 3, msg->conversationId);
        sqlite3_bind_int64(s, 4, msg->peerId);
        sqlite3_bind_int64(s, 5, msg->seq);
        sqlite3_bind_int64(s, 6, msg->ts);
        sqlite3_bind_int64(s, 7, msg->localOrder);
        sqlite3_bind_int(s, 8, msg->fromMe ? 1 : 0);
        sqlite3_bind_int(s, 9, msg->type);
        bindText(s, 10, msg->content);
        sqlite3_bind_int(s, 11, msg->status);
        const bool ok = sqlite3_step(s) == SQLITE_DONE;
        sqlite3_finalize(s);
        if (!ok) return false;

        // 是否首次插入：DO NOTHING 冲突时 changes=0（重复提交草稿）
        const bool firstInsert = sqlite3_changes(db) > 0;
        if (!fillMissingMedia(db, *msg)) return false;
        if (!firstInsert) {
            // 重复提交同 msg_id：幂等，不重复写 FTS / 摘要 / outbox（第 8 点缺陷）。
            // 重试是独立操作（retryMessage），不在此处重置 retry_count。
            return true;
        }

        // outbox：与数据库发号、草稿插入处于同一事务。
        {
            sqlite3_stmt* st = nullptr;
            const char* os = "INSERT INTO outbox(local_order,owner_id,msg_id,"
                             "packet_type,payload,retry_count,next_retry_at,last_error,payload_version) "
                             "VALUES(?,?,?,?,?,0,0,NULL,1)";
            if (sqlite3_prepare_v2(db, os, -1, &st, nullptr) != SQLITE_OK || !st) return false;
            sqlite3_bind_int64(st, 1, msg->localOrder);
            sqlite3_bind_int64(st, 2, msg->ownerId);
            bindText(st, 3, msg->msgId);
            sqlite3_bind_int(st, 4, im::proto::DEF_PROT_CHAT_INFO_RQ);
            const auto payload = outgoingPayload(*msg);
            sqlite3_bind_blob(st, 5, payload.data(), static_cast<int>(payload.size()), SQLITE_TRANSIENT);
            const bool ok2 = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
            if (!ok2) return false;
        }

        // FTS：发送中的消息也应可被搜索（否则刚发送的消息搜不到）
        if (!upsertFts(db, *msg)) return false;

        // 会话摘要：更新 last_message 让草稿出现在会话 preview；
        // 但草稿 ts=0（无 server_time），**不回退** last_message_time。
        {
            sqlite3_stmt* st = nullptr;
            const char* cs =
                "INSERT INTO conversations(conversation_id,owner_id,peer_id,"
                "last_message,last_message_time,unread) VALUES(?,?,?,?,?,0) "
                "ON CONFLICT(conversation_id) DO UPDATE SET "
                "last_message = excluded.last_message";
            if (sqlite3_prepare_v2(db, cs, -1, &st, nullptr) != SQLITE_OK || !st) return false;
            sqlite3_bind_int64(st, 1, msg->conversationId);
            sqlite3_bind_int64(st, 2, msg->ownerId);
            sqlite3_bind_int64(st, 3, msg->peerId);
            bindText(st, 4, msg->content);
            sqlite3_bind_int64(st, 5, msg->ts);
            const bool ok3 = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
            return ok3 && finishProjection(db,*msg,true);
        }
    }, err);
    if (committed && allocatedOrder) *allocatedOrder = msg->localOrder;
    return committed;
}

bool NativeRepository::commitAck(std::int64_t ownerId, const std::string& msgId,
                                 std::int64_t serverTime, std::int64_t conversationSeq,
                                 std::int32_t status, bool* updated, std::string* err)
{
    if (updated) *updated = false;
    // This API accepts storage delivery states, NOT protocol result codes.
    if (status < 0 || status > 3 || conversationSeq < 0) {
        if (err) *err = "invalid acknowledgement state";
        return false;
    }
    auto id = std::make_shared<std::string>(msgId);
    auto upd = std::make_shared<bool>(false);

    const bool ok = runTx([ownerId, id, serverTime, conversationSeq, status, upd](sqlite3* db) -> bool {
        sqlite3_stmt* q = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT conversation_seq FROM messages WHERE owner_id=? AND msg_id=?",
                              -1, &q, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_int64(q,1,ownerId); bindText(q,2,*id);
        const int found = sqlite3_step(q);
        const auto oldSeq = found == SQLITE_ROW ? sqlite3_column_int64(q,0) : 0;
        sqlite3_finalize(q);
        if (found == SQLITE_DONE) return true;
        if (found != SQLITE_ROW || (oldSeq>0 && conversationSeq>0 && oldSeq!=conversationSeq)) return false;
        // 状态机：只在 Sending(0) → 终态(>0) 时前进。
        // 迟到旧回执（status 已是终态）**不覆盖**；seq/server_time 只在 0→非0 时补齐（不回退）。
        // 注意：不用数值大小判断前进（存储语义 1=已送达/2=已接收/3=离线转存，无大小顺序）。
        sqlite3_stmt* s = nullptr;
        const char* sql =
            "UPDATE messages SET "
            "  conversation_seq = CASE WHEN conversation_seq = 0 AND ?3 > 0 "
            "      THEN ?3 ELSE conversation_seq END, "
            "  server_time = CASE WHEN server_time = 0 AND ?4 > 0 "
            "      THEN ?4 ELSE server_time END, "
            "  status = CASE WHEN status = 0 AND ?5 > 0 THEN ?5 ELSE status END "
            "WHERE owner_id = ?1 AND msg_id = ?2 AND "
            "((conversation_seq=0 AND ?3>0) OR (server_time=0 AND ?4>0) OR (status=0 AND ?5>0))";
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK || !s) return false;
        sqlite3_bind_int64(s, 1, ownerId);
        bindText(s, 2, *id);
        sqlite3_bind_int64(s, 3, conversationSeq);
        sqlite3_bind_int64(s, 4, serverTime);
        sqlite3_bind_int(s, 5, status);
        const bool ok = sqlite3_step(s) == SQLITE_DONE;
        sqlite3_finalize(s);
        if (!ok) return false;
        *upd = sqlite3_changes(db) > 0;

        // Outbox：仅在**终态**（status>0，即成功/离线转存等已处理结果）删除；
        // 可重试失败只登记错误。attempt 应由实际发送领取推进，不能按回调数累计。
        if (status > 0) {
            sqlite3_stmt* preview=nullptr;
            if (sqlite3_prepare_v2(db,"UPDATE conversations SET last_message_time=MAX(last_message_time,?3) "
                                     "WHERE owner_id=?1 AND last_msg_id=?2",-1,&preview,nullptr)!=SQLITE_OK) return false;
            sqlite3_bind_int64(preview,1,ownerId); bindText(preview,2,*id); sqlite3_bind_int64(preview,3,serverTime);
            const bool projected=sqlite3_step(preview)==SQLITE_DONE; sqlite3_finalize(preview);
            if (!projected) return false;
            sqlite3_stmt* st = nullptr;
            const char* ds = "DELETE FROM outbox WHERE owner_id=? AND msg_id=?";
            if (sqlite3_prepare_v2(db, ds, -1, &st, nullptr) != SQLITE_OK || !st) return false;
            sqlite3_bind_int64(st, 1, ownerId);
            bindText(st, 2, *id);
            const bool ok2 = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
            return ok2;
        } else {
            sqlite3_stmt* st = nullptr;
            const char* rs = "UPDATE outbox SET last_error = 'retryable' "
                             "WHERE owner_id=? AND msg_id=?";
            if (sqlite3_prepare_v2(db, rs, -1, &st, nullptr) != SQLITE_OK || !st) return false;
            sqlite3_bind_int64(st, 1, ownerId);
            bindText(st, 2, *id);
            const bool ok2 = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
            return ok2;
        }
    }, err);

    if (updated) *updated = ok && *upd;
    return ok;
}

bool NativeRepository::markConversationRead(std::int64_t ownerId, std::int64_t conversationId,
                                            std::int64_t readSeq, std::string* err)
{
    return runTx([ownerId, conversationId, readSeq](sqlite3* db) -> bool {
        // 先读旧 read_seq，计算**有效水位** = MAX(旧, 本次请求)。
        // 关键：read_seq 与 unread 重算必须用同一个有效水位，否则"迟到旧已读请求"
        // 会让 read_seq 保持较大、但把 91..100 重新算成未读（第 4 点缺陷）。
        std::int64_t oldRead = 0;
        {
            sqlite3_stmt* q = nullptr;
            const char* qs =
                "SELECT read_seq FROM conversations WHERE owner_id=?1 AND conversation_id=?2";
            if (sqlite3_prepare_v2(db, qs, -1, &q, nullptr) != SQLITE_OK || !q) return false;
            sqlite3_bind_int64(q, 1, ownerId);
            sqlite3_bind_int64(q, 2, conversationId);
            if (sqlite3_step(q) == SQLITE_ROW) oldRead = sqlite3_column_int64(q, 0);
            sqlite3_finalize(q);
        }
        const std::int64_t effective = readSeq > oldRead ? readSeq : oldRead;

        sqlite3_stmt* s = nullptr;
        const char* sql =
            "UPDATE conversations SET "
            "  read_seq = ?1, "
            "  unread = (SELECT count(*) FROM messages "
            "            WHERE owner_id = ?2 AND conversation_id = ?3 "
            "              AND from_me = 0 AND conversation_seq > ?4) "
            "WHERE owner_id = ?5 AND conversation_id = ?6";
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK || !s) return false;
        sqlite3_bind_int64(s, 1, effective);
        sqlite3_bind_int64(s, 2, ownerId);
        sqlite3_bind_int64(s, 3, conversationId);
        sqlite3_bind_int64(s, 4, effective);
        sqlite3_bind_int64(s, 5, ownerId);
        sqlite3_bind_int64(s, 6, conversationId);
        const bool ok = sqlite3_step(s) == SQLITE_DONE;
        sqlite3_finalize(s);
        if (!ok) return false;

        // 已读水位（供多设备同步的增强项未来复用；本轮仅本地），同样单调递增
        sqlite3_stmt* st = nullptr;
        const char* ws =
            "INSERT INTO sync_watermarks(owner_id,domain,peer_id,synced_seq,updated_at) "
            "VALUES(?,'read',?,?,0) "
            "ON CONFLICT(owner_id,domain,peer_id) DO UPDATE SET "
            "synced_seq = MAX(sync_watermarks.synced_seq, excluded.synced_seq)";
        if (sqlite3_prepare_v2(db, ws, -1, &st, nullptr) != SQLITE_OK || !st) return false;
        sqlite3_bind_int64(st, 1, ownerId);
        sqlite3_bind_int64(st, 2, conversationId);
        sqlite3_bind_int64(st, 3, effective);
        const bool ok2 = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        return ok2;
    }, err);
}

bool NativeRepository::upsertMediaAndMessage(const std::vector<im::dto::MediaTaskDto>& variants,
                                             const im::dto::MessageDto& m, std::string* err)
{
    auto vs = std::make_shared<std::vector<im::dto::MediaTaskDto>>(variants);
    auto msg = std::make_shared<im::dto::MessageDto>(m);

    return runTx([vs, msg](sqlite3* db) -> bool {
        if (!compatibleIdentity(db,*msg)) return false;
        bool inserted = false;
        if (!mergeIncoming(db, *msg, &inserted, IncomingContext{})) return false;

        for (const auto& t : *vs) {
            sqlite3_stmt* s = nullptr;
            const char* sql = "INSERT INTO media_variants"
                              "(media_id,variant,real_mime,width,height,file_size,file_hash,"
                              "local_path,state) VALUES(?,?,?,?,?,?,?,?,?) "
                              "ON CONFLICT(media_id,variant) DO UPDATE SET "
                              "real_mime=excluded.real_mime,width=excluded.width,height=excluded.height,"
                              "file_size=excluded.file_size,file_hash=excluded.file_hash,"
                              "local_path=excluded.local_path,state=excluded.state "
                              "WHERE media_variants.state<>1 AND excluded.state=1";
            if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK || !s) return false;
            bindText(s, 1, t.fileHash); // media_id 以内容 hash 为键（跨账号同 hash 由上层授权控制）
            sqlite3_bind_int(s, 2, static_cast<int>(t.variant));
            // 真实元数据：由 DTO 提供，禁止用路径后缀或占位值（JPEG 字节标 AVIF 会错）
            bindText(s, 3, t.realMime);
            sqlite3_bind_int(s, 4, t.width);
            sqlite3_bind_int(s, 5, t.height);
            sqlite3_bind_int64(s, 6, t.bytesTotal);
            bindText(s, 7, t.fileHash);
            bindText(s, 8, t.filePath);
            sqlite3_bind_int(s, 9, t.state == im::dto::TaskState::Success ? 1 : 0);
            const bool ok = sqlite3_step(s) == SQLITE_DONE;
            sqlite3_finalize(s);
            if (!ok) return false;

            // 引用关系（引用计数由 media_refs 行数体现，物理删文件在 commit 后）
            sqlite3_stmt* st = nullptr;
            const char* rs = "INSERT OR REPLACE INTO media_refs(owner_id,msg_id,media_id,variant) "
                             "VALUES(?,?,?,?)";
            if (sqlite3_prepare_v2(db, rs, -1, &st, nullptr) != SQLITE_OK || !st) return false;
            sqlite3_bind_int64(st, 1, msg->ownerId);
            bindText(st, 2, msg->msgId);
            bindText(st, 3, t.fileHash);
            sqlite3_bind_int(st, 4, static_cast<int>(t.variant));
            const bool ok2 = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
            if (!ok2) return false;
        }
        return true;
    }, err);
}

// ---------------- 好友域（P7-G4） ----------------

bool NativeRepository::upsertFriend(std::int64_t ownerId, const im::dto::FriendDto& f,
                                    std::string* err)
{
    if (ownerId <= 0 || f.friendId <= 0) {
        if (err) *err = "invalid friend identity";
        return false;
    }
    auto prof = std::make_shared<im::dto::FriendDto>(f);
    return runTx([ownerId, prof](sqlite3* db) -> bool {
        sqlite3_stmt* s = nullptr;
        const char* sql =
            "INSERT INTO friends(owner_id,friend_id,nick,tel,avatar,signature,sex,ts) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,0) "
            "ON CONFLICT(owner_id,friend_id) DO UPDATE SET "
            "  nick=excluded.nick, tel=excluded.tel, avatar=excluded.avatar, "
            "  signature=excluded.signature, sex=excluded.sex";
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK || !s) return false;
        sqlite3_bind_int64(s, 1, ownerId);
        sqlite3_bind_int64(s, 2, prof->friendId);
        bindText(s, 3, prof->nick);
        bindText(s, 4, prof->tel);
        bindText(s, 5, prof->avatar);
        bindText(s, 6, prof->signature);
        sqlite3_bind_int(s, 7, prof->sex);
        const bool ok = sqlite3_step(s) == SQLITE_DONE;
        sqlite3_finalize(s);
        return ok;
    }, err);
}

bool NativeRepository::deleteFriend(std::int64_t ownerId, std::int64_t friendId, bool* deleted,
                                    std::string* err)
{
    if (deleted) *deleted = false;
    if (ownerId <= 0 || friendId <= 0) {
        if (err) *err = "invalid friend identity";
        return false;
    }
    auto del = std::make_shared<bool>(false);
    const bool ok = runTx([ownerId, friendId, del](sqlite3* db) -> bool {
        sqlite3_stmt* s = nullptr;
        const char* sql = "DELETE FROM friends WHERE owner_id=?1 AND friend_id=?2";
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK || !s) return false;
        sqlite3_bind_int64(s, 1, ownerId);
        sqlite3_bind_int64(s, 2, friendId);
        const bool stepped = sqlite3_step(s) == SQLITE_DONE;
        *del = sqlite3_changes(db) > 0;
        sqlite3_finalize(s);
        return stepped;
    }, err);
    if (deleted) *deleted = ok && *del;
    return ok;
}

bool NativeRepository::upsertFriendRequest(std::int64_t ownerId,
                                           const im::dto::FriendRequestDto& r,
                                           bool* inserted, std::string* err)
{
    if (inserted) *inserted = false;
    if (ownerId <= 0 || r.requestId.empty()) {
        if (err) *err = "invalid friend request identity";
        return false;
    }
    auto req = std::make_shared<im::dto::FriendRequestDto>(r);
    auto ins = std::make_shared<bool>(false);
    const bool ok = runTx([ownerId, req, ins](sqlite3* db) -> bool {
        sqlite3_stmt* s = nullptr;
        // 1) 插入（冲突则跳过）：DO NOTHING 使 sqlite3_changes 能区分首次/重复
        const char* insertSql =
            "INSERT INTO friend_requests(request_id,owner_id,from_user_id,to_user_id,"
            "direction,state,message,server_version,updated_at,created_at) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,0,?8,?8) "
            "ON CONFLICT(owner_id,request_id) DO NOTHING";
        if (sqlite3_prepare_v2(db, insertSql, -1, &s, nullptr) != SQLITE_OK || !s) return false;
        bindText(s, 1, req->requestId);
        sqlite3_bind_int64(s, 2, ownerId);
        sqlite3_bind_int64(s, 3, req->fromUserId);
        sqlite3_bind_int64(s, 4, req->toUserId);
        sqlite3_bind_int(s, 5, static_cast<int>(req->direction));
        sqlite3_bind_int(s, 6, static_cast<int>(req->state));
        bindText(s, 7, req->message);
        sqlite3_bind_int64(s, 8, req->createdAt);
        const bool insertedNow = sqlite3_step(s) == SQLITE_DONE;
        *ins = sqlite3_changes(db) > 0;
        sqlite3_finalize(s);
        if (!insertedNow) return false;

        // 2) 已存在则补齐 state/message/updated_at（幂等；created_at 保持首次值）
        if (!*ins) {
            sqlite3_stmt* u = nullptr;
            const char* updateSql =
                "UPDATE friend_requests SET state=?1, message=?2, updated_at=?3 "
                "WHERE owner_id=?4 AND request_id=?5";
            if (sqlite3_prepare_v2(db, updateSql, -1, &u, nullptr) != SQLITE_OK || !u) return false;
            sqlite3_bind_int(u, 1, static_cast<int>(req->state));
            bindText(u, 2, req->message);
            sqlite3_bind_int64(u, 3, req->createdAt);
            sqlite3_bind_int64(u, 4, ownerId);
            bindText(u, 5, req->requestId);
            const bool updated = sqlite3_step(u) == SQLITE_DONE;
            sqlite3_finalize(u);
            return updated;
        }
        return true;
    }, err);
    if (inserted) *inserted = ok && *ins;
    return ok;
}

bool NativeRepository::setFriendRequestState(std::int64_t ownerId, const std::string& requestId,
                                             im::dto::RequestState state, std::string* err)
{
    if (ownerId <= 0 || requestId.empty()) {
        if (err) *err = "invalid friend request identity";
        return false;
    }
    return runTx([ownerId, requestId, state](sqlite3* db) -> bool {
        sqlite3_stmt* s = nullptr;
        const char* sql = "UPDATE friend_requests SET state=?1 WHERE owner_id=?2 AND request_id=?3";
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK || !s) return false;
        sqlite3_bind_int(s, 1, static_cast<int>(state));
        sqlite3_bind_int64(s, 2, ownerId);
        bindText(s, 3, requestId);
        const bool ok = sqlite3_step(s) == SQLITE_DONE;
        sqlite3_finalize(s);
        return ok;
    }, err);
}

bool NativeRepository::acceptFriendRequest(std::int64_t ownerId, const std::string& requestId,
                                           const im::dto::FriendDto& accepted, std::string* err)
{
    if (ownerId <= 0 || requestId.empty() || accepted.friendId <= 0) {
        if (err) *err = "invalid accept request";
        return false;
    }
    auto prof = std::make_shared<im::dto::FriendDto>(accepted);
    return runTx([ownerId, requestId, prof](sqlite3* db) -> bool {
        // 1) 仅当申请存在且为 pending 时，才置为 Accepted（幂等：已处理/不存在不重复插好友）
        sqlite3_stmt* s = nullptr;
        const char* sql =
            "UPDATE friend_requests SET state=?1 WHERE owner_id=?2 AND request_id=?3 AND state=0";
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK || !s) return false;
        sqlite3_bind_int(s, 1, static_cast<int>(im::dto::RequestState::Accepted));
        sqlite3_bind_int64(s, 2, ownerId);
        bindText(s, 3, requestId);
        const bool stepped = sqlite3_step(s) == SQLITE_DONE;
        const bool changed = sqlite3_changes(db) > 0;
        sqlite3_finalize(s);
        if (!stepped) return false;
        if (!changed) return true;

        // 2) 同事务插入/更新好友资料
        sqlite3_stmt* f = nullptr;
        const char* fs =
            "INSERT INTO friends(owner_id,friend_id,nick,tel,avatar,signature,sex,ts) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,0) "
            "ON CONFLICT(owner_id,friend_id) DO UPDATE SET "
            "  nick=excluded.nick, tel=excluded.tel, avatar=excluded.avatar, "
            "  signature=excluded.signature, sex=excluded.sex";
        if (sqlite3_prepare_v2(db, fs, -1, &f, nullptr) != SQLITE_OK || !f) return false;
        sqlite3_bind_int64(f, 1, ownerId);
        sqlite3_bind_int64(f, 2, prof->friendId);
        bindText(f, 3, prof->nick);
        bindText(f, 4, prof->tel);
        bindText(f, 5, prof->avatar);
        bindText(f, 6, prof->signature);
        sqlite3_bind_int(f, 7, prof->sex);
        const bool ok = sqlite3_step(f) == SQLITE_DONE;
        sqlite3_finalize(f);
        return ok;
    }, err);
}

bool NativeRepository::loadFriends(std::int64_t ownerId, std::vector<im::dto::FriendDto>* out,
                                   std::string* err)
{
    if (out) out->clear();
    if (!m_db || !out || ownerId <= 0) {
        if (err) *err = "invalid load friends";
        return false;
    }
    bool queryOk = false;
    const ReadResult result = m_db->withRead([&](sqlite3* db) {
        sqlite3_stmt* s = nullptr;
        const char* sql =
            "SELECT friend_id,nick,tel,avatar,signature,sex FROM friends "
            "WHERE owner_id=?1 ORDER BY friend_id";
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) return;
        sqlite3_bind_int64(s, 1, ownerId);
        int rc = SQLITE_OK;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            auto text = [&](int i) { const auto* p = sqlite3_column_text(s, i); return p ?
                std::string(reinterpret_cast<const char*>(p), sqlite3_column_bytes(s, i)) : std::string(); };
            im::dto::FriendDto f;
            f.friendId = sqlite3_column_int64(s, 0);
            f.nick = text(1);
            f.tel = text(2);
            f.avatar = text(3);
            f.signature = text(4);
            f.sex = sqlite3_column_int(s, 5);
            out->push_back(std::move(f));
        }
        queryOk = rc == SQLITE_DONE;
        sqlite3_finalize(s);
    });
    if (result != ReadResult::Ok || !queryOk) {
        out->clear();
        if (err) *err = "friends database unavailable";
        return false;
    }
    return true;
}

bool NativeRepository::loadFriendRequests(std::int64_t ownerId,
                                          std::vector<im::dto::FriendRequestDto>* out,
                                          std::string* err)
{
    if (out) out->clear();
    if (!m_db || !out || ownerId <= 0) {
        if (err) *err = "invalid load friend requests";
        return false;
    }
    bool queryOk = false;
    const ReadResult result = m_db->withRead([&](sqlite3* db) {
        sqlite3_stmt* s = nullptr;
        const char* sql =
            "SELECT request_id,from_user_id,to_user_id,direction,state,message,created_at "
            "FROM friend_requests WHERE owner_id=?1 ORDER BY created_at DESC, request_id";
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) return;
        sqlite3_bind_int64(s, 1, ownerId);
        int rc = SQLITE_OK;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            auto text = [&](int i) { const auto* p = sqlite3_column_text(s, i); return p ?
                std::string(reinterpret_cast<const char*>(p), sqlite3_column_bytes(s, i)) : std::string(); };
            im::dto::FriendRequestDto r;
            r.requestId = text(0);
            r.fromUserId = sqlite3_column_int64(s, 1);
            r.toUserId = sqlite3_column_int64(s, 2);
            r.direction = static_cast<im::dto::RequestDirection>(sqlite3_column_int(s, 3));
            r.state = static_cast<im::dto::RequestState>(sqlite3_column_int(s, 4));
            r.message = text(5);
            r.createdAt = sqlite3_column_int64(s, 6);
            out->push_back(std::move(r));
        }
        queryOk = rc == SQLITE_DONE;
        sqlite3_finalize(s);
    });
    if (result != ReadResult::Ok || !queryOk) {
        out->clear();
        if (err) *err = "friend requests database unavailable";
        return false;
    }
    return true;
}

// ---------------- AI 候选回复（P7-G4） ----------------

bool NativeRepository::upsertAiSuggestion(std::int64_t ownerId,
                                          const im::dto::AiSuggestionDto& s,
                                          std::string* err)
{
    if (ownerId <= 0 || s.requestId.empty()) {
        if (err) *err = "invalid ai suggestion identity";
        return false;
    }
    auto sug = std::make_shared<im::dto::AiSuggestionDto>(s);
    auto joined = std::make_shared<std::string>(joinSuggestions(s.suggestions));
    return runTx([ownerId, sug, joined](sqlite3* db) -> bool {
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "INSERT INTO ai_suggestions(request_id,owner_id,conversation_id,peer_id,tone,"
            "status,suggestions,error_code,generated_at,context_version) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10) "
            "ON CONFLICT(owner_id,request_id) DO UPDATE SET "
            "  status=excluded.status, suggestions=excluded.suggestions, "
            "  error_code=excluded.error_code, generated_at=excluded.generated_at, "
            "  context_version=excluded.context_version";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK || !st) return false;
        bindText(st, 1, sug->requestId);
        sqlite3_bind_int64(st, 2, ownerId);
        sqlite3_bind_int64(st, 3, sug->conversationId);
        sqlite3_bind_int64(st, 4, sug->peerId);
        bindText(st, 5, sug->tone);
        sqlite3_bind_int(st, 6, static_cast<int>(sug->status));
        bindText(st, 7, *joined);
        sqlite3_bind_int(st, 8, sug->errorCode);
        sqlite3_bind_int64(st, 9, sug->generatedAt);
        bindText(st, 10, sug->contextVersion);
        const bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        return ok;
    }, err);
}

bool NativeRepository::loadAiSuggestions(std::int64_t ownerId, std::int64_t conversationId,
                                         std::vector<im::dto::AiSuggestionDto>* out,
                                         std::string* err)
{
    if (out) out->clear();
    if (!m_db || !out || ownerId <= 0) {
        if (err) *err = "invalid load ai suggestions";
        return false;
    }
    bool queryOk = false;
    const ReadResult result = m_db->withRead([&](sqlite3* db) {
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "SELECT request_id,conversation_id,peer_id,tone,status,suggestions,error_code,"
            "generated_at,context_version FROM ai_suggestions "
            "WHERE owner_id=?1 AND conversation_id=?2 ORDER BY generated_at, request_id";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return;
        sqlite3_bind_int64(st, 1, ownerId);
        sqlite3_bind_int64(st, 2, conversationId);
        int rc = SQLITE_OK;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            auto text = [&](int i) { const auto* p = sqlite3_column_text(st, i); return p ?
                std::string(reinterpret_cast<const char*>(p), sqlite3_column_bytes(st, i)) : std::string(); };
            im::dto::AiSuggestionDto s;
            s.requestId = text(0);
            s.conversationId = sqlite3_column_int64(st, 1);
            s.peerId = sqlite3_column_int64(st, 2);
            s.tone = text(3);
            s.status = static_cast<im::dto::AiStatus>(sqlite3_column_int(st, 4));
            s.suggestions = splitSuggestions(text(5));
            s.errorCode = sqlite3_column_int(st, 6);
            s.generatedAt = sqlite3_column_int64(st, 7);
            s.contextVersion = text(8);
            out->push_back(std::move(s));
        }
        queryOk = rc == SQLITE_DONE;
        sqlite3_finalize(st);
    });
    if (result != ReadResult::Ok || !queryOk) {
        out->clear();
        if (err) *err = "ai suggestions database unavailable";
        return false;
    }
    return true;
}

// ---------------- 同步缺洞（P7-G4） ----------------

bool NativeRepository::replaceSyncGaps(std::int64_t ownerId, std::int64_t conversationId,
                                       const std::vector<SyncGapRow>& gaps, std::string* err)
{
    if (ownerId <= 0 || conversationId <= 0) {
        if (err) *err = "invalid sync gap identity";
        return false;
    }
    auto gs = std::make_shared<std::vector<SyncGapRow>>(gaps);
    return runTx([ownerId, conversationId, gs](sqlite3* db) -> bool {
        sqlite3_stmt* d = nullptr;
        const char* del = "DELETE FROM sync_gaps WHERE owner_id=?1 AND conversation_id=?2";
        if (sqlite3_prepare_v2(db, del, -1, &d, nullptr) != SQLITE_OK || !d) return false;
        sqlite3_bind_int64(d, 1, ownerId);
        sqlite3_bind_int64(d, 2, conversationId);
        const bool delOk = sqlite3_step(d) == SQLITE_DONE;
        sqlite3_finalize(d);
        if (!delOk) return false;

        for (const auto& g : *gs) {
            sqlite3_stmt* s = nullptr;
            const char* ins =
                "INSERT INTO sync_gaps(owner_id,conversation_id,gap_from,gap_to,attempt,next_retry_at,updated_at) "
                "VALUES(?1,?2,?3,?4,?5,?6,0)";
            if (sqlite3_prepare_v2(db, ins, -1, &s, nullptr) != SQLITE_OK || !s) return false;
            sqlite3_bind_int64(s, 1, ownerId);
            sqlite3_bind_int64(s, 2, conversationId);
            sqlite3_bind_int64(s, 3, g.gapFrom);
            sqlite3_bind_int64(s, 4, g.gapTo);
            sqlite3_bind_int(s, 5, g.attempt);
            sqlite3_bind_int64(s, 6, g.nextRetryAtMs);
            const bool ok = sqlite3_step(s) == SQLITE_DONE;
            sqlite3_finalize(s);
            if (!ok) return false;
        }
        return true;
    }, err);
}

bool NativeRepository::loadSyncGaps(std::int64_t ownerId, std::int64_t conversationId,
                                    std::vector<SyncGapRow>* out, std::string* err)
{
    if (out) out->clear();
    if (!m_db || !out || ownerId <= 0 || conversationId <= 0) {
        if (err) *err = "invalid load sync gaps";
        return false;
    }
    bool queryOk = false;
    const ReadResult result = m_db->withRead([&](sqlite3* db) {
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "SELECT gap_from,gap_to,attempt,next_retry_at FROM sync_gaps "
            "WHERE owner_id=?1 AND conversation_id=?2 ORDER BY gap_from";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return;
        sqlite3_bind_int64(st, 1, ownerId);
        sqlite3_bind_int64(st, 2, conversationId);
        int rc = SQLITE_OK;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            SyncGapRow g;
            g.gapFrom = sqlite3_column_int64(st, 0);
            g.gapTo = sqlite3_column_int64(st, 1);
            g.attempt = sqlite3_column_int(st, 2);
            g.nextRetryAtMs = sqlite3_column_int64(st, 3);
            out->push_back(g);
        }
        queryOk = rc == SQLITE_DONE;
        sqlite3_finalize(st);
    });
    if (result != ReadResult::Ok || !queryOk) {
        out->clear();
        if (err) *err = "sync gaps database unavailable";
        return false;
    }
    return true;
}

bool NativeRepository::loadMessageSeqRanges(std::int64_t ownerId,std::int64_t conversationId,
    std::vector<MessageSeqRange>* out,std::string* err)
{
    if(out)out->clear();
    if(!m_db||!out||ownerId<=0||conversationId<=0){if(err)*err="invalid seq range query";return false;}
    bool ok=false;
    const auto result=m_db->withRead([&](sqlite3* db){
        sqlite3_stmt* st=nullptr;
        const char* sql="SELECT DISTINCT conversation_seq FROM messages "
            "WHERE owner_id=?1 AND conversation_id=?2 AND conversation_seq>0 ORDER BY conversation_seq";
        if(sqlite3_prepare_v2(db,sql,-1,&st,nullptr)!=SQLITE_OK)return;
        sqlite3_bind_int64(st,1,ownerId);sqlite3_bind_int64(st,2,conversationId);
        int rc=SQLITE_OK;
        while((rc=sqlite3_step(st))==SQLITE_ROW){
            const auto seq=sqlite3_column_int64(st,0);
            if(out->empty()||(seq>out->back().to&&seq-out->back().to>1))out->push_back({seq,seq});
            else if(seq>out->back().to)out->back().to=seq;
        }
        ok=rc==SQLITE_DONE;sqlite3_finalize(st);
    });
    if(result!=ReadResult::Ok||!ok){out->clear();if(err)*err="seq range database unavailable";return false;}
    return true;
}

// ---------------- 分片上传草稿（P7 媒体断点续传） ----------------

namespace {

std::string joinChunkIndices(const std::vector<std::int32_t>& idx)
{
    std::string out;
    for (std::int32_t i : idx) {
        if (!out.empty()) out += ',';
        out += std::to_string(i);
    }
    return out;
}

std::vector<std::int32_t> splitChunkIndices(const std::string& s)
{
    std::vector<std::int32_t> out;
    std::size_t start = 0;
    while (start < s.size()) {
        const std::size_t comma = s.find(',', start);
        const std::string part = s.substr(start, comma == std::string::npos ? comma : comma - start);
        if (!part.empty()) {
            try { out.push_back(std::stoi(part)); } catch (...) { return {}; }
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

} // namespace

bool NativeRepository::upsertUploadDraft(std::int64_t ownerId,
                                         const im::dto::UploadDraftDto& d, bool* inserted,
                                         std::string* err)
{
    if (inserted) *inserted = false;
    if (ownerId <= 0 || d.msgId.empty() || d.conversationId <= 0 || d.peerId <= 0 ||
        d.localPath.empty() || d.fileSize <= 0 || d.sha256.size() != 64 ||
        d.imageWidth < 0 || d.imageHeight < 0 ||
        d.imageWidth > 100000 || d.imageHeight > 100000) {
        if (err) *err = "invalid upload draft";
        return false;
    }
    auto draft = std::make_shared<im::dto::UploadDraftDto>(d);
    auto ins = std::make_shared<bool>(false);
    const bool ok = runTx([ownerId, draft, ins](sqlite3* db) -> bool {
        sqlite3_stmt* s = nullptr;
        // 幂等：已存在则保留进度（upload_id/chunks_done/file_id/state 不重置）
        const char* sql =
            "INSERT INTO upload_drafts(owner_id,msg_id,variant,conversation_id,peer_id,local_path,"
            "file_name,file_size,sha256,content_type,upload_id,chunk_size,chunk_count,chunks_done,"
            "file_id,state,error_code,created_at,updated_at,image_width,image_height) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,'',0,0,'','',?11,?12,?13,?13,?14,?15) "
            "ON CONFLICT(owner_id,msg_id,variant) DO NOTHING";
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK || !s) return false;
        sqlite3_bind_int64(s, 1, ownerId);
        bindText(s, 2, draft->msgId);
        sqlite3_bind_int(s, 3, static_cast<int>(draft->variant));
        sqlite3_bind_int64(s, 4, draft->conversationId);
        sqlite3_bind_int64(s, 5, draft->peerId);
        bindText(s, 6, draft->localPath);
        bindText(s, 7, draft->fileName);
        sqlite3_bind_int64(s, 8, draft->fileSize);
        bindText(s, 9, draft->sha256);
        bindText(s, 10, draft->contentType);
        sqlite3_bind_int(s, 11, static_cast<int>(im::dto::UploadDraftState::Pending));
        sqlite3_bind_int(s, 12, 0);
        sqlite3_bind_int64(s, 13, draft->createdAt > 0 ? draft->createdAt
                                                       : static_cast<std::int64_t>(std::time(nullptr)));
        sqlite3_bind_int(s, 14, draft->imageWidth);
        sqlite3_bind_int(s, 15, draft->imageHeight);
        const bool stepped = sqlite3_step(s) == SQLITE_DONE;
        *ins = sqlite3_changes(db) > 0;
        sqlite3_finalize(s);
        return stepped;
    }, err);
    if (inserted) *inserted = ok && *ins;
    return ok;
}

bool NativeRepository::setUploadDraftSession(std::int64_t ownerId, const std::string& msgId,
                                             im::dto::MediaVariant variant,
                                             const std::string& uploadId,
                                             std::int64_t chunkSize, std::int32_t chunkCount,
                                             std::string* err)
{
    if (ownerId <= 0 || msgId.empty() || uploadId.empty() || chunkSize <= 0 || chunkCount <= 0) {
        if (err) *err = "invalid draft session";
        return false;
    }
    return runTx([=, this](sqlite3* db) -> bool {
        sqlite3_stmt* s = nullptr;
        // 只允许 Pending/Uploading/Failed → Uploading；终态与 Finalized 不可回退
        const char* sql =
            "UPDATE upload_drafts SET upload_id=?4,chunk_size=?5,chunk_count=?6,"
            "state=?7,error_code=0,updated_at=?8 "
            "WHERE owner_id=?1 AND msg_id=?2 AND variant=?3 AND state IN(0,1,5)";
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK || !s) return false;
        sqlite3_bind_int64(s, 1, ownerId);
        bindText(s, 2, msgId);
        sqlite3_bind_int(s, 3, static_cast<int>(variant));
        bindText(s, 4, uploadId);
        sqlite3_bind_int64(s, 5, chunkSize);
        sqlite3_bind_int(s, 6, chunkCount);
        sqlite3_bind_int(s, 7, static_cast<int>(im::dto::UploadDraftState::Uploading));
        sqlite3_bind_int64(s, 8, static_cast<std::int64_t>(std::time(nullptr)));
        // 0 行命中 = 状态不允许（终态/Finalized 回退），必须返回失败而非静默成功
        const bool ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1;
        sqlite3_finalize(s);
        return ok;
    }, err);
}

bool NativeRepository::markUploadChunkDone(std::int64_t ownerId, const std::string& msgId,
                                           im::dto::MediaVariant variant,
                                           std::int32_t chunkIndex, std::string* err)
{
    if (ownerId <= 0 || msgId.empty() || chunkIndex < 0) {
        if (err) *err = "invalid chunk mark";
        return false;
    }
    return runTx([=, this](sqlite3* db) -> bool {
        // 读-改-写在同一写事务：重复分片号幂等
        sqlite3_stmt* s = nullptr;
        const char* q = "SELECT chunks_done,chunk_count,state FROM upload_drafts "
                        "WHERE owner_id=?1 AND msg_id=?2 AND variant=?3";
        if (sqlite3_prepare_v2(db, q, -1, &s, nullptr) != SQLITE_OK || !s) return false;
        sqlite3_bind_int64(s, 1, ownerId);
        bindText(s, 2, msgId);
        sqlite3_bind_int(s, 3, static_cast<int>(variant));
        if (sqlite3_step(s) != SQLITE_ROW) { sqlite3_finalize(s); return false; }
        const unsigned char* p = sqlite3_column_text(s, 0);
        auto done = splitChunkIndices(p ? reinterpret_cast<const char*>(p) : "");
        const int count = sqlite3_column_int(s, 1);
        const int state = sqlite3_column_int(s, 2);
        sqlite3_finalize(s);
        if (chunkIndex >= count) return false;
        if (state == static_cast<int>(im::dto::UploadDraftState::Sent) ||
            state == static_cast<int>(im::dto::UploadDraftState::Cancelled)) return false;
        if (std::find(done.begin(), done.end(), chunkIndex) == done.end()) {
            done.push_back(chunkIndex);
            std::sort(done.begin(), done.end());
        }
        const std::string joined = joinChunkIndices(done);
        const char* u = "UPDATE upload_drafts SET chunks_done=?4,updated_at=?5 "
                        "WHERE owner_id=?1 AND msg_id=?2 AND variant=?3";
        if (sqlite3_prepare_v2(db, u, -1, &s, nullptr) != SQLITE_OK || !s) return false;
        sqlite3_bind_int64(s, 1, ownerId);
        bindText(s, 2, msgId);
        sqlite3_bind_int(s, 3, static_cast<int>(variant));
        bindText(s, 4, joined);
        sqlite3_bind_int64(s, 5, static_cast<std::int64_t>(std::time(nullptr)));
        const bool ok = sqlite3_step(s) == SQLITE_DONE;
        sqlite3_finalize(s);
        return ok;
    }, err);
}

bool NativeRepository::setUploadDraftFileId(std::int64_t ownerId, const std::string& msgId,
                                            im::dto::MediaVariant variant,
                                            const std::string& fileId, std::string* err)
{
    if (ownerId <= 0 || msgId.empty() || fileId.empty()) {
        if (err) *err = "invalid draft fileId";
        return false;
    }
    return runTx([=, this](sqlite3* db) -> bool {
        sqlite3_stmt* s = nullptr;
        // Uploading/Failed/Pending → Finalized；已 Finalized 且同 file_id 幂等
        const char* sql =
            "UPDATE upload_drafts SET file_id=?4,state=?5,error_code=0,updated_at=?6 "
            "WHERE owner_id=?1 AND msg_id=?2 AND variant=?3 AND "
            "(state IN(0,1,5) OR (state=2 AND file_id=?4))";
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK || !s) return false;
        sqlite3_bind_int64(s, 1, ownerId);
        bindText(s, 2, msgId);
        sqlite3_bind_int(s, 3, static_cast<int>(variant));
        bindText(s, 4, fileId);
        sqlite3_bind_int(s, 5, static_cast<int>(im::dto::UploadDraftState::Finalized));
        sqlite3_bind_int64(s, 6, static_cast<std::int64_t>(std::time(nullptr)));
        // 0 行命中 = 草稿不存在/终态/不同 file_id 冲突，必须失败
        const bool ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1;
        sqlite3_finalize(s);
        return ok;
    }, err);
}

bool NativeRepository::resetUploadDraftForNewSession(std::int64_t ownerId,
                                                     const std::string& msgId,
                                                     im::dto::MediaVariant variant,
                                                     const std::string& uploadId,
                                                     std::int64_t chunkSize,
                                                     std::int32_t chunkCount, std::string* err)
{
    if (ownerId <= 0 || msgId.empty() || uploadId.empty() || chunkSize <= 0 || chunkCount <= 0) {
        if (err) *err = "invalid reset session";
        return false;
    }
    return runTx([=, this](sqlite3* db) -> bool {
        sqlite3_stmt* s = nullptr;
        // 仅允许从非终态重置（服务端会话丢失后的恢复路径）
        const char* sql =
            "UPDATE upload_drafts SET upload_id=?4,chunk_size=?5,chunk_count=?6,chunks_done='',"
            "file_id='',state=?7,error_code=0,updated_at=?8 "
            "WHERE owner_id=?1 AND msg_id=?2 AND variant=?3 AND state IN(0,1,5)";
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK || !s) return false;
        sqlite3_bind_int64(s, 1, ownerId);
        bindText(s, 2, msgId);
        sqlite3_bind_int(s, 3, static_cast<int>(variant));
        bindText(s, 4, uploadId);
        sqlite3_bind_int64(s, 5, chunkSize);
        sqlite3_bind_int(s, 6, chunkCount);
        sqlite3_bind_int(s, 7, static_cast<int>(im::dto::UploadDraftState::Uploading));
        sqlite3_bind_int64(s, 8, static_cast<std::int64_t>(std::time(nullptr)));
        const bool ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1;
        sqlite3_finalize(s);
        return ok;
    }, err);
}

bool NativeRepository::setUploadDraftState(std::int64_t ownerId, const std::string& msgId,
                                           im::dto::MediaVariant variant,
                                           im::dto::UploadDraftState state,
                                           std::int32_t errorCode, std::string* err)
{
    if (ownerId <= 0 || msgId.empty()) {
        if (err) *err = "invalid draft state";
        return false;
    }
    return runTx([=, this](sqlite3* db) -> bool {
        sqlite3_stmt* s = nullptr;
        // 终态保护：Sent/Cancelled 后不可再迁移
        const char* sql =
            "UPDATE upload_drafts SET state=?4,error_code=?5,updated_at=?6 "
            "WHERE owner_id=?1 AND msg_id=?2 AND variant=?3 AND state NOT IN(3,4)";
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK || !s) return false;
        sqlite3_bind_int64(s, 1, ownerId);
        bindText(s, 2, msgId);
        sqlite3_bind_int(s, 3, static_cast<int>(variant));
        sqlite3_bind_int(s, 4, static_cast<int>(state));
        sqlite3_bind_int(s, 5, errorCode);
        sqlite3_bind_int64(s, 6, static_cast<std::int64_t>(std::time(nullptr)));
        // 0 行命中 = 草稿不存在或已终态（Sent/Cancelled 不可再迁移），必须失败
        const bool ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1;
        sqlite3_finalize(s);
        return ok;
    }, err);
}

bool NativeRepository::getUploadDraft(std::int64_t ownerId, const std::string& msgId,
                                      im::dto::MediaVariant variant,
                                      im::dto::UploadDraftDto* out, std::string* err)
{
    if (!out || ownerId <= 0 || msgId.empty()) {
        if (err) *err = "invalid get draft";
        return false;
    }
    std::vector<im::dto::UploadDraftDto> one;
    // 复用读路径：按主键过滤
    bool found = false;
    const ReadResult rr = m_db->withRead([&](sqlite3* db) {
        sqlite3_stmt* s = nullptr;
        const char* sql =
            "SELECT msg_id,variant,conversation_id,peer_id,local_path,file_name,file_size,sha256,"
            "content_type,upload_id,chunk_size,chunk_count,chunks_done,file_id,state,error_code,"
            "created_at,updated_at,image_width,image_height FROM upload_drafts "
            "WHERE owner_id=?1 AND msg_id=?2 AND variant=?3";
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) return;
        sqlite3_bind_int64(s, 1, ownerId);
        bindText(s, 2, msgId);
        sqlite3_bind_int(s, 3, static_cast<int>(variant));
        if (sqlite3_step(s) == SQLITE_ROW) {
            auto text = [&](int i) { const auto* t = sqlite3_column_text(s, i); return t ?
                std::string(reinterpret_cast<const char*>(t), sqlite3_column_bytes(s, i)) : std::string(); };
            out->msgId = text(0);
            out->variant = static_cast<im::dto::MediaVariant>(sqlite3_column_int(s, 1));
            out->conversationId = sqlite3_column_int64(s, 2);
            out->peerId = sqlite3_column_int64(s, 3);
            out->localPath = text(4);
            out->fileName = text(5);
            out->fileSize = sqlite3_column_int64(s, 6);
            out->sha256 = text(7);
            out->contentType = text(8);
            out->uploadId = text(9);
            out->chunkSize = sqlite3_column_int64(s, 10);
            out->chunkCount = sqlite3_column_int(s, 11);
            out->chunksDone = splitChunkIndices(text(12));
            out->fileId = text(13);
            out->state = static_cast<im::dto::UploadDraftState>(sqlite3_column_int(s, 14));
            out->errorCode = sqlite3_column_int(s, 15);
            out->createdAt = sqlite3_column_int64(s, 16);
            out->updatedAt = sqlite3_column_int64(s, 17);
            out->imageWidth = sqlite3_column_int(s, 18);
            out->imageHeight = sqlite3_column_int(s, 19);
            found = true;
        }
        sqlite3_finalize(s);
    });
    if (rr != ReadResult::Ok) {
        if (err) *err = "draft database unavailable";
        return false;
    }
    return found;
}

bool NativeRepository::loadActiveUploadDrafts(std::int64_t ownerId,
                                              std::vector<im::dto::UploadDraftDto>* out,
                                              std::string* err)
{
    if (out) out->clear();
    if (!m_db || !out || ownerId <= 0) {
        if (err) *err = "invalid load drafts";
        return false;
    }
    bool queryOk = false;
    const ReadResult rr = m_db->withRead([&](sqlite3* db) {
        sqlite3_stmt* s = nullptr;
        const char* sql =
            "SELECT msg_id,variant,conversation_id,peer_id,local_path,file_name,file_size,sha256,"
            "content_type,upload_id,chunk_size,chunk_count,chunks_done,file_id,state,error_code,"
            "created_at,updated_at,image_width,image_height FROM upload_drafts "
            "WHERE owner_id=?1 AND state IN(0,1,2,5) ORDER BY updated_at";
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) return;
        sqlite3_bind_int64(s, 1, ownerId);
        int rc = SQLITE_OK;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            auto text = [&](int i) { const auto* t = sqlite3_column_text(s, i); return t ?
                std::string(reinterpret_cast<const char*>(t), sqlite3_column_bytes(s, i)) : std::string(); };
            im::dto::UploadDraftDto d;
            d.msgId = text(0);
            d.variant = static_cast<im::dto::MediaVariant>(sqlite3_column_int(s, 1));
            d.conversationId = sqlite3_column_int64(s, 2);
            d.peerId = sqlite3_column_int64(s, 3);
            d.localPath = text(4);
            d.fileName = text(5);
            d.fileSize = sqlite3_column_int64(s, 6);
            d.sha256 = text(7);
            d.contentType = text(8);
            d.uploadId = text(9);
            d.chunkSize = sqlite3_column_int64(s, 10);
            d.chunkCount = sqlite3_column_int(s, 11);
            d.chunksDone = splitChunkIndices(text(12));
            d.fileId = text(13);
            d.state = static_cast<im::dto::UploadDraftState>(sqlite3_column_int(s, 14));
            d.errorCode = sqlite3_column_int(s, 15);
            d.createdAt = sqlite3_column_int64(s, 16);
            d.updatedAt = sqlite3_column_int64(s, 17);
            d.imageWidth = sqlite3_column_int(s, 18);
            d.imageHeight = sqlite3_column_int(s, 19);
            out->push_back(std::move(d));
        }
        queryOk = rc == SQLITE_DONE;
        sqlite3_finalize(s);
    });
    if (rr != ReadResult::Ok || !queryOk) {
        out->clear();
        if (err) *err = "draft database unavailable";
        return false;
    }
    return true;
}

} // namespace storage
} // namespace im
