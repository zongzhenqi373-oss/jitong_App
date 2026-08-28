#include "DbWriteQueue.h"

#include <sqlite3.h>

namespace imsrv {

DbWriteQueue::~DbWriteQueue()
{
    {
        std::lock_guard<std::mutex> lg(m_mtx);
        m_running = false;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;   // 防止重复关闭
    }
}

bool DbWriteQueue::open(const std::string& dbPath)
{
    std::lock_guard<std::mutex> lock(m_mtx); 
    if (m_running) return true;

    // 打开数据库
    sqlite3* openDb = nullptr;
    if (sqlite3_open(dbPath.c_str(), &openDb) != SQLITE_OK) {
        if(openDb) sqlite3_close(openDb);   
        return false;
    }
    // 执行 PRAGMA
    char* err = nullptr;
    auto excutePragma = [&](const char* pragma){
        const int rc = sqlite3_exec(openDb, pragma, nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            if (err) sqlite3_free(err);
            sqlite3_close(openDb);
            return false;
        }
        return true;
    };

    // 跟 Database 的只读连接池一致的并发配置：WAL（读写不互斥）+ busy_timeout。
    // 这条连接从头到尾只在这一条写线程上使用，不会有别的线程碰它。
    if (!excutePragma("PRAGMA journal_mode=WAL;")) return false;
    if (!excutePragma("PRAGMA busy_timeout=5000;")) return false;
    if (!excutePragma("PRAGMA foreign_keys=ON;")) return false;

    m_db = openDb;
    m_running = true;
    // 启动线程
    try
    {
        m_thread = std::thread([this] { run(); });
    }
    catch (...) { // 线程启动失败，关闭数据库
        sqlite3_close(m_db);
        m_db = nullptr;
        m_running = false;
        throw;
    }

    return true;
}

// 获取 statement
sqlite3_stmt* DbWriteQueue::statement(const std::string& key, const char* sql)
{
    auto found = m_stmtCache.find(key);

    if (found != m_stmtCache.end()) {
        sqlite3_reset(found->second);
        sqlite3_clear_bindings(found->second);
        return found->second;
    }

    sqlite3_stmt* statement = nullptr;

    if (sqlite3_prepare_v2(m_db, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return nullptr;
    }

    m_stmtCache.emplace(key, statement);
    return statement;
}

void DbWriteQueue::run()
{
    {
        //在线程开始的时候记录线程id
        std::lock_guard<std::mutex> lock(m_mtx);
        m_workerId = std::this_thread::get_id();
    }
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(m_mtx);  // 等待任务
            m_cv.wait(lk, [this] { return !m_queue.empty() || !m_running; });  // 等待任务或停止
            if (!m_running && m_queue.empty()) {  // 停止且无任务
                m_workerId = {};   
                return;
            }
            task = std::move(m_queue.front());   // 取任务
            m_queue.pop_front();   // 移除任务
        }
        task(); // 严格串行：一次只跑一个任务，跑完才取下一个
    }
}

} // namespace imsrv
