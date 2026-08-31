#include "session/Presence.h"
#include "session/Session.h"

// Presence 类实现，管理用户在线状态及会话信息
namespace imsrv {

// 上线
void Presence::online(int userId, const std::shared_ptr<Session>& s)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    m_map[userId] = Entry{s, std::time(nullptr)};
}

// 替换
std::shared_ptr<Session> Presence::replace(int userId, const std::shared_ptr<Session>& s)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    std::shared_ptr<Session> old;
    if (auto it = m_map.find(userId); it != m_map.end()) old = it->second.sess.lock();
    m_map[userId] = Entry{s, std::time(nullptr)};
    return old;
}

// 下线
void Presence::offline(int userId, const std::shared_ptr<Session>& s)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    auto it = m_map.find(userId);
    if (it != m_map.end()) {
        // 仅当记录的就是这个 session 才摘除：互踢场景下 map 已被新连接覆盖，
        // 旧连接的断开上报会走到这里，此时绝不能把新连接摘掉
        if (it->second.sess.lock() == s) m_map.erase(it);
    }
}

// 获取在线会话
std::shared_ptr<Session> Presence::get(int userId)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    auto it = m_map.find(userId);
    if (it == m_map.end()) return nullptr;
    return it->second.sess.lock();
}

// 更新最后活跃时间
void Presence::touch(int userId)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    auto it = m_map.find(userId);
    if (it != m_map.end()) it->second.lastActive = std::time(nullptr);
}

// 扫描超时的会话
std::vector<std::pair<int, std::shared_ptr<Session>>> Presence::scanStale(int timeoutSec)
{
    std::vector<std::pair<int, std::shared_ptr<Session>>> stale;
    const time_t now = std::time(nullptr);
    std::lock_guard<std::mutex> lock(m_mtx);
    for (auto& kv : m_map) {
        auto s = kv.second.sess.lock();
        if (!s) continue; // 弱引用失效，连接已消亡（由关闭路径负责清理映射）
        if (kv.second.lastActive == 0) {
            kv.second.lastActive = now; // 首次见到给宽限期
            continue;
        }
        if (now - kv.second.lastActive > timeoutSec) {
            stale.emplace_back(kv.first, s);
        }
    }
    return stale;
}

} // namespace imsrv
