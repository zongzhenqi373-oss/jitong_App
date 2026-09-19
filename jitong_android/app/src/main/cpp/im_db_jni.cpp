// 数据库层 JNI（P6）。
//
// 本文件当前只包含 **S1-3 双向 fixture 验证** 所需的「测试专用受限接口」，用于证明：
//   Room 建的加密库 能被 Native 打开；Native 建的加密库 能被 Room 打开。
// 这些接口：
//   - 名字统一带 ForTest 后缀，明确表示**仅测试使用**；
//   - 不接受任意 SQL（不存在 nativeExecSql），只能操作固定结构的 probe 表；
//   - 不返回数据库句柄、不返回 key。
// 正式的 6 个数据库接口（nativeOpenAccountDatabase / nativeCloseAccountDatabase /
// nativeSubmitMigrationBatch / nativeFinishMigration / nativeGetMigrationState /
// nativeRunDatabaseSelfTest）在 S6 追加到本文件。

#include <jni.h>

#include <sqlite3.h>

#include <android/log.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "client_core/storage/CipherDatabase.h"
#include "client_core/storage/CipherParams.h"
#include "client_core/storage/MigrationImporter.h"
#include "client_core/storage/NativeDatabase.h"
#include "client_core/storage/NativeRepository.h"
#include "client_core/search/SearchService.h"

#include "native_sdk_handle.h"
#include "jni_db_key_bridge.h"

extern "C" JavaVM* jtGlobalJavaVm(); // 定义于 im_core_jni.cpp（g_vm 为内部链接，经此访问）

using im::storage::CipherDatabase;
using im::storage::CipherError;
using im::storage::CipherParams;
using im::storage::toString;
using im::storage::NativeDatabase;
using im::storage::DbStatus;
using im::storage::MigrationMessage;
using im::storage::MigrationConversation;
using im::storage::LegacyDeltaChange;
using im::storage::MigrationImporter;
using im::storage::MigrationSummary;
using im::storage::MigrationState;
using im::storage::MigrationStateSnapshot;
using im::storage::MigrationSubmitOutcome;
using im::storage::SelfTestOutcome;
using im::storage::CommandResult;
using im::storage::DbCommandQueue;

namespace {

class JByteArrayRead final {
public:
    JByteArrayRead(JNIEnv* env, jbyteArray array)
        : env_(env), array_(array), data_(env->GetByteArrayElements(array, nullptr)) {}
    ~JByteArrayRead()
    {
        if (data_) env_->ReleaseByteArrayElements(array_, data_, JNI_ABORT);
    }
    jbyte* get() const { return data_; }
private:
    JNIEnv* env_;
    jbyteArray array_;
    jbyte* data_;
};

std::string jstr(JNIEnv* env, jstring s)
{
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return r;
}

bool copyKey(JNIEnv* env, jbyteArray arr, std::vector<unsigned char>& out)
{
    if (!arr) return false;
    const jsize n = env->GetArrayLength(arr);
    if (n != static_cast<jsize>(CipherParams::kRawKeyBytes)) return false;
    out.resize(static_cast<std::size_t>(n));
    jbyte* buf = env->GetByteArrayElements(arr, nullptr);
    if (!buf) return false;
    for (jsize i = 0; i < n; ++i) out[i] = static_cast<unsigned char>(buf[i]);
    // 清零 JNI 侧副本（key 不在 native 长期驻留）
    for (jsize i = 0; i < n; ++i) buf[i] = 0;
    env->ReleaseByteArrayElements(arr, buf, JNI_ABORT);
    return true;
}

bool execSqlite(sqlite3* db, const std::string& sql, std::string& errOut)
{
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        errOut = err ? err : sqlite3_errmsg(db);
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

jstring report(JNIEnv* env, const std::string& s) { return env->NewStringUTF(s.c_str()); }

const char* kProbeTable = "native_probe";
const char* kProbeMarker = "NATIVE_FIXTURE_MARKER_3E7B";

// 单个迁移批次字节上限（对应 MigrationExporter 每批 200~500 条、含正文/媒体元数据）。
// 在 GetByteArrayElements 分配/拷贝之前先拒绝超大输入，避免内存放大与恶意超大数组。
constexpr jsize kMaxBatchBytes = 8 * 1024 * 1024; // 8 MiB

// ---------------- 批次二进制解码 ----------------
// 布局（小端）：
//   header: magic:u32 + version:u32 + count:u32
//   随后每条：
//   msgId(len:u32+bytes), conversationId:i64, peerId:i64, seq:i64, serverTime:i64,
//   localOrder:i64, fromMe:i32, type:i32, content(len+bytes), status:i32,
//   pinyin(len+bytes), initials(len+bytes), mediaPath(len+bytes),
//   imgW:i32, imgH:i32, fileId(len+bytes), fileName(len+bytes), fileSize:i64,
//   contentType(len+bytes), sha256(len+bytes),
//   thumbnailFileId, thumbnailPath, thumbnailSize:i64, thumbnailSha256,
//   thumbnailW:i32, thumbnailH:i32,
//   largeThumbnailFileId, largeThumbnailPath, largeThumbnailSize:i64, largeThumbnailSha256,
//   largeThumbnailW:i32, largeThumbnailH:i32,
//   localPath(len+bytes), transferred:i64
constexpr std::uint32_t kBatchMagic = 0x4A544D47u; // "JTMG"
constexpr std::uint32_t kBatchVersion = 1u;
constexpr std::uint32_t kMaxBatchMessages = 500u;
constexpr std::uint32_t kMaxStringBytes = 1024u * 1024u; // 单字符串字段上限 1 MiB

class ByteReader {
public:
    ByteReader(const unsigned char* d, std::size_t n) : m_d(d), m_n(n) {}

    // 无符号解码，再按位转换得到有符号值（避免有符号左移 UB，F11）
    bool u32(std::uint32_t& v)
    {
        if (m_pos + 4 > m_n) return false;
        std::uint32_t acc = 0;
        for (int i = 0; i < 4; ++i)
            acc |= static_cast<std::uint32_t>(m_d[m_pos + i]) << (8 * i);
        v = acc;
        m_pos += 4;
        return true;
    }
    bool u64(std::uint64_t& v)
    {
        if (m_pos + 8 > m_n) return false;
        std::uint64_t acc = 0;
        for (int i = 0; i < 8; ++i)
            acc |= static_cast<std::uint64_t>(m_d[m_pos + i]) << (8 * i);
        v = acc;
        m_pos += 8;
        return true;
    }
    bool i32(int& v)
    {
        std::uint32_t u = 0;
        if (!u32(u)) return false;
        static_assert(sizeof(int) == sizeof(std::uint32_t), "int must be 32-bit");
        std::memcpy(&v, &u, sizeof(v));
        return true;
    }
    bool i64(std::int64_t& v)
    {
        std::uint64_t u = 0;
        if (!u64(u)) return false;
        static_assert(sizeof(std::int64_t) == sizeof(std::uint64_t), "int64 must be 64-bit");
        std::memcpy(&v, &u, sizeof(v));
        return true;
    }
    bool str(std::string& out)
    {
        std::uint32_t len = 0;
        if (!u32(len)) return false;
        if (len > kMaxStringBytes) return false; // 单字段上限
        if (m_pos + static_cast<std::size_t>(len) > m_n) return false;
        out.assign(reinterpret_cast<const char*>(m_d + m_pos), static_cast<std::size_t>(len));
        m_pos += static_cast<std::size_t>(len);
        return true;
    }
    bool eof() const { return m_pos == m_n; }

private:
    const unsigned char* m_d;
    std::size_t m_n;
    std::size_t m_pos = 0;
};

bool decodeMessage(ByteReader& r, MigrationMessage& m)
{
    return r.str(m.msgId) && r.i64(m.conversationId) && r.i64(m.peerId) &&
           r.i64(m.conversationSeq) && r.i64(m.serverTime) && r.i64(m.localOrder) &&
           r.i32(m.fromMe) && r.i32(m.type) && r.str(m.content) && r.i32(m.status) &&
           r.str(m.pinyin) && r.str(m.initials) && r.str(m.mediaPath) &&
           r.i32(m.imgW) && r.i32(m.imgH) && r.str(m.fileId) && r.str(m.fileName) &&
           r.i64(m.fileSize) && r.str(m.contentType) && r.str(m.sha256) &&
           r.str(m.thumbnailFileId) && r.str(m.thumbnailPath) && r.i64(m.thumbnailSize) &&
           r.str(m.thumbnailSha256) && r.i32(m.thumbnailW) && r.i32(m.thumbnailH) &&
           r.str(m.largeThumbnailFileId) && r.str(m.largeThumbnailPath) &&
           r.i64(m.largeThumbnailSize) && r.str(m.largeThumbnailSha256) &&
           r.i32(m.largeThumbnailW) && r.i32(m.largeThumbnailH) &&
           r.str(m.localPath) && r.i64(m.transferred);
}

bool decodeBatch(jbyte* data, jsize len, std::vector<MigrationMessage>& out, std::string& err)
{
    if (!data || len <= 0) { err = "空批次"; return false; }
    ByteReader r(reinterpret_cast<const unsigned char*>(data), static_cast<std::size_t>(len));

    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t count = 0;
    if (!r.u32(magic) || magic != kBatchMagic) { err = "批次魔数非法"; return false; }
    if (!r.u32(version) || version != kBatchVersion) { err = "批次版本不支持"; return false; }
    if (!r.u32(count) || count > kMaxBatchMessages) { err = "批次行数非法或超限"; return false; }

    out.reserve(static_cast<std::size_t>(count));
    for (std::uint32_t i = 0; i < count; ++i) {
        MigrationMessage m;
        if (!decodeMessage(r, m)) {
            err = "批次解码失败（第 " + std::to_string(i) + " 条）";
            return false;
        }
        out.push_back(std::move(m));
    }
    if (!r.eof()) { err = "批次存在尾随垃圾"; return false; }
    return true;
}

// 会话流解码：conversationId:i64, peerId:i64, lastMsg(str), lastTs:i64, unread:i64
bool decodeConversation(ByteReader& r, MigrationConversation& c)
{
    return r.i64(c.conversationId) && r.i64(c.peerId) && r.str(c.lastMsg) &&
           r.i64(c.lastTs) && r.i64(c.unread);
}

bool decodeConversationBatch(jbyte* data, jsize len, std::vector<MigrationConversation>& out,
                             std::string& err)
{
    if (!data || len <= 0) { err = "空批次"; return false; }
    ByteReader r(reinterpret_cast<const unsigned char*>(data), static_cast<std::size_t>(len));

    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t count = 0;
    if (!r.u32(magic) || magic != kBatchMagic) { err = "批次魔数非法"; return false; }
    if (!r.u32(version) || version != kBatchVersion) { err = "批次版本不支持"; return false; }
    if (!r.u32(count) || count > kMaxBatchMessages) { err = "批次行数非法或超限"; return false; }

    out.reserve(static_cast<std::size_t>(count));
    for (std::uint32_t i = 0; i < count; ++i) {
        MigrationConversation c;
        if (!decodeConversation(r, c)) {
            err = "会话批次解码失败（第 " + std::to_string(i) + " 条）";
            return false;
        }
        out.push_back(std::move(c));
    }
    if (!r.eof()) { err = "批次存在尾随垃圾"; return false; }
    return true;
}

bool decodeLegacyDeltaBatch(jbyte* data, jsize len, std::vector<LegacyDeltaChange>& out,
                            std::string& err)
{
    if (!data || len <= 0) { err = "空 delta 批次"; return false; }
    ByteReader r(reinterpret_cast<const unsigned char*>(data), static_cast<std::size_t>(len));
    std::uint32_t magic=0, version=0, count=0;
    if (!r.u32(magic) || magic != 0x4A54444Cu) { err = "delta 魔数非法"; return false; }
    if (!r.u32(version) || version != 1u) { err = "delta 版本不支持"; return false; }
    if (!r.u32(count) || count > kMaxBatchMessages) { err = "delta 行数非法或超限"; return false; }
    out.reserve(count);
    for (std::uint32_t i=0; i<count; ++i) {
        LegacyDeltaChange c; int owner=0;
        if (!r.i64(c.changeSeq) || !r.i32(owner) || !r.str(c.entityType) ||
            !r.str(c.entityKey) || !r.str(c.operation) || !r.i64(c.changedAt) ||
            !r.i32(c.payloadVersion) || !r.str(c.payload)) {
            err = "delta 解码失败（第 " + std::to_string(i) + " 条）"; return false;
        }
        c.ownerId = owner;
        out.push_back(std::move(c));
    }
    if (!r.eof()) { err = "delta 批次存在尾随垃圾"; return false; }
    return true;
}

} // namespace

extern "C" {

// String nativeCipherCreateFixtureForTest(String path, byte[] key32)
// 创建（或覆盖）一个加密 fixture 库，内含 native_probe 表与一行固定 marker。
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeCipherCreateFixtureForTest(JNIEnv* env, jclass,
                                                                        jstring path, jbyteArray key)
{
    const std::string pathStr = jstr(env, path);
    std::vector<unsigned char> keyVec;
    if (!copyKey(env, key, keyVec)) {
        return report(env, "err|BadKeyLength|key 必须是 32 字节");
    }
    // 测试夹具：先删除旧文件，保证可重复执行
    std::remove(pathStr.c_str());
    std::remove((pathStr + "-wal").c_str());
    std::remove((pathStr + "-shm").c_str());

    CipherDatabase db;
    const CipherError e = db.open(pathStr, keyVec);
    if (e != CipherError::Ok) {
        return report(env, std::string("err|") + toString(e) + "|" + db.lastMessage());
    }
    std::string err;
    const std::string createSql = std::string("CREATE TABLE ") + kProbeTable + "(marker TEXT)";
    if (!execSqlite(db.nativeHandle(), createSql, err)) {
        return report(env, std::string("err|Internal|") + err);
    }
    const std::string insertSql =
        std::string("INSERT INTO ") + kProbeTable + "(marker) VALUES('" + kProbeMarker + "')";
    if (!execSqlite(db.nativeHandle(), insertSql, err)) {
        return report(env, std::string("err|Internal|") + err);
    }
    const std::string version = db.cipherVersion();
    db.close();
    return report(env, std::string("ok|cipher=") + version + "|marker=" + kProbeMarker);
}

// String nativeCipherVerifyFixtureForTest(String path, byte[] key32)
// 只读方式打开既有加密库，回报 cipher 版本、表数量与是否能看到 native_probe 的 marker。
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeCipherVerifyFixtureForTest(JNIEnv* env, jclass,
                                                                        jstring path, jbyteArray key)
{
    const std::string pathStr = jstr(env, path);
    std::vector<unsigned char> keyVec;
    if (!copyKey(env, key, keyVec)) {
        return report(env, "err|BadKeyLength|key 必须是 32 字节");
    }

    CipherDatabase db;
    const CipherError e = db.open(pathStr, keyVec);
    if (e != CipherError::Ok) {
        return report(env, std::string("err|") + toString(e) + "|" + db.lastMessage());
    }
    const std::string version = db.cipherVersion();

    sqlite3* h = db.nativeHandle();
    int tableCount = 0;
    std::string marker;
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT count(*) FROM sqlite_master WHERE type='table'";
        if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
            if (sqlite3_step(stmt) == SQLITE_ROW) tableCount = sqlite3_column_int(stmt, 0);
        }
        if (stmt) sqlite3_finalize(stmt);
    }
    {
        sqlite3_stmt* stmt = nullptr;
        const std::string sql = std::string("SELECT marker FROM ") + kProbeTable + " LIMIT 1";
        if (sqlite3_prepare_v2(h, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK && stmt) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char* t = sqlite3_column_text(stmt, 0);
                if (t) marker = reinterpret_cast<const char*>(t);
            }
        }
        if (stmt) sqlite3_finalize(stmt);
    }
    db.close();

    char buf[512];
    std::snprintf(buf, sizeof(buf), "ok|cipher=%s|tables=%d|marker=%s", version.c_str(),
                  tableCount, marker.empty() ? "-" : marker.c_str());
    return report(env, std::string(buf));
}

// ---------------- P6-T05/S6：正式数据库生命周期与影子迁移 ----------------
// key 不再由调用方直接传，而是经 platformBridge（Kotlin NativeDbKeyPlatform）从
// DbKeyManager 解出。Token-only 冷启动无 passHash → 桥返回 null → NativeDatabase 保持
// Locked，绝不降级、绝不拿 Token 派生 key。数据库由 NativeSdkHandle 持有（不透明句柄），
// 不暴露 sqlite3*、key 或任意 SQL。

namespace {

// 在锁内取句柄上的 db + ownerId 副本，供迁移/自检使用；返回是否可用。
bool acquireDb(jlong handle, std::shared_ptr<NativeDatabase>& db, std::int64_t& ownerId)
{
    auto h = jt::lookupHandle(handle);
    if (!h) return false;
    std::lock_guard<std::mutex> lk(h->mutex);
    if (h->isDestroying()) return false;
    db = h->db;
    ownerId = h->dbOwnerId;
    return db && db->status() == DbStatus::Ready && ownerId > 0;
}

// ---- cutover DIRTY → 带 MAC KV 镜像的桥（P7-G8） ----
// NativeDatabase 的 transaction hook 在首次业务事务中原子推进 DIRTY 后，
// 经此回调 Kotlin 写 CutoverMirrorStore。回调可能发生在非 JVM 附着线程
// （如内核网络线程驱动的来消息落库），必须按需 AttachCurrentThread。
// jclass/jmethodID 在 nativeOpenAccountDatabase（Java 线程）缓存为全局引用，
// 避免在 native 附着线程上 FindClass 找不到应用类。

std::mutex g_dirtyBridgeMutex;
jclass g_nativeBindingsClass = nullptr;      // global ref
jmethodID g_onCutoverDirtyMethod = nullptr;  // static (JJIJI... )V

bool cacheCutoverDirtyBridge(JNIEnv* env)
{
    std::lock_guard<std::mutex> lk(g_dirtyBridgeMutex);
    if (g_nativeBindingsClass && g_onCutoverDirtyMethod) return true;
    jclass local = env->FindClass("com/jitong/im/core/NativeBindings");
    if (!local) return false;
    g_nativeBindingsClass = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    if (!g_nativeBindingsClass) return false;
    // (ownerId:J, epoch:J, state:I, highWater:J, schemaVersion:I, keyId:String, summary:String, updatedAt:J)V
    g_onCutoverDirtyMethod = env->GetStaticMethodID(
        g_nativeBindingsClass, "onCutoverDirtyFromNative",
        "(JJIJILjava/lang/String;Ljava/lang/String;J)V");
    if (!g_onCutoverDirtyMethod) {
        env->DeleteGlobalRef(g_nativeBindingsClass);
        g_nativeBindingsClass = nullptr;
        return false;
    }
    return true;
}

void notifyCutoverDirty(std::int64_t ownerId, const im::storage::CutoverJournalSnapshot& s)
{
    JavaVM* vm = jtGlobalJavaVm();
    if (!vm) return;
    JNIEnv* env = nullptr;
    bool detach = false;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
        detach = true;
    }
    {
        std::lock_guard<std::mutex> lk(g_dirtyBridgeMutex);
        if (g_nativeBindingsClass && g_onCutoverDirtyMethod) {
            jstring keyId = env->NewStringUTF(s.keyId.c_str());
            jstring summary = env->NewStringUTF(s.summary.c_str());
            env->CallStaticVoidMethod(g_nativeBindingsClass, g_onCutoverDirtyMethod,
                                      static_cast<jlong>(ownerId), static_cast<jlong>(s.epoch),
                                      static_cast<jint>(s.state), static_cast<jlong>(s.highWater),
                                      static_cast<jint>(s.schemaVersion), keyId, summary,
                                      static_cast<jlong>(s.updatedAt));
            if (keyId) env->DeleteLocalRef(keyId);
            if (summary) env->DeleteLocalRef(summary);
            if (env->ExceptionCheck()) {
                env->ExceptionDescribe();
                env->ExceptionClear();
            }
        }
    }
    if (detach) vm->DetachCurrentThread();
}

// 搜索结果二进制（小端）：magic:u32, version:u32, count:u32，随后每项：
// msgId:string, conversationId:i64, peerId:i64, ts:i64, snippet:string,
// highlightUnit:i32, rangeCount:u32, ranges[start:i32,end:i32]。
// 不使用分隔符文本，避免正文中的换行/竖线破坏协议边界。
constexpr std::uint32_t kSearchMagic = 0x4A545352u; // "JTSR"
constexpr std::uint32_t kSearchVersion = 1u;

class ByteWriter {
public:
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xff));
    }
    void i32(std::int32_t v) { std::uint32_t u = 0; std::memcpy(&u, &v, sizeof(u)); u32(u); }
    void i64(std::int64_t v) {
        std::uint64_t u = 0; std::memcpy(&u, &v, sizeof(u));
        for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<unsigned char>((u >> (8 * i)) & 0xff));
    }
    bool str(const std::string& value) {
        if (value.size() > kMaxStringBytes) return false;
        u32(static_cast<std::uint32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
        return true;
    }
    std::vector<unsigned char> bytes;
};

bool writeHistoryMessage(ByteWriter& w, const im::dto::MessageDto& m) {
    w.i64(m.ownerId); if(!w.str(m.msgId))return false; w.i64(m.conversationId); w.i64(m.peerId);
    w.i64(m.seq); w.i64(m.ts); w.i64(m.localOrder); w.i32(m.fromMe?1:0); w.i32(m.type);
    if(!w.str(m.content))return false; w.i32(m.status);
    if(!w.str(m.pinyin)||!w.str(m.initials)||!w.str(m.mediaPath))return false;
    w.i32(m.imgW); w.i32(m.imgH);
    if(!w.str(m.fileId)||!w.str(m.fileName))return false; w.i64(m.fileSize);
    if(!w.str(m.contentType)||!w.str(m.sha256)||!w.str(m.thumbnailFileId)||
       !w.str(m.thumbnailPath))return false;
    w.i64(m.thumbnailSize); if(!w.str(m.thumbnailSha256))return false;
    w.i32(m.thumbnailW); w.i32(m.thumbnailH);
    if(!w.str(m.largeThumbnailFileId)||!w.str(m.largeThumbnailPath))return false;
    w.i64(m.largeThumbnailSize); if(!w.str(m.largeThumbnailSha256))return false;
    w.i32(m.largeThumbnailW); w.i32(m.largeThumbnailH);
    if(!w.str(m.localPath))return false; w.i64(m.transferred); return true;
}

} // namespace

// boolean nativeOpenAccountDatabase(long handle, int ownerId, String filesDir, Object bridge)
JNIEXPORT jboolean JNICALL
Java_com_jitong_im_core_NativeBindings_nativeOpenAccountDatabase(JNIEnv* env, jclass, jlong handle,
                                                                 jint ownerId, jstring filesDir,
                                                                 jobject bridge)
{
    auto h = jt::lookupHandle(handle);
    if (!h || h->isDestroying()) return JNI_FALSE;
    if (ownerId <= 0) return JNI_FALSE;

    auto keyBridge = std::make_shared<jt::JniDbKeyBridge>(jtGlobalJavaVm(), env, bridge);
    if (!keyBridge->valid()) return JNI_FALSE;

    const std::string dir = jstr(env, filesDir);
    auto db = std::make_shared<NativeDatabase>();
    std::string err;
    if (!db->openWithBridge(dir, static_cast<std::int64_t>(ownerId), *keyBridge, &err)) {
        // 错误信息不含 key/正文，仅用于诊断
        __android_log_print(ANDROID_LOG_WARN, "JitongKernel",
                            "nativeOpenAccountDatabase(%d): %s（状态=%s）", ownerId,
                            err.c_str(), toString(db->status()));
        return JNI_FALSE;
    }

    // open 成功后才挂到句柄；挂载与销毁互斥，避免 use-after-destroy。
    std::shared_ptr<NativeDatabase> previous;
    bool attached = false;
    {
        std::lock_guard<std::mutex> lk(h->mutex);
        if (!h->isDestroying() && (!h->runtime || h->runtime->setDatabase(db))) {
            previous = std::move(h->db);
            h->db = db;
            h->dbOwnerId = static_cast<std::int64_t>(ownerId);
            h->dbKeyBridge = std::move(keyBridge);
            attached = true;
        }
    }
    if (!attached) { db->close(); return JNI_FALSE; }

    // cutover DIRTY 镜像桥：transaction hook 推进 DIRTY 后通知 Kotlin 写带 MAC 镜像。
    // 桥不可用时仍打开库（镜像失败最坏 fail-close 为 Repair，绝不回退 Room），但记日志。
    if (cacheCutoverDirtyBridge(env)) {
        const std::int64_t owner = static_cast<std::int64_t>(ownerId);
        db->setCutoverDirtyListener([owner](const im::storage::CutoverJournalSnapshot& s) {
            notifyCutoverDirty(owner, s);
        });
    } else {
        __android_log_print(ANDROID_LOG_WARN, "JitongKernel",
                            "cutover dirty bridge 不可用，DIRTY 镜像只能依赖冷启动 fail-close");
    }
    // 旧库可能 drain Writer；必须在 handle 锁外关闭。
    if (previous && previous != db) previous->close();
    return JNI_TRUE;
}

// void nativeCloseAccountDatabase(long handle)
JNIEXPORT void JNICALL
Java_com_jitong_im_core_NativeBindings_nativeCloseAccountDatabase(JNIEnv*, jclass, jlong handle)
{
    auto h = jt::lookupHandle(handle);
    if (!h) return;
    std::shared_ptr<NativeDatabase> db;
    std::shared_ptr<im::runtime::ClientRuntime> runtime;
    {
        std::lock_guard<std::mutex> lk(h->mutex);
        db = std::move(h->db);
        runtime = h->runtime;
        h->dbOwnerId = 0;
        h->dbKeyBridge.reset();
    }
    // 显式关库意味着停止依赖该库的业务服务并解除 Runtime 引用。
    if (runtime) {
        runtime->stop();
        runtime->setDatabase(nullptr);
    }
    if (db) db->close();
}

// boolean nativeBeginMigration(long handle)
// 开始影子导入：Disabled → ShadowImport；已 Verified 时返回 false 拒绝重复迁移。
JNIEXPORT jboolean JNICALL
Java_com_jitong_im_core_NativeBindings_nativeBeginMigration(JNIEnv* env, jclass, jlong handle)
{
    std::shared_ptr<NativeDatabase> db;
    std::int64_t ownerId = 0;
    if (!acquireDb(handle, db, ownerId)) return JNI_FALSE;
    std::string err;
    if (!db->beginMigration(ownerId, &err)) {
        __android_log_print(ANDROID_LOG_WARN, "JitongKernel", "nativeBeginMigration: %s", err.c_str());
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

// boolean nativeResetMigration(long handle)
// 回滚到 ShadowImport：清完成标记与 checkpoint（不删生产数据）。
JNIEXPORT jboolean JNICALL
Java_com_jitong_im_core_NativeBindings_nativeResetMigration(JNIEnv* env, jclass, jlong handle)
{
    std::shared_ptr<NativeDatabase> db;
    std::int64_t ownerId = 0;
    if (!acquireDb(handle, db, ownerId)) return JNI_FALSE;
    std::string err;
    if (!db->resetMigration(ownerId, &err)) {
        __android_log_print(ANDROID_LOG_WARN, "JitongKernel", "nativeResetMigration: %s", err.c_str());
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

// String nativeSubmitMigrationBatch(long handle, byte[] batch, String checkpoint)
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeSubmitMigrationBatch(JNIEnv* env, jclass, jlong handle,
                                                                  jbyteArray batch, jstring checkpoint)
{
    std::shared_ptr<NativeDatabase> db;
    std::int64_t ownerId = 0;
    if (!acquireDb(handle, db, ownerId)) return report(env, "err|NotOpen|库未打开或句柄无效");

    // 分配/拷贝之前先做大小校验：拒绝超大/非法输入（对应 24.8 约束）。
    if (!batch) return report(env, "err|BadData|批次为空");
    const jsize len = env->GetArrayLength(batch);
    if (len <= 0 || len > kMaxBatchBytes) {
        return report(env, "err|BadData|批次大小非法或超限");
    }

    try {
        std::vector<MigrationMessage> msgs;
        std::string decErr;
        {
            JByteArrayRead bytes(env, batch);
            if (!bytes.get()) return report(env, "err|Internal|批次读取失败");
            if (!decodeBatch(bytes.get(), len, msgs, decErr)) {
                return report(env, std::string("err|BadData|") + decErr);
            }
        }
        for (auto& m : msgs) m.ownerId = ownerId;
        const std::string cp = jstr(env, checkpoint);
        const MigrationSubmitOutcome out = db->submitMigrationBatch(ownerId, msgs, cp);
        if (out.timedOut) return report(env, std::string("err|TimedOutButMayCommit|") + out.error);
        if (!out.ok) return report(env, std::string("err|") + toString(out.command) +
                                         "|批次执行失败（已回滚）" + out.error);
        return report(env, std::string("ok|imported=") + std::to_string(out.imported));
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, "JitongKernel", "message batch exception: %s", e.what());
        return report(env, "err|Internal|Native 批次处理异常");
    } catch (...) {
        return report(env, "err|Internal|Native 批次处理异常");
    }
}

// String nativeSubmitConversationBatch(long handle, byte[] batch, String checkpoint)
// 会话流独立提交（unread/lastMsg/lastTs 权威覆盖，独立 conversations_checkpoint）。
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeSubmitConversationBatch(JNIEnv* env, jclass,
                                                                     jlong handle,
                                                                     jbyteArray batch,
                                                                     jstring checkpoint)
{
    std::shared_ptr<NativeDatabase> db;
    std::int64_t ownerId = 0;
    if (!acquireDb(handle, db, ownerId)) return report(env, "err|NotOpen|库未打开或句柄无效");

    if (!batch) return report(env, "err|BadData|批次为空");
    const jsize len = env->GetArrayLength(batch);
    if (len <= 0 || len > kMaxBatchBytes) {
        return report(env, "err|BadData|批次大小非法或超限");
    }

    try {
        std::vector<MigrationConversation> convs;
        std::string decErr;
        {
            JByteArrayRead bytes(env, batch);
            if (!bytes.get()) return report(env, "err|Internal|批次读取失败");
            if (!decodeConversationBatch(bytes.get(), len, convs, decErr)) {
                return report(env, std::string("err|BadData|") + decErr);
            }
        }
        for (auto& c : convs) c.ownerId = ownerId;
        const std::string cp = jstr(env, checkpoint);
        const MigrationSubmitOutcome out = db->submitConversationBatch(ownerId, convs, cp);
        if (out.timedOut) return report(env, std::string("err|TimedOutButMayCommit|") + out.error);
        if (!out.ok) return report(env, std::string("err|") + toString(out.command) +
                                         "|会话批次执行失败（已回滚）" + out.error);
        return report(env, std::string("ok|imported=") + std::to_string(out.imported));
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, "JitongKernel", "conversation batch exception: %s", e.what());
        return report(env, "err|Internal|Native 会话批次处理异常");
    } catch (...) {
        return report(env, "err|Internal|Native 会话批次处理异常");
    }
}

// String nativeSubmitLegacyDeltaBatch(long handle, long epoch, long expectedAfter, byte[] batch)
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeSubmitLegacyDeltaBatch(JNIEnv* env, jclass,
    jlong handle, jlong epoch, jlong expectedAfter, jbyteArray batch)
{
    std::shared_ptr<NativeDatabase> db; std::int64_t ownerId=0;
    if (!acquireDb(handle, db, ownerId)) return report(env, "err|NotOpen|库未打开或句柄无效");
    if (!batch || epoch <= 0 || expectedAfter < 0) return report(env, "err|BadData|delta 参数非法");
    const jsize len=env->GetArrayLength(batch);
    if (len<=0 || len>kMaxBatchBytes) return report(env, "err|BadData|delta 批次大小非法或超限");
    try {
        std::vector<LegacyDeltaChange> changes; std::string error;
        {
            JByteArrayRead bytes(env,batch);
            if(!bytes.get() || !decodeLegacyDeltaBatch(bytes.get(),len,changes,error))
                return report(env,std::string("err|BadData|")+error);
        }
        const auto out=db->submitLegacyDeltaBatch(ownerId,epoch,expectedAfter,changes);
        if(out.timedOut)return report(env,std::string("err|TimedOutButMayCommit|")+out.error);
        if(!out.ok)return report(env,std::string("err|")+toString(out.command)+"|"+out.error);
        return report(env,"ok|checkpoint="+std::to_string(out.committedCheckpoint));
    } catch (...) {
        return report(env,"err|Internal|Native delta 处理异常");
    }
}

JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeGetLegacyDeltaCheckpoint(JNIEnv* env,jclass,
    jlong handle,jlong epoch)
{
    std::shared_ptr<NativeDatabase> db;std::int64_t ownerId=0;
    if(!acquireDb(handle,db,ownerId)||epoch<=0)return report(env,"err|NotOpen|库未打开或参数非法");
    std::int64_t checkpoint=0;bool queryOk=false;
    const auto rr=db->withRead([&](sqlite3*d){queryOk=MigrationImporter::readLegacyDeltaCheckpoint(d,epoch,&checkpoint);});
    if(rr!=im::storage::ReadResult::Ok||!queryOk)return report(env,"err|Internal|checkpoint 查询失败");
    return report(env,"ok|checkpoint="+std::to_string(checkpoint));
}

JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeSeedLegacyDeltaCheckpoint(JNIEnv* env,jclass,
    jlong handle,jlong epoch,jlong baseline,jlong updatedAt)
{
    std::shared_ptr<NativeDatabase> db;std::int64_t ownerId=0;
    if(!acquireDb(handle,db,ownerId))return report(env,"err|NotOpen|库未打开");
    std::string error;
    if(!db->seedLegacyDeltaCheckpoint(ownerId,epoch,baseline,updatedAt,&error))
        return report(env,"err|Rejected|"+error);
    return report(env,"ok|checkpoint="+std::to_string(baseline));
}

JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeGetCutoverJournal(JNIEnv* env,jclass,jlong handle)
{
    std::shared_ptr<NativeDatabase> db;std::int64_t ownerId=0;
    if(!acquireDb(handle,db,ownerId))return report(env,"err|NotOpen|库未打开");
    im::storage::CutoverJournalSnapshot s;
    if(!db->queryCutoverJournal(ownerId,&s))return report(env,"err|Internal|journal 查询失败");
    if(!s.present)return report(env,"ok|present=0");
    return report(env,"ok|present=1|epoch="+std::to_string(s.epoch)+
        "|state="+std::to_string(static_cast<int>(s.state))+
        "|highWater="+std::to_string(s.highWater)+
        "|schemaVersion="+std::to_string(s.schemaVersion)+
        "|keyId="+s.keyId+"|updatedAt="+std::to_string(s.updatedAt)+"|summary="+s.summary);
}

JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeAdvanceCutoverJournal(JNIEnv* env,jclass,
    jlong handle,jlong epoch,jint expectedState,jint targetState,jlong highWater,
    jint schemaVersion,jstring keyId,jstring summary,jlong updatedAt)
{
    std::shared_ptr<NativeDatabase> db;std::int64_t ownerId=0;
    if(!acquireDb(handle,db,ownerId))return report(env,"err|NotOpen|库未打开");
    if(targetState<0||targetState>3||expectedState< -1||expectedState>3)
        return report(env,"err|BadData|cutover 状态非法");
    std::string error;
    const bool ok=db->advanceCutoverJournal(ownerId,epoch,expectedState,
        static_cast<im::storage::CutoverJournalState>(targetState),highWater,schemaVersion,
        jstr(env,keyId),jstr(env,summary),updatedAt,&error);
    return report(env,ok?"ok":("err|Rejected|"+error));
}

// String nativeFinishMigration(long handle, long msgs, long convs, long minSeq, long maxSeq, long fts)
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeFinishMigration(JNIEnv* env, jclass, jlong handle,
                                                             jlong expMsgs, jlong expConvs,
                                                             jlong expMinSeq, jlong expMaxSeq,
                                                             jlong expFts)
{
    std::shared_ptr<NativeDatabase> db;
    std::int64_t ownerId = 0;
    if (!acquireDb(handle, db, ownerId)) return report(env, "err|NotOpen|库未打开或句柄无效");

    MigrationSummary expected;
    expected.messageCount = expMsgs;
    expected.conversationCount = expConvs;
    expected.minSeq = expMinSeq;
    expected.maxSeq = expMaxSeq;
    expected.ftsCount = expFts;

    std::string err;
    MigrationSummary actual;
    if (!db->finishMigration(ownerId, expected, &actual, &err)) {
        return report(env, std::string("err|VerifyFailed|") + err);
    }
    return report(env, std::string("ok|") + toString(actual));
}

// String nativeGetMigrationState(long handle)
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeGetMigrationState(JNIEnv* env, jclass, jlong handle)
{
    std::shared_ptr<NativeDatabase> db;
    std::int64_t ownerId = 0;
    if (!acquireDb(handle, db, ownerId)) return report(env, "err|NotOpen|库未打开或句柄无效");

    MigrationStateSnapshot snap;
    if (!db->queryMigrationState(ownerId, &snap)) {
        return report(env, "err|NotOpen|查询状态失败");
    }
    return report(env, std::string("ok|completed=") + (snap.completed ? "1" : "0") +
                          "|state=" + toString(snap.state) +
                          "|checkpoint=" + snap.checkpoint + "|" + toString(snap.summary));
}

// String nativeRunDatabaseSelfTest(long handle)
JNIEXPORT jstring JNICALL
Java_com_jitong_im_core_NativeBindings_nativeRunDatabaseSelfTest(JNIEnv* env, jclass, jlong handle)
{
    std::shared_ptr<NativeDatabase> db;
    std::int64_t ownerId = 0;
    if (!acquireDb(handle, db, ownerId)) {
        // 区分「未打开」与「句柄无效」；未打开也给出可读状态
        auto h = jt::lookupHandle(handle);
        if (!h) return report(env, "err|NotOpen|句柄不存在");
        std::shared_ptr<NativeDatabase> d;
        {
            std::lock_guard<std::mutex> lk(h->mutex);
            d = h->db;
        }
        return report(env, std::string("err|Locked|数据库未就绪（状态=") +
                              toString(d ? d->status() : DbStatus::Closed) + "）");
    }

    const SelfTestOutcome out = db->runSelfTest(ownerId);
    if (!out.ok) {
        return report(env, std::string("err|NotOpen|") + out.error);
    }
    return report(env, std::string("ok|status=Ready|cipher=") + out.cipher +
                          "|schema=" + std::to_string(out.schemaVersion) +
                          "|completed=" + (out.completed ? "1" : "0") + "|" + toString(out.summary));
}

// byte[] nativeSearchMessages(long handle, long conversationId, String keyword, int limit)
JNIEXPORT jbyteArray JNICALL
Java_com_jitong_im_core_NativeBindings_nativeSearchMessages(JNIEnv* env, jclass, jlong handle,
                                                             jlong conversationId, jstring keyword,
                                                             jint limit)
{
    std::shared_ptr<NativeDatabase> db;
    std::int64_t ownerId = 0;
    if (!acquireDb(handle, db, ownerId) || !keyword || conversationId < 0 || limit <= 0 || limit > 100)
        return nullptr;

    im::search::SearchQuery query;
    query.ownerId = ownerId;
    query.conversationId = static_cast<std::int64_t>(conversationId);
    query.keyword = jstr(env, keyword);
    query.limit = limit;
    std::vector<im::dto::SearchHit> hits;
    std::string error;
    im::search::SearchService service(db.get());
    if (!service.search(query, &hits, &error)) return nullptr;

    ByteWriter writer;
    writer.u32(kSearchMagic);
    writer.u32(kSearchVersion);
    writer.u32(static_cast<std::uint32_t>(hits.size()));
    for (const auto& hit : hits) {
        if (!writer.str(hit.msgId)) return nullptr;
        writer.i64(hit.conversationId);
        writer.i64(hit.peerId);
        writer.i64(hit.ts);
        if (!writer.str(hit.snippet)) return nullptr;
        writer.i32(static_cast<std::int32_t>(hit.highlightUnit));
        writer.u32(static_cast<std::uint32_t>(hit.highlightRanges.size()));
        for (const auto& range : hit.highlightRanges) {
            writer.i32(range.first);
            writer.i32(range.second);
        }
    }
    if (writer.bytes.size() > static_cast<std::size_t>(std::numeric_limits<jsize>::max())) return nullptr;
    auto result = env->NewByteArray(static_cast<jsize>(writer.bytes.size()));
    if (!result) return nullptr;
    env->SetByteArrayRegion(result, 0, static_cast<jsize>(writer.bytes.size()),
                            reinterpret_cast<const jbyte*>(writer.bytes.data()));
    return env->ExceptionCheck() ? nullptr : result;
}

// byte[] nativeLoadHistory(handle, conversationId, limit, hasCursor, cursor fields...)
JNIEXPORT jbyteArray JNICALL
Java_com_jitong_im_core_NativeBindings_nativeLoadHistory(JNIEnv* env, jclass, jlong handle,
    jlong conversationId, jint limit, jboolean hasCursor, jlong cursorTime, jlong cursorSeq,
    jlong cursorOrder, jstring cursorMsgId)
{
    std::shared_ptr<NativeDatabase> db; std::int64_t ownerId=0;
    if(!acquireDb(handle,db,ownerId)||conversationId<=0||limit<=0||limit>200)return nullptr;
    im::storage::MessageCursor cursor;
    const im::storage::MessageCursor* cursorPtr=nullptr;
    if(hasCursor==JNI_TRUE){
        if(!cursorMsgId)return nullptr;
        cursor={static_cast<std::int64_t>(cursorTime),static_cast<std::int64_t>(cursorSeq),
                static_cast<std::int64_t>(cursorOrder),jstr(env,cursorMsgId)};
        if(cursor.msgId.empty())return nullptr; cursorPtr=&cursor;
    }
    im::storage::NativeRepository repository(db.get()); im::storage::MessagePage page;
    std::string error;
    if(!repository.listConversation(ownerId,conversationId,cursorPtr,limit,&page,&error))return nullptr;
    ByteWriter writer; writer.u32(0x4A544850u); writer.u32(1u); // "JTHP", v1
    writer.i32(page.hasMore?1:0); writer.u32(static_cast<std::uint32_t>(page.messages.size()));
    for(const auto& message:page.messages)if(!writeHistoryMessage(writer,message))return nullptr;
    if(writer.bytes.size()>static_cast<std::size_t>(std::numeric_limits<jsize>::max()))return nullptr;
    auto result=env->NewByteArray(static_cast<jsize>(writer.bytes.size())); if(!result)return nullptr;
    env->SetByteArrayRegion(result,0,static_cast<jsize>(writer.bytes.size()),
        reinterpret_cast<const jbyte*>(writer.bytes.data()));
    return env->ExceptionCheck()?nullptr:result;
}

JNIEXPORT jbyteArray JNICALL
Java_com_jitong_im_core_NativeBindings_nativeLoadConversations(JNIEnv* env,jclass,jlong handle)
{
    std::shared_ptr<NativeDatabase> db;std::int64_t ownerId=0;
    if(!acquireDb(handle,db,ownerId))return nullptr;
    im::storage::NativeRepository repository(db.get());
    std::vector<im::dto::ConversationDto> rows;std::string error;
    if(!repository.loadConversations(ownerId,&rows,&error))return nullptr;
    ByteWriter writer;writer.u32(0x4A54434Cu);writer.u32(1u); // JTCL v1
    writer.u32(static_cast<std::uint32_t>(rows.size()));
    for(const auto& row:rows){
        writer.i64(row.conversationId);writer.i64(row.ownerId);writer.i64(row.peerId);
        if(!writer.str(row.lastMsg))return nullptr;
        writer.i64(row.lastTs);writer.i64(row.unread);
    }
    if(writer.bytes.size()>static_cast<std::size_t>(std::numeric_limits<jsize>::max()))return nullptr;
    auto result=env->NewByteArray(static_cast<jsize>(writer.bytes.size()));if(!result)return nullptr;
    env->SetByteArrayRegion(result,0,static_cast<jsize>(writer.bytes.size()),
        reinterpret_cast<const jbyte*>(writer.bytes.data()));
    return env->ExceptionCheck()?nullptr:result;
}

JNIEXPORT jboolean JNICALL
Java_com_jitong_im_core_NativeBindings_nativeBeginDownloadTask(JNIEnv* env,jclass,jlong handle,
    jstring taskId,jstring msgId,jstring fileId,jstring localPath,jlong generation)
{
    std::shared_ptr<NativeDatabase> db;std::int64_t owner=0;
    if(!acquireDb(handle,db,owner)||generation<=0)return JNI_FALSE;
    auto h=jt::lookupHandle(handle);if(!h)return JNI_FALSE;
    std::shared_ptr<im::runtime::ClientRuntime> runtime;
    { std::lock_guard<std::mutex> lk(h->mutex);runtime=h->runtime; }
    if(!runtime||runtime->ownerId()!=owner||!runtime->isCurrentGeneration(generation))return JNI_FALSE;
    im::storage::NativeRepository repo(db);
    return repo.beginDownloadTask(owner,jstr(env,taskId),jstr(env,msgId),jstr(env,fileId),
        jstr(env,localPath),generation)?JNI_TRUE:JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_jitong_im_core_NativeBindings_nativeFinishDownloadTask(JNIEnv* env,jclass,jlong handle,
    jstring taskId,jlong generation,jint state,jlong transferred)
{
    std::shared_ptr<NativeDatabase> db;std::int64_t owner=0;
    if(!acquireDb(handle,db,owner))return JNI_FALSE;
    im::storage::NativeRepository repo(db);
    return repo.finishDownloadTask(owner,jstr(env,taskId),generation,state,transferred)
        ?JNI_TRUE:JNI_FALSE;
}

JNIEXPORT jbyteArray JNICALL
Java_com_jitong_im_core_NativeBindings_nativeListRecoverableDownloads(JNIEnv* env,jclass,jlong handle)
{
    std::shared_ptr<NativeDatabase> db;std::int64_t owner=0;
    if(!acquireDb(handle,db,owner))return nullptr;
    im::storage::NativeRepository repo(db);
    std::vector<im::storage::DownloadTaskRow> rows;
    if(!repo.listRecoverableDownloads(owner,&rows))return nullptr;
    ByteWriter writer;writer.u32(0x4A54444Cu);writer.u32(1u);writer.u32(static_cast<std::uint32_t>(rows.size()));
    for(const auto& row:rows){
        if(!writer.str(row.taskId)||!writer.str(row.msgId)||!writer.str(row.fileId)||
           !writer.str(row.localPath)||!writer.str(row.expectedSha256))return nullptr;
        writer.i64(row.totalSize);writer.i64(row.transferred);writer.i64(row.generation);
    }
    if(writer.bytes.size()>static_cast<std::size_t>(std::numeric_limits<jsize>::max()))return nullptr;
    auto result=env->NewByteArray(static_cast<jsize>(writer.bytes.size()));if(!result)return nullptr;
    env->SetByteArrayRegion(result,0,static_cast<jsize>(writer.bytes.size()),
        reinterpret_cast<const jbyte*>(writer.bytes.data()));
    return env->ExceptionCheck()?nullptr:result;
}

} // extern "C"
