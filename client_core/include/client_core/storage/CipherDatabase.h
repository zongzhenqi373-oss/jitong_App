// SQLCipher 数据库打开器（P6-T01）。
//
// 职责边界：只负责"安全地打开一个加密数据库"——固定的打开顺序、raw key 编码、
// 参数固化、以及 fail-close 的错误判定。不负责业务 Schema、连接池、迁移（分别在 T03/T04/T05）。
//
// 固定打开顺序（不可颠倒、不可短路）：
//   sqlite3_open_v2 → sqlite3_key(raw key) → 固化 cipher 参数 → 首个受保护查询
// 其中"首个受保护查询"是**真正验证 key 是否正确的动作**：错误 key 时它会失败
// （SQLITE_NOTADB，实测 rc=26），而不是等到业务读写才暴露。
//
// key 使用约定（**实测确定，勿改**）：把 32 字节 key 材料**直接**交给 sqlite3_key，
// 即以它为口令、由 KDF（PBKDF2_HMAC_SHA512，256000 轮）派生实际密钥。
// 这与 Room 使用的 `net.zetetic:sqlcipher-android` SupportOpenHelperFactory(key) 一致。
//
// ⚠️ 不要改用 SQLCipher 的 raw-key 字面量 `x'<64hex>'`：那是另一种模式，与 Room 不兼容，
//    实测会导致 Room↔Native 互开失败（code 26 / SQLITE_NOTADB）。
//    详见 outputs/kernel-round5-encryption-note.md 的 S1-3 结论。
//
// 安全约束：
//   - 任何失败路径都 close 并清零 key 字面量缓冲，**不删除/覆盖/截断现有文件**；
//   - key 相关的错误只返回错误码，错误消息中不得包含 key 或其 hex；
//   - 本类不持久化 key、不提供 key getter。

#ifndef CLIENT_CORE_CIPHER_DATABASE_H
#define CLIENT_CORE_CIPHER_DATABASE_H

#include <cstddef>
#include <string>
#include <vector>

struct sqlite3; // 前向声明：公共头文件不暴露 sqlite3.h

namespace im {
namespace storage {

enum class CipherError {
    Ok = 0,
    OpenFailed,          // sqlite3_open_v2 失败（路径/权限等）
    KeyFailed,           // sqlite3_key 调用失败
    BadKeyLength,        // key 为空或长度不等于 CipherParams::kRawKeyBytes
    CipherUnavailable,   // cipher_version 取不到或与期望版本不符 → fail-close
    NotEncrypted,        // 首个受保护查询失败：错误 key / 文件头损坏 / 实际是明文库
    Internal,            // 其它内部错误
};

const char* toString(CipherError e);

class CipherDatabase {
public:
    CipherDatabase() = default;
    ~CipherDatabase();

    CipherDatabase(const CipherDatabase&) = delete;
    CipherDatabase& operator=(const CipherDatabase&) = delete;

    /**
     * 以 raw key 打开（必要时创建）加密数据库。
     * @param path  数据库文件路径
     * @param key32 32 字节 raw key（与 DbKeyManager 的 realKey 一致）
     * @return CipherError::Ok 表示已打开且 key 校验通过
     */
    CipherError open(const std::string& path, const std::vector<unsigned char>& key32);

    /**
     * 打开一条**独立**的加密连接（供只读连接池等多连接场景使用）。
     * @param path     数据库文件路径
     * @param key32    32 字节 raw key
     * @param readonly true 表示以只读方式打开（库文件不存在时直接失败）
     * @param outDb    成功时输出 sqlite3*，调用方负责 sqlite3_close
     * @param message  失败时输出原因（不含 key）
     */
    static CipherError openConnection(const std::string& path,
                                      const std::vector<unsigned char>& key32, bool readonly,
                                      sqlite3** outDb, std::string* message = nullptr);

    /** 关闭并释放资源。可重复调用。 */
    void close();

    bool isOpen() const { return m_db != nullptr; }

    /** 当前连接的 cipher_version（仅在 open 成功后有效）。 */
    std::string cipherVersion() const;

    /** 原生句柄：仅供 C++ 存储层内部使用，**不得**跨 JNI 暴露。 */
    sqlite3* nativeHandle() const { return m_db; }

    /** 最近一次 sqlite 错误消息（不含 key）。失败诊断用。 */
    const std::string& lastMessage() const { return m_lastMessage; }

private:
    sqlite3* m_db = nullptr;
    std::string m_lastMessage;
};

} // namespace storage
} // namespace im

#endif // CLIENT_CORE_CIPHER_DATABASE_H
