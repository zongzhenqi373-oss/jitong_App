#include "client_core/storage/NativeDatabase.h"

#include <sqlite3.h>

#include <openssl/crypto.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <condition_variable>

#include "client_core/storage/CipherDatabase.h"
#include "client_core/storage/DatabasePaths.h"
#include "client_core/storage/DbCommandQueue.h"
#include "client_core/storage/ProcessLock.h"
#include "client_core/storage/ReadPool.h"
#include "client_core/storage/SchemaManager.h"

namespace im {
namespace storage {

namespace {

// 同步等待一个写命令完成。核心：completion state 放堆上（shared_ptr），回调只捕获
// shared_ptr 与值拷贝，绝不捕获 JNI/栈变量——超时返回后迟到回调不会触碰已销毁的栈（F03）。
struct SyncWriteResult {
    CommandResult command = CommandResult::NotOpen;
    bool timedOut = false;
};

SyncWriteResult runWriteSync(const std::shared_ptr<DbCommandQueue>& q,
                             const DbCommandQueue::WriteFn& fn,
                             std::chrono::milliseconds timeout)
{
    if (!q) return {CommandResult::NotOpen, false};

    struct State {
        CommandResult result = CommandResult::NotOpen;
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
    };
    auto state = std::make_shared<State>();

    DbCommandQueue::Request req;
    req.fn = fn;
    req.onDone = [state](CommandResult r) {
        std::lock_guard<std::mutex> lk(state->mutex);
        state->result = r;
        state->done = true;
        state->cv.notify_all();
    };

    const CommandResult submitRes = q->submit(req);
    if (submitRes != CommandResult::Ok) return {submitRes, false};

    std::unique_lock<std::mutex> lk(state->mutex);
    const bool done = state->cv.wait_for(lk, timeout, [&]() { return state->done; });
    if (done) return {state->result, false};
    // 超时只表示「调用方停止等待」，不表示命令失败；命令可能仍在 Writer 运行。
    return {state->result, true};
}

} // namespace

const char* toString(DbStatus s)
{
    switch (s) {
        case DbStatus::Closed:  return "Closed";
        case DbStatus::Opening: return "Opening";
        case DbStatus::Ready:   return "Ready";
        case DbStatus::Closing: return "Closing";
        case DbStatus::Locked:  return "Locked";
    }
    return "?";
}

struct NativeDatabase::Impl {
    std::unique_ptr<CipherDatabase> conn;
    std::shared_ptr<DbCommandQueue> queue;
    std::shared_ptr<ReadPool> pool;
    ProcessLock lock; // 跨进程排他锁：仅主进程可打开影子库
};

NativeDatabase::NativeDatabase() : m_impl(std::make_unique<Impl>()) {}
NativeDatabase::~NativeDatabase() { close(); }

bool NativeDatabase::open(const std::string& filesDir, std::int64_t ownerId,
                          const std::vector<unsigned char>& key32, std::string* err)
{
    std::lock_guard<std::mutex> lifecycle(m_lifecycleMutex);
    closeInternal();
    return openInternal(filesDir, ownerId, key32, err);
}

bool NativeDatabase::openInternal(const std::string& filesDir, std::int64_t ownerId,
                                  const std::vector<unsigned char>& key32, std::string* err)
{
    m_status.store(DbStatus::Opening);

    std::string path;
    std::string pathErr;
    if (!resolveNativeDatabasePath(filesDir, ownerId, path, pathErr)) {
        if (err) *err = pathErr;
        m_status.store(DbStatus::Closed);
        return false;
    }

    // 目录可能不存在：先确保 native_db 存在（SQLite 不会自动建多级目录）
    {
        const std::string dir = path.substr(0, path.find_last_of('/'));
        ::mkdir(dir.c_str(), 0700);
    }

    // 跨进程排他锁：影子库只允许应用主进程打开；第二进程在此被拒绝。
    {
        std::string lockErr;
        if (!m_impl->lock.tryAcquire(path, &lockErr)) {
            if (err) *err = "获取进程锁失败（第二进程打开被拒）：" + lockErr;
            m_status.store(DbStatus::Locked);
            return false;
        }
    }

    auto conn = std::make_unique<CipherDatabase>();
    const CipherError ce = conn->open(path, key32);
    if (ce != CipherError::Ok) {
        if (err) *err = std::string("打开加密库失败：") + toString(ce) + " " + conn->lastMessage();
        m_status.store(DbStatus::Locked);
        conn->close();
        m_impl->lock.release();
        return false;
    }
    sqlite3* db = conn->nativeHandle();
    if (db == nullptr) {
        if (err) *err = "写连接为空";
        m_status.store(DbStatus::Locked);
        conn->close();
        m_impl->lock.release();
        return false;
    }

    std::string migErr;
    if (SchemaManager::migrateTo(db, 0, &migErr) != SchemaError::Ok) {
        if (err) *err = std::string("Schema 迁移失败：") + migErr;
        m_status.store(DbStatus::Locked);
        conn->close();
        m_impl->lock.release();
        return false; // 不删库：保留现场供诊断
    }

    auto queue = std::make_shared<DbCommandQueue>(db, /*capacity=*/4096, nullptr);
    auto pool = std::make_shared<ReadPool>(path, key32, /*size=*/2);
    if (!pool->open(&migErr)) {
        if (err) *err = std::string("只读池打开失败：") + migErr;
        queue->close();
        conn->close();
        m_impl->lock.release();
        m_status.store(DbStatus::Locked);
        return false;
    }

    // 挂载（锁内，短临界区）
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_impl->conn = std::move(conn);
        m_impl->queue = std::move(queue);
        m_impl->pool = std::move(pool);
        m_path = path;
        m_ownerId.store(ownerId, std::memory_order_release);
        m_status.store(DbStatus::Ready);
    }
    return true;
}

bool NativeDatabase::openWithBridge(const std::string& filesDir, std::int64_t ownerId,
                                    IPlatformKeyBridge& bridge, std::string* err)
{
    std::lock_guard<std::mutex> lifecycle(m_lifecycleMutex);
    closeInternal();
    SecureKeyBuffer key;
    const IPlatformKeyBridge::Result r = bridge.loadKey(ownerId, key);
    if (r != IPlatformKeyBridge::Result::Ok || key.size() != SecureKeyBuffer::kExpectedSize) {
        // fail-close：不降级、不用 Token 派生、不静默生成新 key
        m_status.store(DbStatus::Locked);
        if (err) *err = std::string("库密钥不可用（") + toString(r) + "），数据库保持锁定";
        return false;
    }
    const bool ok = openInternal(filesDir, ownerId, key.bytes(), err);
    key.clear(); // 用完立即清零
    return ok;
}

void NativeDatabase::close()
{
    std::lock_guard<std::mutex> lifecycle(m_lifecycleMutex);
    closeInternal();
}

void NativeDatabase::closeInternal()
{
    // 阶段一：锁内移交所有权并置 Closing，禁止新 operation/lease
    std::unique_ptr<CipherDatabase> conn;
    std::shared_ptr<DbCommandQueue> queue;
    std::shared_ptr<ReadPool> pool;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_status.load() == DbStatus::Closed) return; // 幂等
        m_status.store(DbStatus::Closing);
        conn = std::move(m_impl->conn);
        queue = std::move(m_impl->queue);
        pool = std::move(m_impl->pool);
    }

    // 阶段二：锁外关闭（join Writer、等待 lease、关连接）；绝不持生命周期锁执行回调/join
    if (queue) queue->close();
    if (pool) pool->close();
    if (conn) conn->close();

    // 阶段三：锁内置 Closed（锁内只做快速 syscall 释放进程锁）
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_impl->lock.release();
        m_status.store(DbStatus::Closed);
    }
}

CommandResult NativeDatabase::submit(const DbCommandQueue::Request& req)
{
    std::shared_ptr<DbCommandQueue> q;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_status.load() != DbStatus::Ready) return CommandResult::NotOpen;
        q = m_impl->queue;
    }
    if (!q) return CommandResult::NotOpen;
    return q->submit(req);
}

CommandResult NativeDatabase::submitSync(const DbCommandQueue::WriteFn& fn,
                                         std::chrono::milliseconds timeout, bool* timedOut)
{
    std::shared_ptr<DbCommandQueue> q;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_status.load() != DbStatus::Ready) return CommandResult::NotOpen;
        q = m_impl->queue;
    }
    if (!q) return CommandResult::NotOpen;

    const auto r = runWriteSync(q, fn, timeout);
    if (timedOut) *timedOut = r.timedOut;
    return r.command;
}

ReadResult NativeDatabase::withRead(const std::function<void(sqlite3*)>& fn,
                                    std::chrono::milliseconds waitFor)
{
    std::shared_ptr<ReadPool> p;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_status.load() != DbStatus::Ready) return ReadResult::Closing;
        p = m_impl->pool;
    }
    if (!p) return ReadResult::Closing;
    return p->withRead(fn, waitFor);
}

bool NativeDatabase::beginMigration(std::int64_t ownerId, std::string* err)
{
    std::shared_ptr<DbCommandQueue> q;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_status.load() != DbStatus::Ready) {
            if (err) *err = "数据库未就绪";
            return false;
        }
        q = m_impl->queue;
    }
    if (!q) {
        if (err) *err = "队列为空";
        return false;
    }

    auto innerErr = std::make_shared<std::string>();
    const auto r = runWriteSync(q, [ownerId, innerErr](sqlite3* d) -> bool {
        return MigrationImporter::beginImport(d, ownerId, innerErr.get());
    }, std::chrono::seconds(60));

    if (r.timedOut) {
        if (err) *err = "等待 Writer 超时，操作状态未确定";
        return false;
    }
    if (err) *err = *innerErr;
    return r.command == CommandResult::Ok;
}

bool NativeDatabase::resetMigration(std::int64_t ownerId, std::string* err)
{
    std::shared_ptr<DbCommandQueue> q;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_status.load() != DbStatus::Ready) {
            if (err) *err = "数据库未就绪";
            return false;
        }
        q = m_impl->queue;
    }
    if (!q) {
        if (err) *err = "队列为空";
        return false;
    }

    auto innerErr = std::make_shared<std::string>();
    const auto r = runWriteSync(q, [ownerId, innerErr](sqlite3* d) -> bool {
        return MigrationImporter::resetForReimport(d, ownerId, innerErr.get());
    }, std::chrono::seconds(60));

    if (r.timedOut) {
        if (err) *err = "等待 Writer 超时，操作状态未确定";
        return false;
    }
    if (err) *err = *innerErr;
    return r.command == CommandResult::Ok;
}

MigrationSubmitOutcome NativeDatabase::submitMigrationBatch(
    std::int64_t ownerId, const std::vector<MigrationMessage>& msgs,
    const std::string& checkpoint, std::chrono::milliseconds timeout)
{
    MigrationSubmitOutcome out;
    std::shared_ptr<DbCommandQueue> q;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_status.load() != DbStatus::Ready) {
            out.error = "数据库未就绪";
            return out;
        }
        q = m_impl->queue;
    }
    if (!q) {
        out.error = "队列为空";
        return out;
    }

    // 输入拷贝到堆上：超时返回后 Writer 仍可能执行 fn，不能捕获 JNI 栈变量
    auto msgsPtr = std::make_shared<std::vector<MigrationMessage>>(msgs);
    auto cpPtr = std::make_shared<std::string>(checkpoint);
    auto innerErr = std::make_shared<std::string>();

    const auto r = runWriteSync(q, [ownerId, msgsPtr, cpPtr, innerErr](sqlite3* d) -> bool {
        // F05：状态校验与数据操作在同一 Writer 事务内，Disabled/Verified 时拒绝导入
        MigrationState st = MigrationState::Disabled;
        if (!MigrationImporter::getState(d, ownerId, &st)) {
            *innerErr = "读取迁移状态失败";
            return false;
        }
        if (st != MigrationState::ShadowImport) {
            *innerErr = "迁移状态非 shadow_import（当前=" + std::string(toString(st)) + "），拒绝导入";
            return false;
        }
        std::string e;
        if (!MigrationImporter::importBatch(d, ownerId, *msgsPtr, &e)) {
            *innerErr = "import: " + e;
            return false;
        }
        std::string e2;
        if (!MigrationImporter::saveCheckpoint(d, ownerId, *cpPtr, &e2)) {
            *innerErr = "checkpoint: " + e2;
            return false;
        }
        return true;
    }, timeout);

    if (r.timedOut) {
        out.timedOut = true;
        out.error = "等待 Writer 超时，操作状态未确定";
        return out;
    }
    out.command = r.command;
    out.ok = (r.command == CommandResult::Ok);
    if (out.ok) out.imported = msgs.size();
    out.error = *innerErr;
    return out;
}

MigrationSubmitOutcome NativeDatabase::submitConversationBatch(
    std::int64_t ownerId, const std::vector<MigrationConversation>& convs,
    const std::string& checkpoint, std::chrono::milliseconds timeout)
{
    MigrationSubmitOutcome out;
    std::shared_ptr<DbCommandQueue> q;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_status.load() != DbStatus::Ready) {
            out.error = "数据库未就绪";
            return out;
        }
        q = m_impl->queue;
    }
    if (!q) {
        out.error = "队列为空";
        return out;
    }

    auto convsPtr = std::make_shared<std::vector<MigrationConversation>>(convs);
    auto cpPtr = std::make_shared<std::string>(checkpoint);
    auto innerErr = std::make_shared<std::string>();

    const auto r = runWriteSync(q, [ownerId, convsPtr, cpPtr, innerErr](sqlite3* d) -> bool {
        MigrationState st = MigrationState::Disabled;
        if (!MigrationImporter::getState(d, ownerId, &st)) {
            *innerErr = "读取迁移状态失败";
            return false;
        }
        if (st != MigrationState::ShadowImport) {
            *innerErr = "迁移状态非 shadow_import（当前=" + std::string(toString(st)) + "），拒绝导入";
            return false;
        }
        std::string e;
        if (!MigrationImporter::importConversations(d, ownerId, *convsPtr, &e)) {
            *innerErr = "importConversations: " + e;
            return false;
        }
        std::string e2;
        if (!MigrationImporter::saveConversationCheckpoint(d, ownerId, *cpPtr, &e2)) {
            *innerErr = "conversations_checkpoint: " + e2;
            return false;
        }
        return true;
    }, timeout);

    if (r.timedOut) {
        out.timedOut = true;
        out.error = "等待 Writer 超时，操作状态未确定";
        return out;
    }
    out.command = r.command;
    out.ok = (r.command == CommandResult::Ok);
    if (out.ok) out.imported = convs.size();
    out.error = *innerErr;
    return out;
}

bool NativeDatabase::finishMigration(std::int64_t ownerId, const MigrationSummary& expected,
                                     MigrationSummary* actual, std::string* err)
{
    std::shared_ptr<DbCommandQueue> q;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_status.load() != DbStatus::Ready) {
            if (err) *err = "数据库未就绪";
            return false;
        }
        q = m_impl->queue;
    }
    if (!q) {
        if (err) *err = "队列为空";
        return false;
    }

    auto innerErr = std::make_shared<std::string>();
    auto got = std::make_shared<MigrationSummary>();
    const auto r = runWriteSync(q, [ownerId, expected, innerErr, got](sqlite3* d) -> bool {
        // F05：finish 要求当前必须为 shadow_import；对账通过后由 finish 置 verified
        MigrationState st = MigrationState::Disabled;
        if (!MigrationImporter::getState(d, ownerId, &st)) {
            *innerErr = "读取迁移状态失败";
            return false;
        }
        if (st != MigrationState::ShadowImport) {
            *innerErr = "迁移状态非 shadow_import（当前=" + std::string(toString(st)) + "），拒绝完成";
            return false;
        }
        return MigrationImporter::finish(d, ownerId, expected, got.get(), innerErr.get());
    }, std::chrono::seconds(60));

    if (r.timedOut) {
        if (err) *err = "等待 Writer 超时，操作状态未确定";
        return false;
    }
    if (actual) *actual = *got;
    if (err) *err = *innerErr;
    return r.command == CommandResult::Ok;
}

bool NativeDatabase::queryMigrationState(std::int64_t ownerId, MigrationStateSnapshot* out)
{
    if (!out) return false;
    std::shared_ptr<ReadPool> p;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_status.load() != DbStatus::Ready) return false;
        p = m_impl->pool;
    }
    if (!p) return false;

    auto snap = std::make_shared<MigrationStateSnapshot>();
    bool ok = false;
    const ReadResult rr = p->withRead([&](sqlite3* d) {
        // 四组字段必须来自同一 WAL 快照；read-only 连接允许 BEGIN 读事务。
        if (sqlite3_exec(d, "BEGIN;", nullptr, nullptr, nullptr) != SQLITE_OK) return;
        bool completed = false;
        const bool c1 = MigrationImporter::isCompleted(d, ownerId, &completed);
        MigrationState st = MigrationState::Disabled;
        const bool c2 = MigrationImporter::getState(d, ownerId, &st);
        std::string cp;
        MigrationImporter::readCheckpoint(d, ownerId, &cp); // 无记录 → cp 空
        MigrationSummary sum;
        const bool c3 = MigrationImporter::computeSummary(d, ownerId, &sum, nullptr);
        if (c1 && c2 && c3) {
            snap->completed = completed;
            snap->state = st;
            snap->checkpoint = cp;
            snap->summary = sum;
            ok = true;
        }
        if (sqlite3_exec(d, ok ? "COMMIT;" : "ROLLBACK;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            ok = false;
        }
    });
    if (rr == ReadResult::Ok && ok) {
        *out = *snap;
        return true;
    }
    return false;
}

SelfTestOutcome NativeDatabase::runSelfTest(std::int64_t ownerId)
{
    SelfTestOutcome out;
    std::shared_ptr<ReadPool> p;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_status.load() != DbStatus::Ready) {
            out.error = "数据库未就绪（状态=" + std::string(toString(m_status.load())) + "）";
            return out;
        }
        p = m_impl->pool;
    }
    if (!p) {
        out.error = "读池为空";
        return out;
    }

    const ReadResult rr = p->withRead([&](sqlite3* d) {
        if (sqlite3_exec(d, "BEGIN;", nullptr, nullptr, nullptr) != SQLITE_OK) return;
        bool valid = true;
        // PRAGMA cipher_version
        {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(d, "PRAGMA cipher_version", -1, &stmt, nullptr) == SQLITE_OK &&
                stmt) {
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    const unsigned char* t = sqlite3_column_text(stmt, 0);
                    if (t) out.cipher = reinterpret_cast<const char*>(t);
                }
            } else valid = false;
            if (stmt) sqlite3_finalize(stmt);
        }
        // PRAGMA user_version
        {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(d, "PRAGMA user_version", -1, &stmt, nullptr) == SQLITE_OK &&
                stmt) {
                if (sqlite3_step(stmt) == SQLITE_ROW) out.schemaVersion = sqlite3_column_int(stmt, 0);
            } else valid = false;
            if (stmt) sqlite3_finalize(stmt);
        }
        valid = MigrationImporter::isCompleted(d, ownerId, &out.completed) && valid;
        valid = MigrationImporter::computeSummary(d, ownerId, &out.summary, nullptr) && valid;
        if (sqlite3_exec(d, valid ? "COMMIT;" : "ROLLBACK;", nullptr, nullptr, nullptr) == SQLITE_OK) {
            out.ok = valid;
        }
    });
    if (rr != ReadResult::Ok) out.error = std::string("读取失败：") + toString(rr);
    return out;
}

} // namespace storage
} // namespace im
