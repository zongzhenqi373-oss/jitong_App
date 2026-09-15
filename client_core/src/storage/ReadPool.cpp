#include "client_core/storage/ReadPool.h"

#include <sqlite3.h>

#include <openssl/crypto.h>

#include "client_core/storage/CipherDatabase.h"

namespace im {
namespace storage {

const char* toString(ReadResult r)
{
    switch (r) {
        case ReadResult::Ok:             return "Ok";
        case ReadResult::Busy:           return "Busy";
        case ReadResult::Timeout:        return "Timeout";
        case ReadResult::Closing:        return "Closing";
        case ReadResult::CallbackFailed: return "CallbackFailed";
    }
    return "?";
}

ReadPool::ReadPool(std::string path, std::vector<unsigned char> key, int size)
    : m_path(std::move(path)), m_key(std::move(key)), m_size(size)
{
    if (m_size < 1) m_size = 1;
}

ReadPool::~ReadPool() { close(); }

bool ReadPool::open(std::string* message)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_open) return true;

    m_conns.clear();
    m_busy.clear();
    for (int i = 0; i < m_size; ++i) {
        sqlite3* conn = nullptr;
        std::string err;
        const CipherError e =
            CipherDatabase::openConnection(m_path, m_key, /*readonly=*/true, &conn, &err);
        if (e != CipherError::Ok || conn == nullptr) {
            if (message) {
                *message = "只读连接 " + std::to_string(i) + " 打开失败：" + err;
            }
            for (sqlite3* c : m_conns) sqlite3_close(c);
            m_conns.clear();
            m_busy.clear();
            m_open = false;
            if (!m_key.empty()) {
                OPENSSL_cleanse(m_key.data(), m_key.size());
                m_key.clear();
            }
            return false;
        }
        m_conns.push_back(conn);
        m_busy.push_back(false);
    }
    m_open = true;
    // key 仅在打开期间使用，成功后立即清零，不长期驻留（F09/F13）
    if (!m_key.empty()) {
        OPENSSL_cleanse(m_key.data(), m_key.size());
        m_key.clear();
    }
    return true;
}

void ReadPool::close()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    m_open = false;
    m_cv.notify_all(); // 唤醒所有 waiter，让它们返回 Closing
    // active lease 与已进入 condition_variable 的 waiter 都退出后，才能销毁连接/cv 状态。
    m_cv.wait(lk, [this]() { return m_active == 0 && m_waiters == 0; });

    for (sqlite3* c : m_conns) sqlite3_close(c);
    m_conns.clear();
    m_busy.clear();
    if (!m_key.empty()) {
        OPENSSL_cleanse(m_key.data(), m_key.size());
        m_key.clear();
    }
}

bool ReadPool::isOpen() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_open;
}

int ReadPool::size() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return static_cast<int>(m_conns.size());
}

ReadResult ReadPool::withRead(const std::function<void(sqlite3*)>& fn,
                              std::chrono::milliseconds waitFor)
{
    std::unique_lock<std::mutex> lk(m_mutex);
    if (!m_open || m_conns.empty()) return ReadResult::Closing;

    auto findIdle = [&](std::size_t& out) -> bool {
        for (std::size_t i = 0; i < m_conns.size(); ++i) {
            const std::size_t j = (m_next + i) % m_conns.size();
            if (!m_busy[j]) {
                out = j;
                return true;
            }
        }
        return false;
    };

    std::size_t slot = 0;
    if (!findIdle(slot)) {
        if (waitFor.count() <= 0) return ReadResult::Busy;
        const auto deadline = std::chrono::steady_clock::now() + waitFor;
        ++m_waiters;
        // 等待直到有空闲、或关闭、或超时；绝不复用忙连接
        const bool ok = m_cv.wait_until(lk, deadline, [&]() {
            if (!m_open) return true;
            std::size_t ignored = 0;
            return findIdle(ignored);
        });
        --m_waiters;
        m_cv.notify_all();
        if (!m_open) return ReadResult::Closing;
        if (!ok) return ReadResult::Timeout;
        if (!findIdle(slot)) return ReadResult::Closing; // 防御：predicate 保证有空闲，不应发生
    }

    sqlite3* conn = m_conns[slot];
    m_busy[slot] = true;
    ++m_active;
    m_next = (slot + 1) % m_conns.size();
    lk.unlock();

    // ---- 锁外执行回调：绝不持锁进入用户代码，避免重入死锁 ----
    ReadResult result = ReadResult::Ok;
    try {
        if (fn) fn(conn);
    } catch (...) {
        result = ReadResult::CallbackFailed;
    }

    // ---- 归还连接 ----
    lk.lock();
    m_busy[slot] = false;
    if (m_active > 0) --m_active;
    lk.unlock();
    m_cv.notify_all();
    return result;
}

} // namespace storage
} // namespace im
