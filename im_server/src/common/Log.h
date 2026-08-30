#pragma once
// 线程安全日志：先把整行拼好再一次性输出，避免多线程 cout 交错。
// 用法：imsrv::log("[server] xxx ", value, " ...");

#include <iostream>
#include <mutex>
#include <sstream>

namespace imsrv {

namespace detail {
inline std::mutex& logMutex()
{
    static std::mutex m;
    return m;
}
} // namespace detail

template <typename... Args>
void log(Args&&... args)
{
    std::ostringstream oss;
    (oss << ... << std::forward<Args>(args));
    std::lock_guard<std::mutex> lock(detail::logMutex());
    std::cout << oss.str() << std::endl;
}

} // namespace imsrv
