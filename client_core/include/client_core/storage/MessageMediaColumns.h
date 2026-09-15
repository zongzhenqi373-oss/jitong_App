#pragma once

#include "client_core/dto/Dtos.h"
#include <sqlite3.h>

namespace im { namespace storage {
// One mapping for both binding and reading. Remote metadata is authoritative in messages;
// media_variants only describes the independently replaceable local cache.
#define IM_MEDIA_COLUMNS(TEXT, NUMBER) \
    TEXT(media_path, mediaPath) NUMBER(img_w, imgW) NUMBER(img_h, imgH) \
    TEXT(file_id, fileId) TEXT(file_name, fileName) NUMBER(file_size, fileSize) \
    TEXT(content_type, contentType) TEXT(sha256, sha256) \
    TEXT(thumbnail_file_id, thumbnailFileId) TEXT(thumbnail_path, thumbnailPath) \
    NUMBER(thumbnail_size, thumbnailSize) TEXT(thumbnail_sha256, thumbnailSha256) \
    NUMBER(thumbnail_w, thumbnailW) NUMBER(thumbnail_h, thumbnailH) \
    TEXT(large_thumbnail_file_id, largeThumbnailFileId) TEXT(large_thumbnail_path, largeThumbnailPath) \
    NUMBER(large_thumbnail_size, largeThumbnailSize) TEXT(large_thumbnail_sha256, largeThumbnailSha256) \
    NUMBER(large_thumbnail_w, largeThumbnailW) NUMBER(large_thumbnail_h, largeThumbnailH) \
    TEXT(local_path, localPath) NUMBER(transferred, transferred)

inline bool fillMissingMedia(sqlite3* db, const im::dto::MessageDto& m) {
    // Never replace a known value with an empty duplicate Push payload.
#define TEXT(c,f) #c "=CASE WHEN COALESCE(" #c ",'')='' THEN ? ELSE " #c " END,"
#define NUMBER(c,f) #c "=CASE WHEN COALESCE(" #c ",0)=0 THEN ? ELSE " #c " END,"
    std::string sql = "UPDATE messages SET " IM_MEDIA_COLUMNS(TEXT, NUMBER);
#undef TEXT
#undef NUMBER
    sql.pop_back();
    sql += " WHERE owner_id=? AND msg_id=?";
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) != SQLITE_OK) return false;
    int i = 1;
#define TEXT(c,f) sqlite3_bind_text(s,i++,m.f.data(),static_cast<int>(m.f.size()),SQLITE_TRANSIENT);
#define NUMBER(c,f) sqlite3_bind_int64(s,i++,m.f);
    IM_MEDIA_COLUMNS(TEXT, NUMBER)
#undef TEXT
#undef NUMBER
    sqlite3_bind_int64(s,i++,m.ownerId);
    sqlite3_bind_text(s,i,m.msgId.data(),static_cast<int>(m.msgId.size()),SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}
inline std::string mediaSelectColumns() {
#define COLUMN(c,f) #c ","
    std::string columns = IM_MEDIA_COLUMNS(COLUMN, COLUMN);
#undef COLUMN
    columns.pop_back();
    return columns;
}
inline void readMediaColumns(sqlite3_stmt* s, int offset, im::dto::MessageDto& m) {
    int i = offset;
#define TEXT(c,f) { const auto* p=sqlite3_column_text(s,i); const int n=sqlite3_column_bytes(s,i++); m.f=p?std::string(reinterpret_cast<const char*>(p),n):std::string(); }
#define NUMBER(c,f) m.f=sqlite3_column_int64(s,i++);
    IM_MEDIA_COLUMNS(TEXT, NUMBER)
#undef TEXT
#undef NUMBER
}
#undef IM_MEDIA_COLUMNS
} }
