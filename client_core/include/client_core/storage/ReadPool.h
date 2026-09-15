// 只读连接池（P6-T04）。
//
// 目的：把读查询与唯一的 Writer 分开，读不阻塞写、多个读可以并行。
//
// 不变式：
//   - 所有连接都以 `SQLITE_OPEN_READONLY` 打开并正确设 key（错误 key 直接开失败）；
//   - 写 SQL 在只读连接上**必须被拒绝**（由 SQLite 的 SQLITE_READONLY 保证）；
//   - 一个查询在其作用域内**独占一条连接**：池满时等待空闲连接（有 deadline），
//     **绝不**把正在使用的连接再次借出；
//   - 查询结果必须由调用方**复制为值对象**；sqlite3* / sqlite3_stmt* 不跨越模块边界，
//     更不得跨 JNI 暴露；
//   - 连接数量有界（默认 2），避免 fd 膨胀；
//   - key 仅在 open 期间使用，open 成功后立即清零，不长期驻留。

#ifndef CLIENT_CORE_READ_POOL_H
#define CLIENT_CORE_READ_POOL_H

#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace im {
namespace storage {

/** 借一条只读连接执行查询的结果。 */
enum class ReadResult {
    Ok,              // 借到连接且回调执行成功
    Busy,            // 全部连接占用（调用方不允许等待）
    Timeout,         // 等待空闲连接超过 deadline
    Closing,         // 池正在关闭（close 已开始）
    CallbackFailed,  // 回调抛异常（已被捕获并转换为该结果）
};

const char* toString(ReadResult r);

class ReadPool {
public:
    ReadPool(std::string path, std::vector<unsigned char> key, int size = 2);
    ~ReadPool();

    ReadPool(const ReadPool&) = delete;
    ReadPool& operator=(const ReadPool&) = delete;

    /** 打开全部只读连接；任一失败则全部关闭并返回 false。成功后清零 key。 */
    bool open(std::string* message = nullptr);

    /**
     * 关闭：停止借出（置 Closing），唤醒所有等待者（返回 Closing），
     * 等待 active lease 归还后再关闭连接。与 active 查询并发安全。
     */
    void close();
    bool isOpen() const;
    int size() const;

    /**
     * 借一条只读连接执行查询。fn 内部可放心读库；不允许写。
     *
     * @param fn      查询回调（no-throw 约定；若抛异常会被捕获并返回 CallbackFailed）
     * @param waitFor 等待空闲连接的最长时间（池满时）；默认有界等待
     * @return 见 ReadResult
     */
    ReadResult withRead(const std::function<void(sqlite3*)>& fn,
                        std::chrono::milliseconds waitFor = std::chrono::milliseconds(5000));

private:
    std::string m_path;
    std::vector<unsigned char> m_key;
    int m_size;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<sqlite3*> m_conns;
    // mutable：withRead 是 const 方法，但需要标记连接的占用状态（用 char 避免 vector<bool> 代理）
    mutable std::vector<char> m_busy;
    mutable std::size_t m_next = 0;
    // 正在执行回调的连接数（close 需等待其归零）
    mutable std::size_t m_active = 0;
    mutable std::size_t m_waiters = 0;
    bool m_open = false;
};

} // namespace storage
} // namespace im

#endif // CLIENT_CORE_READ_POOL_H
