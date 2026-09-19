#include "client_core/storage/MigrationImporter.h"

#include <sqlite3.h>

#include <sstream>
#include <stdexcept>

namespace im {
namespace storage {

namespace {

bool exec(sqlite3* db, const std::string& sql, std::string* err)
{
    char* e = nullptr;
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &e);
    if (rc != SQLITE_OK) {
        if (err) *err = e ? e : sqlite3_errmsg(db);
        if (e) sqlite3_free(e);
        return false;
    }
    return true;
}

std::string quote(const std::string& s)
{
    // 单引号转义；SQLite 标准转义方式
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string num(long long v) { return std::to_string(v); }

bool setMeta(sqlite3* db, std::int64_t ownerId, const std::string& key, const std::string& value,
             std::string* err)
{
    const std::string sql =
        "INSERT INTO migration_meta(key,value) VALUES(" +
        quote("owner_" + std::to_string(ownerId) + "_" + key) + "," + quote(value) + ") "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value";
    return exec(db, sql, err);
}

std::string metaKey(std::int64_t ownerId, const std::string& key)
{
    return "owner_" + std::to_string(ownerId) + "_" + key;
}

// F06：比对数据库中既有消息与待导入消息的全部持久化字段（messages 表，除 owner_id/msg_id）。
// 一致返回 true；不一致通过 mismatch 给出首个不一致字段名；行不存在返回 false（mismatch="missing"）。
bool sameMessage(sqlite3* db, std::int64_t ownerId, const MigrationMessage& m, std::string* mismatch)
{
    const char* sql =
        "SELECT conversation_id,peer_id,conversation_seq,server_time,local_order,from_me,type,"
        "content,status,media_path,img_w,img_h,file_id,file_name,file_size,content_type,sha256,"
        "thumbnail_file_id,thumbnail_path,thumbnail_size,thumbnail_sha256,thumbnail_w,thumbnail_h,"
        "large_thumbnail_file_id,large_thumbnail_path,large_thumbnail_size,large_thumbnail_sha256,"
        "large_thumbnail_w,large_thumbnail_h,local_path,transferred "
        "FROM messages WHERE owner_id=? AND msg_id=?";
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK || !s) return false;
    sqlite3_bind_int64(s, 1, ownerId);
    sqlite3_bind_text(s, 2, m.msgId.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        if (mismatch) *mismatch = "missing";
        return false;
    }
    auto i64 = [&](int i) { return sqlite3_column_int64(s, i); };
    auto i32 = [&](int i) { return sqlite3_column_int(s, i); };
    auto txt = [&](int i) {
        const unsigned char* t = sqlite3_column_text(s, i);
        return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
    };
    auto fail = [&](const char* name) {
        sqlite3_finalize(s);
        if (mismatch) *mismatch = name;
        return false;
    };
    if (i64(0) != m.conversationId) return fail("conversation_id");
    if (i64(1) != m.peerId) return fail("peer_id");
    if (i64(2) != m.conversationSeq) return fail("conversation_seq");
    if (i64(3) != m.serverTime) return fail("server_time");
    if (i64(4) != m.localOrder) return fail("local_order");
    if (i32(5) != m.fromMe) return fail("from_me");
    if (i32(6) != m.type) return fail("type");
    if (txt(7) != m.content) return fail("content");
    if (i32(8) != m.status) return fail("status");
    if (txt(9) != m.mediaPath) return fail("media_path");
    if (i32(10) != m.imgW) return fail("img_w");
    if (i32(11) != m.imgH) return fail("img_h");
    if (txt(12) != m.fileId) return fail("file_id");
    if (txt(13) != m.fileName) return fail("file_name");
    if (i64(14) != m.fileSize) return fail("file_size");
    if (txt(15) != m.contentType) return fail("content_type");
    if (txt(16) != m.sha256) return fail("sha256");
    if (txt(17) != m.thumbnailFileId) return fail("thumbnail_file_id");
    if (txt(18) != m.thumbnailPath) return fail("thumbnail_path");
    if (i64(19) != m.thumbnailSize) return fail("thumbnail_size");
    if (txt(20) != m.thumbnailSha256) return fail("thumbnail_sha256");
    if (i32(21) != m.thumbnailW) return fail("thumbnail_w");
    if (i32(22) != m.thumbnailH) return fail("thumbnail_h");
    if (txt(23) != m.largeThumbnailFileId) return fail("large_thumbnail_file_id");
    if (txt(24) != m.largeThumbnailPath) return fail("large_thumbnail_path");
    if (i64(25) != m.largeThumbnailSize) return fail("large_thumbnail_size");
    if (txt(26) != m.largeThumbnailSha256) return fail("large_thumbnail_sha256");
    if (i32(27) != m.largeThumbnailW) return fail("large_thumbnail_w");
    if (i32(28) != m.largeThumbnailH) return fail("large_thumbnail_h");
    if (txt(29) != m.localPath) return fail("local_path");
    if (i64(30) != m.transferred) return fail("transferred");
    sqlite3_finalize(s);
    if (mismatch) mismatch->clear();
    return true;
}

bool sameFts(sqlite3* db, std::int64_t ownerId, const MigrationMessage& m, std::string* mismatch)
{
    sqlite3_stmt* s = nullptr;
    const char* sql = "SELECT content,pinyin,initials FROM message_fts_identity "
                      "WHERE owner_id=? AND msg_id=?";
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK || !s) {
        if (mismatch) *mismatch = "fts_prepare";
        return false;
    }
    sqlite3_bind_int64(s, 1, ownerId);
    sqlite3_bind_text(s, 2, m.msgId.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(s);
    if (m.content.empty()) {
        sqlite3_finalize(s);
        if (rc == SQLITE_DONE) return true;
        if (mismatch) *mismatch = "fts_unexpected";
        return false;
    }
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(s);
        if (mismatch) *mismatch = "fts_missing";
        return false;
    }
    auto txt = [&](int i) {
        const unsigned char* t = sqlite3_column_text(s, i);
        return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
    };
    const bool same = txt(0) == m.content && txt(1) == m.pinyin && txt(2) == m.initials;
    sqlite3_finalize(s);
    if (!same && mismatch) *mismatch = "fts_content_or_pinyin";
    return same;
}

} // namespace

std::string toString(const MigrationSummary& s)
{
    std::ostringstream os;
    os << "messages=" << s.messageCount << " conversations=" << s.conversationCount
       << " seq=[" << s.minSeq << "," << s.maxSeq << "] fts=" << s.ftsCount;
    return os.str();
}

const char* toString(MigrationState s)
{
    switch (s) {
        case MigrationState::Disabled: return "disabled";
        case MigrationState::ShadowImport: return "shadow_import";
        case MigrationState::Verified: return "verified";
    }
    return "?";
}

bool MigrationImporter::importBatch(sqlite3* db, std::int64_t ownerId,
                                    const std::vector<MigrationMessage>& batch, std::string* err)
{
    if (!db) {
        if (err) *err = "db 为空";
        return false;
    }
    if (batch.empty()) return true;

    // ---- 预编译语句：10 万级导入下逐条拼 SQL 会显著变慢（曾导致 CTest 超时），
    //      同时避免大量字符串拼接带来的注入面。语句在本次调用内复用。 ----
    struct Stmt {
        sqlite3_stmt* s = nullptr;
        ~Stmt() { if (s) sqlite3_finalize(s); }
        bool ok() const { return s != nullptr; }
    };
    auto prep = [&](Stmt& st, const char* sql) -> bool {
        if (sqlite3_prepare_v2(db, sql, -1, &st.s, nullptr) != SQLITE_OK || !st.s) {
            if (err) *err = std::string("预编译失败：") + sqlite3_errmsg(db);
            return false;
        }
        return true;
    };

    Stmt insMsg, insFts, insFtsIdentity, upsertConv;
    if (!prep(insMsg,
              "INSERT INTO messages(owner_id,msg_id,conversation_id,peer_id,"
              "conversation_seq,server_time,local_order,from_me,type,content,status,"
              "media_path,img_w,img_h,file_id,file_name,file_size,content_type,sha256,"
              "thumbnail_file_id,thumbnail_path,thumbnail_size,thumbnail_sha256,thumbnail_w,"
              "thumbnail_h,large_thumbnail_file_id,large_thumbnail_path,large_thumbnail_size,"
              "large_thumbnail_sha256,large_thumbnail_w,large_thumbnail_h,local_path,transferred) "
              "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
              "ON CONFLICT(owner_id,msg_id) DO NOTHING") ||
        !prep(insFts, "INSERT INTO message_fts(content,pinyin,initials,msg_id) VALUES(?,?,?,?)") ||
        !prep(insFtsIdentity,
              "INSERT INTO message_fts_identity(owner_id,msg_id,content,pinyin,initials) "
              "VALUES(?,?,?,?,?)") ||
        !prep(upsertConv,
              "INSERT INTO conversations(conversation_id,owner_id,peer_id,last_message,"
              "last_message_time,unread,read_seq) VALUES(?,?,?,?,?,0,0) "
              "ON CONFLICT(conversation_id) DO UPDATE SET "
              "last_message=excluded.last_message,last_message_time=excluded.last_message_time "
              "WHERE excluded.last_message_time>conversations.last_message_time")) {
        return false;
    }

    auto bindText = [](sqlite3_stmt* s, int idx, const std::string& v) {
        return sqlite3_bind_text(s, idx, v.c_str(), -1, SQLITE_TRANSIENT);
    };
    auto runOnce = [&](Stmt& st) -> bool {
        const int rc = sqlite3_step(st.s);
        sqlite3_reset(st.s);
        if (rc != SQLITE_DONE) {
            if (err) *err = std::string("执行失败：") + sqlite3_errmsg(db);
            return false;
        }
        return true;
    };

    for (const auto& m : batch) {
        if (m.msgId.empty()) {
            if (err) *err = "存在空 msg_id（脏数据）";
            return false;
        }
        if (m.ownerId != ownerId) {
            if (err) *err = "msg_id=" + m.msgId + " 的 ownerId 与本次导入不一致";
            return false;
        }

        // ---- messages：按 (owner_id,msg_id) 幂等；冲突不覆盖（禁止 REPLACE）----
        sqlite3_bind_int64(insMsg.s, 1, m.ownerId);
        bindText(insMsg.s, 2, m.msgId);
        sqlite3_bind_int64(insMsg.s, 3, m.conversationId);
        sqlite3_bind_int64(insMsg.s, 4, m.peerId);
        sqlite3_bind_int64(insMsg.s, 5, m.conversationSeq);
        sqlite3_bind_int64(insMsg.s, 6, m.serverTime);
        sqlite3_bind_int64(insMsg.s, 7, m.localOrder);
        sqlite3_bind_int(insMsg.s, 8, m.fromMe);
        sqlite3_bind_int(insMsg.s, 9, m.type);
        bindText(insMsg.s, 10, m.content);
        sqlite3_bind_int(insMsg.s, 11, m.status);
        bindText(insMsg.s, 12, m.mediaPath);
        sqlite3_bind_int(insMsg.s, 13, m.imgW);
        sqlite3_bind_int(insMsg.s, 14, m.imgH);
        bindText(insMsg.s, 15, m.fileId);
        bindText(insMsg.s, 16, m.fileName);
        sqlite3_bind_int64(insMsg.s, 17, m.fileSize);
        bindText(insMsg.s, 18, m.contentType);
        bindText(insMsg.s, 19, m.sha256);
        bindText(insMsg.s, 20, m.thumbnailFileId);
        bindText(insMsg.s, 21, m.thumbnailPath);
        sqlite3_bind_int64(insMsg.s, 22, m.thumbnailSize);
        bindText(insMsg.s, 23, m.thumbnailSha256);
        sqlite3_bind_int(insMsg.s, 24, m.thumbnailW);
        sqlite3_bind_int(insMsg.s, 25, m.thumbnailH);
        bindText(insMsg.s, 26, m.largeThumbnailFileId);
        bindText(insMsg.s, 27, m.largeThumbnailPath);
        sqlite3_bind_int64(insMsg.s, 28, m.largeThumbnailSize);
        bindText(insMsg.s, 29, m.largeThumbnailSha256);
        sqlite3_bind_int(insMsg.s, 30, m.largeThumbnailW);
        sqlite3_bind_int(insMsg.s, 31, m.largeThumbnailH);
        bindText(insMsg.s, 32, m.localPath);
        sqlite3_bind_int64(insMsg.s, 33, m.transferred);
        // ON CONFLICT DO NOTHING：重复 msg_id 时 changes()=0，据此判断是否真插入。
        // 注意：绝不要对 FTS4 表执行 `DELETE ... WHERE msg_id=?`——msg_id 不在 FTS 索引里，
        // 会触发全表扫描（10 万级导入下退化成 O(n²)，实测耗时 11 分钟）。
        const int inserted = runOnce(insMsg) ? sqlite3_changes(db) : -1;
        if (inserted < 0) {
            if (err) *err = "插入消息失败（msg_id=" + m.msgId + "）：" + *err;
            return false;
        }

        if (inserted == 0) {
            // F06：重复 msg_id——比对全字段；一致 → 幂等成功；不一致 → 冲突回滚
            std::string mismatch;
            if (!sameMessage(db, m.ownerId, m, &mismatch)) {
                if (err) *err = "msg_id=" + m.msgId + " 重复且内容不一致（" + mismatch + "），拒绝覆盖";
                return false;
            }
            if (!sameFts(db, m.ownerId, m, &mismatch)) {
                if (err) *err = "msg_id=" + m.msgId + " 重复但 FTS 不一致（" + mismatch + "），拒绝覆盖";
                return false;
            }
            continue; // 幂等成功：跳过 FTS 与会话摘要，避免篡改摘要
        }

        // ---- FTS：仅在真正新增消息时写入，保证与 messages 一一对应（重复导入不翻倍）----
        if (!m.content.empty()) {
            bindText(insFts.s, 1, m.content);
            bindText(insFts.s, 2, m.pinyin);
            bindText(insFts.s, 3, m.initials);
            bindText(insFts.s, 4, m.msgId);
            if (!runOnce(insFts)) {
                if (err) *err = "写入 FTS 失败（msg_id=" + m.msgId + "）：" + *err;
                return false;
            }
            sqlite3_bind_int64(insFtsIdentity.s, 1, m.ownerId);
            bindText(insFtsIdentity.s, 2, m.msgId);
            bindText(insFtsIdentity.s, 3, m.content);
            bindText(insFtsIdentity.s, 4, m.pinyin);
            bindText(insFtsIdentity.s, 5, m.initials);
            if (!runOnce(insFtsIdentity)) {
                if (err) *err = "写入 FTS identity 失败（msg_id=" + m.msgId + "）：" + *err;
                return false;
            }
        }

        // ---- 会话摘要：按 server_time 取最新一条 ----
        sqlite3_bind_int64(upsertConv.s, 1, m.conversationId);
        sqlite3_bind_int64(upsertConv.s, 2, m.ownerId);
        sqlite3_bind_int64(upsertConv.s, 3, m.peerId);
        bindText(upsertConv.s, 4, m.content);
        sqlite3_bind_int64(upsertConv.s, 5, m.serverTime);
        if (!runOnce(upsertConv)) {
            if (err) *err = "更新会话摘要失败（conversation=" +
                            std::to_string(m.conversationId) + "）：" + *err;
            return false;
        }
    }
    return true;
}

bool MigrationImporter::importConversations(sqlite3* db, std::int64_t ownerId,
                                            const std::vector<MigrationConversation>& convs,
                                            std::string* err)
{
    if (!db) {
        if (err) *err = "db 为空";
        return false;
    }
    if (convs.empty()) return true;

    // 会话元数据权威导入：保留 unread/lastMsg/lastTs（覆盖消息流重建的 0 未读摘要）
    sqlite3_stmt* s = nullptr;
    const char* sql =
        "INSERT INTO conversations(conversation_id,owner_id,peer_id,last_message,"
        "last_message_time,unread,read_seq) VALUES(?,?,?,?,?,?,0) "
        "ON CONFLICT(conversation_id) DO UPDATE SET "
        "last_message=excluded.last_message,last_message_time=excluded.last_message_time,"
        "unread=excluded.unread";
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK || !s) {
        if (err) *err = std::string("预编译失败：") + sqlite3_errmsg(db);
        return false;
    }
    for (const auto& c : convs) {
        if (c.ownerId != ownerId) {
            if (err) *err = "conversation=" + std::to_string(c.conversationId) +
                            " 的 ownerId 与本次导入不一致";
            sqlite3_finalize(s);
            return false;
        }
        sqlite3_bind_int64(s, 1, c.conversationId);
        sqlite3_bind_int64(s, 2, c.ownerId);
        sqlite3_bind_int64(s, 3, c.peerId);
        sqlite3_bind_text(s, 4, c.lastMsg.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 5, c.lastTs);
        sqlite3_bind_int64(s, 6, c.unread);
        if (sqlite3_step(s) != SQLITE_DONE) {
            if (err) *err = "导入会话失败（conversation=" + std::to_string(c.conversationId) +
                            "）：" + sqlite3_errmsg(db);
            sqlite3_finalize(s);
            return false;
        }
        sqlite3_reset(s);
    }
    sqlite3_finalize(s);
    return true;
}

bool MigrationImporter::saveCheckpoint(sqlite3* db, std::int64_t ownerId,
                                       const std::string& checkpoint, std::string* err)
{
    return setMeta(db, ownerId, "checkpoint", checkpoint, err);
}

bool MigrationImporter::readCheckpoint(sqlite3* db, std::int64_t ownerId,
                                       std::string* checkpoint)
{
    if (!db || !checkpoint) return false;
    checkpoint->clear();
    sqlite3_stmt* s = nullptr;
    const std::string sql = "SELECT value FROM migration_meta WHERE key=?";
    bool found = false;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) == SQLITE_OK && s) {
        const std::string key = metaKey(ownerId, "checkpoint");
        sqlite3_bind_text(s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const unsigned char* t = sqlite3_column_text(s, 0);
            if (t) *checkpoint = reinterpret_cast<const char*>(t);
            found = true;
        }
    }
    if (s) sqlite3_finalize(s);
    return found;
}

bool MigrationImporter::saveConversationCheckpoint(sqlite3* db, std::int64_t ownerId,
                                                   const std::string& checkpoint,
                                                   std::string* err)
{
    return setMeta(db, ownerId, "conversations_checkpoint", checkpoint, err);
}

bool MigrationImporter::readConversationCheckpoint(sqlite3* db, std::int64_t ownerId,
                                                   std::string* checkpoint)
{
    if (!db || !checkpoint) return false;
    checkpoint->clear();
    sqlite3_stmt* s = nullptr;
    const std::string sql = "SELECT value FROM migration_meta WHERE key=?";
    bool found = false;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) == SQLITE_OK && s) {
        const std::string key = metaKey(ownerId, "conversations_checkpoint");
        sqlite3_bind_text(s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const unsigned char* t = sqlite3_column_text(s, 0);
            if (t) *checkpoint = reinterpret_cast<const char*>(t);
            found = true;
        }
    }
    if (s) sqlite3_finalize(s);
    return found;
}

bool MigrationImporter::readLegacyDeltaCheckpoint(sqlite3* db, std::int64_t epoch,
                                                   std::int64_t* checkpoint)
{
    if (!db || !checkpoint || epoch <= 0) return false;
    *checkpoint = 0;
    sqlite3_stmt* s = nullptr;
    const char* sql = "SELECT checkpoint_value FROM migration_checkpoint WHERE epoch=?1 AND stream='legacy_delta'";
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK || !s) return false;
    sqlite3_bind_int64(s, 1, epoch);
    const int rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
        const unsigned char* t = sqlite3_column_text(s, 0);
        if (t) {
            try { *checkpoint = std::stoll(reinterpret_cast<const char*>(t)); }
            catch (...) { sqlite3_finalize(s); return false; }
        }
    }
    sqlite3_finalize(s);
    return rc == SQLITE_ROW || rc == SQLITE_DONE;
}

bool MigrationImporter::seedLegacyDeltaCheckpoint(sqlite3* db, std::int64_t epoch,
                                                   std::int64_t baseline,
                                                   std::int64_t updatedAt,std::string* err)
{
    if(!db||epoch<=0||baseline<0||updatedAt<=0){if(err)*err="baseline 参数非法";return false;}
    sqlite3_stmt* read=nullptr;
    const char* query="SELECT checkpoint_value FROM migration_checkpoint "
                      "WHERE epoch=?1 AND stream='legacy_delta'";
    if(sqlite3_prepare_v2(db,query,-1,&read,nullptr)!=SQLITE_OK||!read){
        if(err)*err="读取 baseline 失败";return false;
    }
    sqlite3_bind_int64(read,1,epoch);
    const int rc=sqlite3_step(read);
    if(rc==SQLITE_ROW){
        const auto* text=sqlite3_column_text(read,0);std::int64_t current=-1;
        try{if(text)current=std::stoll(reinterpret_cast<const char*>(text));}catch(...){current=-1;}
        sqlite3_finalize(read);
        if(current==baseline)return true;
        if(err)*err="baseline 已存在且不一致";
        return false;
    }
    sqlite3_finalize(read);
    if(rc!=SQLITE_DONE){if(err)*err="读取 baseline 失败";return false;}
    sqlite3_stmt* insert=nullptr;
    const char* sql="INSERT INTO migration_checkpoint(epoch,stream,checkpoint_value,updated_at) "
                    "VALUES(?1,'legacy_delta',?2,?3)";
    if(sqlite3_prepare_v2(db,sql,-1,&insert,nullptr)!=SQLITE_OK||!insert){
        if(err)*err="准备 baseline 写入失败";return false;
    }
    const std::string value=std::to_string(baseline);
    sqlite3_bind_int64(insert,1,epoch);
    sqlite3_bind_text(insert,2,value.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert,3,updatedAt);
    const bool ok=sqlite3_step(insert)==SQLITE_DONE;
    if(!ok&&err)*err=sqlite3_errmsg(db);
    sqlite3_finalize(insert);
    return ok;
}

bool MigrationImporter::applyLegacyDeltaBatch(sqlite3* db, std::int64_t ownerId,
                                               std::int64_t epoch,
                                               std::int64_t expectedAfter,
                                               const std::vector<LegacyDeltaChange>& changes,
                                               std::int64_t* committedCheckpoint,
                                               std::string* err)
{
    if (committedCheckpoint) *committedCheckpoint = 0;
    if (!db || ownerId <= 0 || epoch <= 0 || expectedAfter < 0) {
        if (err) *err = "delta 参数非法";
        return false;
    }
    std::int64_t current = 0;
    if (!readLegacyDeltaCheckpoint(db, epoch, &current)) {
        if (err) *err = "读取 legacy_delta checkpoint 失败";
        return false;
    }
    if (changes.empty()) {
        if (current != expectedAfter) { if (err) *err = "delta checkpoint 不匹配"; return false; }
        if (committedCheckpoint) *committedCheckpoint = current;
        return true;
    }
    std::int64_t prev = expectedAfter;
    for (const auto& c : changes) {
        if (c.ownerId != ownerId || c.changeSeq <= prev) {
            if (err) *err = "delta owner 或 changeSeq 非严格递增";
            return false;
        }
        if ((c.entityType != "MESSAGE" && c.entityType != "CONVERSATION") ||
            (c.operation != "UPSERT" && c.operation != "DELETE") || c.entityKey.empty()) {
            if (err) *err = "delta 实体或操作非法";
            return false;
        }
        if ((c.operation == "UPSERT" && (c.payloadVersion != 1 || c.payload.empty())) ||
            (c.operation == "DELETE" && (c.payloadVersion != 0 || !c.payload.empty()))) {
            if (err) *err = "delta payload 版本或 tombstone 非法";
            return false;
        }
        if (c.entityType == "CONVERSATION") {
            try {
                std::size_t used = 0;
                const auto id = std::stoll(c.entityKey, &used);
                if (used != c.entityKey.size() || id <= 0) throw std::invalid_argument("id");
            } catch (...) {
                if (err) *err = "conversation entityKey 非法";
                return false;
            }
        }
        prev = c.changeSeq;
    }
    // 整批已落在 checkpoint 之前属于安全重放；校验内容后直接返回当前水位。
    if (expectedAfter < current && changes.back().changeSeq <= current) {
        if (committedCheckpoint) *committedCheckpoint = current;
        return true;
    }
    if (current != expectedAfter) {
        if (err) *err = "delta checkpoint 不匹配或批次跨越已提交水位";
        return false;
    }

    auto run = [&](const char* sql, const LegacyDeltaChange& c) -> bool {
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK || !s) {
            if (err) *err = sqlite3_errmsg(db);
            return false;
        }
        sqlite3_bind_int64(s, 1, ownerId);
        sqlite3_bind_text(s, 2, c.entityKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, c.payload.c_str(), -1, SQLITE_TRANSIENT);
        const bool ok = sqlite3_step(s) == SQLITE_DONE;
        if (!ok && err) *err = sqlite3_errmsg(db);
        sqlite3_finalize(s);
        return ok;
    };

    const char* upsertMessage =
        "INSERT INTO messages(owner_id,msg_id,conversation_id,peer_id,conversation_seq,server_time,"
        "local_order,from_me,type,content,status,media_path,img_w,img_h,file_id,file_name,file_size,"
        "content_type,sha256,thumbnail_file_id,thumbnail_path,thumbnail_size,thumbnail_sha256,"
        "thumbnail_w,thumbnail_h,large_thumbnail_file_id,large_thumbnail_path,large_thumbnail_size,"
        "large_thumbnail_sha256,large_thumbnail_w,large_thumbnail_h,local_path,transferred) VALUES("
        "?1,?2,json_extract(?3,'$.conversationId'),json_extract(?3,'$.peerId'),json_extract(?3,'$.seq'),"
        "json_extract(?3,'$.ts'),json_extract(?3,'$.localOrder'),json_extract(?3,'$.fromMe'),"
        "json_extract(?3,'$.type'),json_extract(?3,'$.content'),json_extract(?3,'$.status'),"
        "json_extract(?3,'$.mediaPath'),json_extract(?3,'$.imgW'),json_extract(?3,'$.imgH'),"
        "json_extract(?3,'$.fileId'),json_extract(?3,'$.fileName'),json_extract(?3,'$.fileSize'),"
        "json_extract(?3,'$.contentType'),json_extract(?3,'$.sha256'),json_extract(?3,'$.thumbnailFileId'),"
        "json_extract(?3,'$.thumbnailPath'),json_extract(?3,'$.thumbnailSize'),json_extract(?3,'$.thumbnailSha256'),"
        "json_extract(?3,'$.thumbnailW'),json_extract(?3,'$.thumbnailH'),json_extract(?3,'$.largeThumbnailFileId'),"
        "json_extract(?3,'$.largeThumbnailPath'),json_extract(?3,'$.largeThumbnailSize'),"
        "json_extract(?3,'$.largeThumbnailSha256'),json_extract(?3,'$.largeThumbnailW'),"
        "json_extract(?3,'$.largeThumbnailH'),json_extract(?3,'$.localPath'),json_extract(?3,'$.transferred')) "
        "ON CONFLICT(owner_id,msg_id) DO UPDATE SET conversation_id=excluded.conversation_id,peer_id=excluded.peer_id,"
        "conversation_seq=excluded.conversation_seq,server_time=excluded.server_time,local_order=excluded.local_order,"
        "from_me=excluded.from_me,type=excluded.type,content=excluded.content,status=excluded.status,media_path=excluded.media_path,"
        "img_w=excluded.img_w,img_h=excluded.img_h,file_id=excluded.file_id,file_name=excluded.file_name,file_size=excluded.file_size,"
        "content_type=excluded.content_type,sha256=excluded.sha256,thumbnail_file_id=excluded.thumbnail_file_id,"
        "thumbnail_path=excluded.thumbnail_path,thumbnail_size=excluded.thumbnail_size,thumbnail_sha256=excluded.thumbnail_sha256,"
        "thumbnail_w=excluded.thumbnail_w,thumbnail_h=excluded.thumbnail_h,large_thumbnail_file_id=excluded.large_thumbnail_file_id,"
        "large_thumbnail_path=excluded.large_thumbnail_path,large_thumbnail_size=excluded.large_thumbnail_size,"
        "large_thumbnail_sha256=excluded.large_thumbnail_sha256,large_thumbnail_w=excluded.large_thumbnail_w,"
        "large_thumbnail_h=excluded.large_thumbnail_h,local_path=excluded.local_path,transferred=excluded.transferred";
    const char* upsertConversation =
        "INSERT INTO conversations(conversation_id,owner_id,peer_id,last_message,last_message_time,unread,read_seq) "
        "VALUES(CAST(?2 AS INTEGER),?1,json_extract(?3,'$.peerId'),json_extract(?3,'$.lastMsg'),"
        "json_extract(?3,'$.lastTs'),json_extract(?3,'$.unread'),0) ON CONFLICT(conversation_id) DO UPDATE SET "
        "owner_id=excluded.owner_id,peer_id=excluded.peer_id,last_message=excluded.last_message,"
        "last_message_time=excluded.last_message_time,unread=excluded.unread";

    for (const auto& c : changes) {
        if (c.entityType == "MESSAGE") {
            // FTS4 无 owner/index；delta 单条更新按 msg_id 删除旧行，再维护 O(1) identity 镜像。
            // 批量 snapshot 路径仍禁止这样做（会 O(n²)），delta 单页上限 500 且更新频率低。
            const std::string cleanFts =
                "DELETE FROM message_fts WHERE msg_id=" + quote(c.entityKey) + ";"
                "DELETE FROM message_fts_identity WHERE owner_id=" + num(ownerId) + " AND msg_id=" + quote(c.entityKey) + ";";
            if (!exec(db, cleanFts, err)) return false;
            if (c.operation == "DELETE") {
                std::int64_t affectedConversation = 0;
                sqlite3_stmt* find = nullptr;
                if (sqlite3_prepare_v2(db, "SELECT conversation_id FROM messages WHERE owner_id=?1 AND msg_id=?2",
                                       -1, &find, nullptr) != SQLITE_OK || !find) return false;
                sqlite3_bind_int64(find, 1, ownerId);
                sqlite3_bind_text(find, 2, c.entityKey.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(find) == SQLITE_ROW) affectedConversation = sqlite3_column_int64(find, 0);
                sqlite3_finalize(find);
                if (!exec(db, "DELETE FROM media_refs WHERE owner_id=" + num(ownerId) + " AND msg_id=" + quote(c.entityKey) +
                              ";DELETE FROM outbox WHERE owner_id=" + num(ownerId) + " AND msg_id=" + quote(c.entityKey) +
                              ";DELETE FROM messages WHERE owner_id=" + num(ownerId) + " AND msg_id=" + quote(c.entityKey), err)) return false;
                if (affectedConversation > 0) {
                    const std::string conv = num(affectedConversation);
                    const std::string recalc =
                        "UPDATE conversations SET "
                        "last_message=COALESCE((SELECT content FROM messages WHERE owner_id=" + num(ownerId) +
                        " AND conversation_id=" + conv + " ORDER BY server_time DESC,conversation_seq DESC,local_order DESC,msg_id DESC LIMIT 1),''),"
                        "last_message_time=COALESCE((SELECT server_time FROM messages WHERE owner_id=" + num(ownerId) +
                        " AND conversation_id=" + conv + " ORDER BY server_time DESC,conversation_seq DESC,local_order DESC,msg_id DESC LIMIT 1),0),"
                        "last_msg_id=COALESCE((SELECT msg_id FROM messages WHERE owner_id=" + num(ownerId) +
                        " AND conversation_id=" + conv + " ORDER BY server_time DESC,conversation_seq DESC,local_order DESC,msg_id DESC LIMIT 1),'') "
                        "WHERE owner_id=" + num(ownerId) + " AND conversation_id=" + conv;
                    if (!exec(db, recalc, err)) return false;
                }
            } else {
                if (!run(upsertMessage, c)) return false;
                const char* fts =
                    "INSERT INTO message_fts(content,pinyin,initials,msg_id) "
                    "SELECT json_extract(?3,'$.content'),json_extract(?3,'$.pinyin'),json_extract(?3,'$.initials'),?2 "
                    "WHERE COALESCE(json_extract(?3,'$.content'),'')<>''";
                const char* identity =
                    "INSERT INTO message_fts_identity(owner_id,msg_id,content,pinyin,initials) "
                    "SELECT ?1,?2,json_extract(?3,'$.content'),json_extract(?3,'$.pinyin'),json_extract(?3,'$.initials') "
                    "WHERE COALESCE(json_extract(?3,'$.content'),'')<>''";
                if (!run(fts, c) || !run(identity, c)) return false;
                const char* derivedConversation =
                    "INSERT INTO conversations(conversation_id,owner_id,peer_id,last_message,last_message_time,unread,read_seq,last_msg_id) "
                    "VALUES(json_extract(?3,'$.conversationId'),?1,json_extract(?3,'$.peerId'),json_extract(?3,'$.content'),"
                    "json_extract(?3,'$.ts'),0,0,?2) ON CONFLICT(conversation_id) DO UPDATE SET "
                    "last_message=excluded.last_message,last_message_time=excluded.last_message_time,last_msg_id=excluded.last_msg_id "
                    "WHERE excluded.last_message_time>=conversations.last_message_time";
                if (!run(derivedConversation, c)) return false;
            }
        } else if (c.operation == "DELETE") {
            if (!exec(db, "DELETE FROM conversations WHERE owner_id=" + num(ownerId) +
                          " AND conversation_id=" + c.entityKey, err)) return false;
        } else if (!run(upsertConversation, c)) return false;
    }

    sqlite3_stmt* cp = nullptr;
    const char* cpSql = "INSERT INTO migration_checkpoint(epoch,stream,checkpoint_value,updated_at) "
                        "VALUES(?1,'legacy_delta',?2,?3) ON CONFLICT(epoch,stream) DO UPDATE SET "
                        "checkpoint_value=excluded.checkpoint_value,updated_at=excluded.updated_at";
    if (sqlite3_prepare_v2(db, cpSql, -1, &cp, nullptr) != SQLITE_OK || !cp) return false;
    sqlite3_bind_int64(cp, 1, epoch);
    const std::string value = std::to_string(prev);
    sqlite3_bind_text(cp, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(cp, 3, changes.back().changedAt);
    const bool ok = sqlite3_step(cp) == SQLITE_DONE;
    if (!ok && err) *err = sqlite3_errmsg(db);
    sqlite3_finalize(cp);
    if (ok && committedCheckpoint) *committedCheckpoint = prev;
    return ok;
}

bool MigrationImporter::computeSummary(sqlite3* db, std::int64_t ownerId, MigrationSummary* out,
                                       std::string* err)
{
    if (!db || !out) return false;
    auto scalar = [&](const std::string& sql, std::int64_t* v) -> bool {
        sqlite3_stmt* s = nullptr;
        bool ok = false;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) == SQLITE_OK && s) {
            if (sqlite3_step(s) == SQLITE_ROW) {
                *v = sqlite3_column_int64(s, 0);
                ok = true;
            }
        }
        if (s) sqlite3_finalize(s);
        return ok;
    };

    const std::string owner = num(ownerId);
    MigrationSummary sum;
    if (!scalar("SELECT count(*) FROM messages WHERE owner_id=" + owner, &sum.messageCount) ||
        !scalar("SELECT count(*) FROM conversations WHERE owner_id=" + owner,
                &sum.conversationCount) ||
        !scalar("SELECT count(*) FROM message_fts", &sum.ftsCount)) {
        if (err) *err = "统计摘要失败";
        return false;
    }
    // seq 区间只统计有效 seq（>0）
    scalar("SELECT min(conversation_seq) FROM messages WHERE owner_id=" + owner +
               " AND conversation_seq>0",
           &sum.minSeq);
    scalar("SELECT max(conversation_seq) FROM messages WHERE owner_id=" + owner +
               " AND conversation_seq>0",
           &sum.maxSeq);
    *out = sum;
    return true;
}

bool MigrationImporter::finish(sqlite3* db, std::int64_t ownerId,
                               const MigrationSummary& expected, MigrationSummary* actual,
                               std::string* err)
{
    std::string messageCheckpoint;
    std::string conversationCheckpoint;
    if (!readCheckpoint(db, ownerId, &messageCheckpoint) ||
        !readConversationCheckpoint(db, ownerId, &conversationCheckpoint)) {
        if (err) *err = "读取迁移双流 checkpoint 失败";
        return false;
    }
    if (expected.messageCount > 0 && messageCheckpoint.empty()) {
        if (err) *err = "消息流尚未提交 checkpoint，拒绝完成迁移";
        return false;
    }
    if (expected.conversationCount > 0 && conversationCheckpoint.empty()) {
        if (err) *err = "会话流尚未提交 checkpoint，拒绝完成迁移";
        return false;
    }
    MigrationSummary got;
    if (!computeSummary(db, ownerId, &got, err)) return false;
    if (actual) *actual = got;

    if (!(got == expected)) {
        if (err) {
            *err = "对账失败：期望 [" + toString(expected) + "]，实际 [" + toString(got) + "]";
        }
        return false; // 不写完成标记；保留旧 Room 为事实源
    }
    // maxSeq 只是会话内序号的摘要，不能冒充全局增量高水位。真正的 change_seq 留给 P13。
    if (!setMeta(db, ownerId, "snapshot_max_conversation_seq", std::to_string(got.maxSeq), err)) return false;
    if (!setMeta(db, ownerId, "messages_stream_completed", "1", err)) return false;
    if (!setMeta(db, ownerId, "conversations_stream_completed", "1", err)) return false;
    if (!setMeta(db, ownerId, "completed", "1", err)) return false;
    return setState(db, ownerId, MigrationState::Verified, err); // 对账通过 → verified
}

bool MigrationImporter::isCompleted(sqlite3* db, std::int64_t ownerId, bool* completed)
{
    if (!db || !completed) return false;
    *completed = false;
    sqlite3_stmt* s = nullptr;
    bool ok = false;
    const std::string sql = "SELECT value FROM migration_meta WHERE key=?";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) == SQLITE_OK && s) {
        const std::string key = metaKey(ownerId, "completed");
        sqlite3_bind_text(s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const unsigned char* t = sqlite3_column_text(s, 0);
            *completed = (t != nullptr && std::string(reinterpret_cast<const char*>(t)) == "1");
            ok = true;
        } else {
            ok = true; // 无记录 → completed=false（查询本身成功）
        }
    }
    if (s) sqlite3_finalize(s);
    return ok;
}

bool MigrationImporter::setState(sqlite3* db, std::int64_t ownerId, MigrationState state,
                                 std::string* err)
{
    return setMeta(db, ownerId, "state", toString(state), err);
}

bool MigrationImporter::getState(sqlite3* db, std::int64_t ownerId, MigrationState* out)
{
    if (!db || !out) return false;
    *out = MigrationState::Disabled;
    sqlite3_stmt* s = nullptr;
    bool ok = false;
    const std::string sql = "SELECT value FROM migration_meta WHERE key=?";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) == SQLITE_OK && s) {
        const std::string key = metaKey(ownerId, "state");
        sqlite3_bind_text(s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const unsigned char* t = sqlite3_column_text(s, 0);
            const std::string v = t ? reinterpret_cast<const char*>(t) : "";
            if (v == "shadow_import") *out = MigrationState::ShadowImport;
            else if (v == "verified") *out = MigrationState::Verified;
            else *out = MigrationState::Disabled;
            ok = true;
        } else {
            ok = true; // 无记录 → 默认 Disabled
        }
    }
    if (s) sqlite3_finalize(s);
    return ok;
}

bool MigrationImporter::beginImport(sqlite3* db, std::int64_t ownerId, std::string* err)
{
    MigrationState st;
    if (!getState(db, ownerId, &st)) {
        if (err) *err = "读取迁移状态失败";
        return false;
    }
    if (st == MigrationState::Verified) {
        if (err) *err = "迁移已完成（verified），拒绝重复迁移";
        return false;
    }
    // 初始化双流元数据，使空源库也有明确的“已开始但尚未完成”记录。
    if (!setMeta(db, ownerId, "completed", "0", err)) return false;
    if (!setMeta(db, ownerId, "checkpoint", "", err)) return false;
    if (!setMeta(db, ownerId, "conversations_checkpoint", "", err)) return false;
    if (!setMeta(db, ownerId, "messages_stream_completed", "0", err)) return false;
    if (!setMeta(db, ownerId, "conversations_stream_completed", "0", err)) return false;
    return setState(db, ownerId, MigrationState::ShadowImport, err);
}

bool MigrationImporter::resetForReimport(sqlite3* db, std::int64_t ownerId, std::string* err)
{
    // 回滚：清完成标记与 checkpoint，状态退回 ShadowImport（允许重新导入，不删生产数据）
    if (!setMeta(db, ownerId, "completed", "0", err)) return false;
    if (!setMeta(db, ownerId, "checkpoint", "", err)) return false;
    if (!setMeta(db, ownerId, "conversations_checkpoint", "", err)) return false;
    if (!setMeta(db, ownerId, "snapshot_max_conversation_seq", "", err)) return false;
    if (!setMeta(db, ownerId, "source_high_watermark", "", err)) return false;
    if (!setMeta(db, ownerId, "messages_stream_completed", "0", err)) return false;
    if (!setMeta(db, ownerId, "conversations_stream_completed", "0", err)) return false;
    return setState(db, ownerId, MigrationState::ShadowImport, err);
}

} // namespace storage
} // namespace im
