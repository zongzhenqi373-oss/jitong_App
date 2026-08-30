#pragma once
// 单写线程队列：一个数据库文件对应一条专属写线程 + 一条任务队列，所有写操作严格串行执行。
//
// 解决的问题：连接池模式下多个连接可能并发发起写事务，本质上都是在抢 SQLite 的全局写锁，
// 换不来真实的写并行，还需要每个写方法自己用 BEGIN IMMEDIATE 才能保证原子性（漏写一次就
// 可能出现竞态，参见早期的会话 seq 分配竞态问题）。改成单写线程后，任意时刻只可能有一个
// 写操作在执行，正确性是架构上的天然保证，不再依赖每个方法自觉写对事务。
//
// 读操作不受影响：SQLite WAL 模式下读写互不阻塞，只读查询继续走 Database 现有的连接池即可，
// 这条队列只负责"写"这一类操作。

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <memory>
#include <stdexcept>
#include <unordered_map>

struct sqlite3_stmt;

struct sqlite3;

namespace imsrv {

class DbWriteQueue {
public:
    DbWriteQueue() = default;
    ~DbWriteQueue();

    DbWriteQueue(const DbWriteQueue&) = delete;
    DbWriteQueue& operator=(const DbWriteQueue&) = delete;

    // 打开专属写连接（与只读连接池指向同一个数据库文件，SQLite/WAL 允许多连接共存），
    // 启动写线程。重复调用安全（幂等）。
    bool open(const std::string& dbPath);

    // 提交一个写任务：fn 在写线程上串行执行，拿到的是这条队列独占的 sqlite3* 连接。
    // 阻塞等待任务真正跑完再返回——调用方感知不到底层已经是异步队列，写在 Database 里的
    // 方法可以保持原来的同步签名，不需要联动修改 Dispatcher.cpp 里的调用方式。
    template <class F>
    auto submit(F&& fn) -> std::invoke_result_t<std::decay_t<F>&, sqlite3*>
    {
        using Fn = std::decay_t<F>;
        using Ret = std::invoke_result_t<Fn&, sqlite3*>;

        auto promise = std::make_shared<std::promise<Ret>>();
        std::future<Ret> future = promise->get_future();
        auto taskFunction = std::make_shared<Fn>(std::forward<F>(fn));
        {
            std::lock_guard<std::mutex> lg(m_mtx);
            // 检查是否已经打开
            if(!m_running){
                throw std::runtime_error("DbWriteQueue not running");
            }
            // 检查是否在写线程
            if(std::this_thread::get_id() == m_workerId){
                throw std::logic_error("DbWriteQueue::submit cannot be called from its worker thread");
            }
            
            // 添加任务
            m_queue.emplace_back([this, taskFunction, promise]() {
                try {
                    if constexpr (std::is_void_v<Ret>) {
                        std::invoke(*taskFunction, m_db);
                        promise->set_value();
                    } else {
                        promise->set_value(std::invoke(*taskFunction, m_db));
                    }
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            });
        }
        m_cv.notify_one();
        return future.get(); // fn 抛出的异常会在这里重新抛出，调用方按原来的方式处理
    }

    sqlite3_stmt* statement(const std::string& key, const char* sql);

private:
    void run();

    sqlite3* m_db = nullptr;
    std::thread m_thread;
    std::thread::id m_workerId{};
    std::mutex m_mtx;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_queue;
    bool m_running = false;
    std::unordered_map<std::string, sqlite3_stmt*> m_stmtCache;
};

} // namespace imsrv
