#include "client_core/storage/CipherDatabase.h"
#include "client_core/storage/CipherParams.h"

#include <sqlite3.h>

namespace im {
namespace storage {

const char* toString(CipherError e)
{
    switch (e) {
        case CipherError::Ok:                return "Ok";
        case CipherError::OpenFailed:        return "OpenFailed";
        case CipherError::KeyFailed:         return "KeyFailed";
        case CipherError::BadKeyLength:      return "BadKeyLength";
        case CipherError::CipherUnavailable: return "CipherUnavailable";
        case CipherError::NotEncrypted:      return "NotEncrypted";
        case CipherError::Internal:          return "Internal";
    }
    return "?";
}

CipherDatabase::~CipherDatabase()
{
    close();
}

void CipherDatabase::close()
{
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

std::string CipherDatabase::cipherVersion() const
{
    if (!m_db) return {};
    sqlite3_stmt* stmt = nullptr;
    std::string version;
    if (sqlite3_prepare_v2(m_db, "PRAGMA cipher_version", -1, &stmt, nullptr) == SQLITE_OK &&
        stmt != nullptr) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(stmt, 0);
            if (text) version = reinterpret_cast<const char*>(text);
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    return version;
}

CipherError CipherDatabase::openConnection(const std::string& path,
                                            const std::vector<unsigned char>& key32, bool readonly,
                                            sqlite3** outDb, std::string* message)
{
    if (outDb) *outDb = nullptr;
    if (message) message->clear();

    // ---- 0) key 校验（长度不符直接拒绝，绝不进入后续流程）----
    if (key32.size() != CipherParams::kRawKeyBytes) {
        if (message) {
            *message = "raw key 长度必须为 " +
                       std::to_string(CipherParams::kRawKeyBytes) + " 字节";
        }
        return CipherError::BadKeyLength;
    }

    // ---- 1) sqlite3_open_v2 ----
    sqlite3* db = nullptr;
    const int flags = readonly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    if (sqlite3_open_v2(path.c_str(), &db, flags, nullptr) != SQLITE_OK || db == nullptr) {
        if (message) *message = db ? sqlite3_errmsg(db) : "sqlite3_open_v2 失败";
        if (db) sqlite3_close(db);
        return CipherError::OpenFailed;
    }

    // ---- 2) sqlite3_key ----
    // 必须与 Room（sqlcipher-android）的约定一致：把 32 字节 key 材料**直接**交给
    // sqlite3_key（即以它为口令，由 KDF 派生实际密钥）。
    // 注意：不要用 x'<64hex>' 字面量（那是 SQLCipher 的 raw-key 模式，与 Room 不同，
    // 实测会导致双方互开失败 code 26）。详见 encryption-note.md 的 S1-3 结论。
    const int keyRc = sqlite3_key(db, key32.data(), static_cast<int>(key32.size()));
    if (keyRc != SQLITE_OK) {
        if (message) *message = "sqlite3_key 失败（sqlite rc=" + std::to_string(keyRc) + "）";
        sqlite3_close(db);
        return CipherError::KeyFailed;
    }

    // ---- 3) 固化 cipher 参数（与 Room/sqlcipher-android 4.6.1 实测默认值一致）----
    const std::string setParams =
        std::string("PRAGMA cipher_page_size = ") + std::to_string(CipherParams::kPageSize) + ";" +
        "PRAGMA kdf_iter = " + std::to_string(CipherParams::kKdfIter) + ";" +
        "PRAGMA cipher_kdf_algorithm = " + CipherParams::kKdfAlgorithm + ";" +
        "PRAGMA cipher_hmac_algorithm = " + CipherParams::kHmacAlgorithm + ";" +
        "PRAGMA cipher_use_hmac = " + std::to_string(CipherParams::kUseHmac) + ";" +
        "PRAGMA cipher_plaintext_header_size = " +
        std::to_string(CipherParams::kPlaintextHeaderSize) + ";";
    {
        char* err = nullptr;
        const int rc = sqlite3_exec(db, setParams.c_str(), nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            if (message) {
                *message = std::string("设置 cipher 参数失败：") + (err ? err : sqlite3_errmsg(db));
            }
            if (err) sqlite3_free(err);
            sqlite3_close(db);
            return CipherError::Internal;
        }
    }

    // ---- 4) 校验 cipher 版本（fail-close：不是预期版本就不算成功）----
    {
        std::string version;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "PRAGMA cipher_version", -1, &stmt, nullptr) == SQLITE_OK &&
            stmt != nullptr) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char* t = sqlite3_column_text(stmt, 0);
                if (t) version = reinterpret_cast<const char*>(t);
            }
        }
        if (stmt) sqlite3_finalize(stmt);
        if (version.rfind(CipherParams::kExpectedCipherVersion, 0) != 0) {
            if (message) {
                *message = "cipher_version 不符合预期（期望前缀 " +
                           std::string(CipherParams::kExpectedCipherVersion) + "）";
            }
            sqlite3_close(db);
            return CipherError::CipherUnavailable;
        }
    }

    // ---- 5) 首个受保护查询：真正验证 key（错误 key 在此失败，实测 rc=26）----
    {
        char* err = nullptr;
        const int rc = sqlite3_exec(db, "SELECT count(*) FROM sqlite_master;", nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            if (message) {
                *message = std::string("首个受保护查询失败（key 错误 / 文件非加密库 / 头损坏）：") +
                           (err ? err : sqlite3_errmsg(db));
            }
            if (err) sqlite3_free(err);
            sqlite3_close(db);
            return CipherError::NotEncrypted;
        }
    }

    if (outDb) *outDb = db;
    return CipherError::Ok;
}

CipherError CipherDatabase::open(const std::string& path, const std::vector<unsigned char>& key32)
{
    if (m_db) close();
    m_lastMessage.clear();

    sqlite3* db = nullptr;
    const CipherError e = openConnection(path, key32, /*readonly=*/false, &db, &m_lastMessage);
    if (e != CipherError::Ok) {
        return e;
    }
    m_db = db;
    m_lastMessage.clear();
    return CipherError::Ok;
}

} // namespace storage
} // namespace im
