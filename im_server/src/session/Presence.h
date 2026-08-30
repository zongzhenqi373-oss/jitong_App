#pragma once
// 在线状态管理：userId → Session 弱引用 + 最近活跃时间
// 线程安全：io 线程（收发）与心跳扫描线程都会访问，内部互斥锁保护。
// 对齐旧版 Kernel::m_mapIdtoSocket，但改为弱引用 Session 指针，避免悬挂。

#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace imsrv {

class Session;

class Presence {
public:
    // 登录上线（重复登录时调用方负责先踢旧连接）
    void online(int userId, const std::shared_ptr<Session>& s);

    // 原子地把 userId 的在线映射替换成 s，返回被替换掉的旧 session（没有则为 nullptr）。
    // 比"先 get() 再 online()"更安全：两步分开会有并发登录竞态窗口，
    // 两个连接都读到旧值为空/旧连接后各自 online()，导致谁都没被踢下线。
    std::shared_ptr<Session> replace(int userId, const std::shared_ptr<Session>& s);

    // 下线：仅当 map 中记录的就是这个 session 时才摘除（防新连接被误摘）
    void offline(int userId, const std::shared_ptr<Session>& s);

    // 查询在线 session（不在线/弱引用失效返回 nullptr）
    std::shared_ptr<Session> get(int userId);

    bool isOnline(int userId) { return get(userId) != nullptr; }

    // 刷新活跃时间（任何有效包到达时调用）
    void touch(int userId);

    // 扫描超时连接：返回 (userId, session) 列表，调用方负责关闭与广播
    std::vector<std::pair<int, std::shared_ptr<Session>>> scanStale(int timeoutSec);

private:
    struct Entry {
        std::weak_ptr<Session> sess;
        time_t lastActive = 0;
    };

    std::mutex m_mtx;
    std::map<int, Entry> m_map;
};

} // namespace imsrv
