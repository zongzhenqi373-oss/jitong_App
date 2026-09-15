// 跨进程排他锁（P6-T05 / §24.7 第二进程拒绝）。
//
// 用途：Native 影子库只允许**应用主进程**打开。用 `flock` 对一个旁路锁文件
// （<dbPath>.lock）加 `LOCK_EX | LOCK_NB`：同一时刻只有一个进程能持有，第二进程
// （或 Room 后台组件 / 其它进程）尝试打开时立即失败，避免与 SQLite 跨进程竞争同一影子库。
//
// 说明：
//   - flock 锁与「打开的锁文件描述」关联，同进程两个独立 open() 也会互斥，因此可被
//     桌面单测在单进程内用 fork 验证「第二进程被拒」语义；
//   - 锁文件独立于数据库文件本身，ReadPool 只读连接不触碰它；
//   - 进程退出时内核自动释放 flock，无需担心崩溃残留（这正是选择 flock 而非
//     fcntl 记录锁的原因之一）。

#ifndef CLIENT_CORE_PROCESS_LOCK_H
#define CLIENT_CORE_PROCESS_LOCK_H

#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <unistd.h>

namespace im {
namespace storage {

class ProcessLock {
public:
    ProcessLock() = default;
    ~ProcessLock() { release(); }
    ProcessLock(const ProcessLock&) = delete;
    ProcessLock& operator=(const ProcessLock&) = delete;

    /**
     * 尝试对 dbPath 的旁路锁文件加排他锁（非阻塞）。
     * @return true 表示本进程成功持有锁；false 表示已被其它进程持有或加锁失败
     */
    bool tryAcquire(const std::string& dbPath, std::string* err = nullptr)
    {
        release();
        m_lockPath = dbPath + ".lock";
        m_fd = ::open(m_lockPath.c_str(), O_CREAT | O_RDWR, 0600);
        if (m_fd < 0) {
            if (err) *err = "打开锁文件失败";
            return false;
        }
        if (::flock(m_fd, LOCK_EX | LOCK_NB) != 0) {
            if (err) *err = "锁已被其它进程持有";
            ::close(m_fd);
            m_fd = -1;
            return false;
        }
        return true;
    }

    void release()
    {
        if (m_fd >= 0) {
            ::flock(m_fd, LOCK_UN);
            ::close(m_fd);
            m_fd = -1;
        }
    }

    bool held() const { return m_fd >= 0; }

private:
    int m_fd = -1;
    std::string m_lockPath;
};

} // namespace storage
} // namespace im

#endif // CLIENT_CORE_PROCESS_LOCK_H
