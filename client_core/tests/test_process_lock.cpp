// 跨进程排他锁单测（P6-T05 / §24.7 第二进程拒绝）。
//
// 验证 Native 影子库只允许一个进程打开：
//   - 首次获取成功；
//   - 同进程第二个实例被拒（flock 与打开的文件描述关联，独立 open 也互斥）；
//   - fork 子进程尝试获取被父进程持有 → 被拒；
//   - 释放后可再次获取。

#include "client_core/storage/ProcessLock.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <iostream>
#include <string>

using namespace im::storage;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& name)
{
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << std::endl;
    if (!cond) ++g_failures;
}

const char* kPath = "/tmp/test_process_lock.db";
} // namespace

int main()
{
    std::cout << "=== test_process_lock ===" << std::endl;
    std::remove((std::string(kPath) + ".lock").c_str());

    // [1] 首次获取成功
    ProcessLock l1;
    std::string err;
    check(l1.tryAcquire(kPath, &err), "首次获取排他锁 " + err);
    check(l1.held(), "持有锁");

    // [2] 同进程第二个实例获取同一锁失败（flock 与 fd 关联，独立 open 也互斥）
    ProcessLock l2;
    std::string err2;
    check(!l2.tryAcquire(kPath, &err2), "同进程第二个实例被拒 " + err2);
    check(!l2.held(), "第二实例未持有");

    // [3] fork 子进程尝试获取，失败（父进程持有）
    pid_t pid = ::fork();
    if (pid == 0) {
        ProcessLock child;
        std::string cerr;
        const bool ok = child.tryAcquire(kPath, &cerr);
        _exit(ok ? 1 : 0); // 成功拿到锁 → 返回 1（错误）；被拒 → 返回 0（正确）
    } else if (pid > 0) {
        int status = 0;
        ::waitpid(pid, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "fork 子进程获取锁被拒");
    } else {
        check(false, "fork 失败");
    }

    // [4] 释放后可再次获取
    l1.release();
    check(!l1.held(), "释放后未持有");
    ProcessLock l3;
    check(l3.tryAcquire(kPath, &err), "释放后可再次获取 " + err);
    l3.release();

    std::remove((std::string(kPath) + ".lock").c_str());
    if (g_failures == 0) {
        std::cout << "test_process_lock PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_process_lock FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
