// 账号隔离与路径校验单元测试（P6-T02）。
// 纯字符串/数值逻辑，覆盖全部越界与非法 ownerId 场景（在创建文件之前就必须拒绝）。

#include "client_core/storage/DatabasePaths.h"
#include "client_core/storage/DbKeyBridge.h"

#include <iostream>
#include <string>
#include <vector>

using namespace im::storage;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& name)
{
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << std::endl;
    if (!cond) ++g_failures;
}

bool ok(const std::string& filesDir, std::int64_t ownerId, std::string& path)
{
    std::string err;
    return resolveNativeDatabasePath(filesDir, ownerId, path, err);
}
} // namespace

int main()
{
    std::cout << "=== test_database_paths ===" << std::endl;

    // [1] ownerId 合法性
    check(!isValidOwnerId(0), "ownerId=0 非法");
    check(!isValidOwnerId(-1), "ownerId=-1 非法");
    check(!isValidOwnerId(-999), "ownerId 负数非法");
    check(isValidOwnerId(1), "ownerId=1 合法");
    check(isValidOwnerId(2147483647LL), "ownerId 大正整数合法");

    // [2] 文件名与目录
    check(nativeDatabaseFileName(123) == "account_123.db", "文件名格式 account_<id>.db");
    check(std::string(nativeDatabaseDirName()) == "native_db", "目录名 native_db");

    // [3] 正常拼装
    {
        std::string path;
        check(ok("/data/user/0/com.jitong.im/files", 42, path), "正常拼装成功");
        check(path == "/data/user/0/com.jitong.im/files/native_db/account_42.db",
              "路径 = filesDir/native_db/account_<id>.db");
    }
    // 结尾斜杠应被规范化
    {
        std::string p1, p2;
        ok("/data/files", 7, p1);
        ok("/data/files///", 7, p2);
        check(p1 == p2, "结尾多余斜杠被规范化");
    }

    // [4] 越界与非法输入：必须在创建文件前拒绝
    {
        std::string path;
        std::string err;
        check(!resolveNativeDatabasePath("", 1, path, err), "filesDir 空 → 拒绝");
        check(!resolveNativeDatabasePath("/data/files/../secret", 1, path, err),
              "filesDir 含 .. → 拒绝");
        check(!resolveNativeDatabasePath("/data/files", 0, path, err), "ownerId=0 → 拒绝");
        check(!resolveNativeDatabasePath("/data/files", -5, path, err), "ownerId 负数 → 拒绝");
        check(path.empty(), "拒绝时不输出路径");
        check(!err.empty(), "拒绝时给出错误原因");
    }

    // [5] 不同账号必须落到不同文件
    {
        std::string a, b;
        ok("/data/files", 1001, a);
        ok("/data/files", 1002, b);
        check(a != b, "不同账号路径不同（物理隔离）");
    }

    // [6] containsParentTraversal
    check(containsParentTraversal(".."), "单独 .. 视为越界");
    check(containsParentTraversal("/data/../etc"), "中间 .. 视为越界");
    check(containsParentTraversal("/data/files/.."), "结尾 .. 视为越界");
    check(!containsParentTraversal("/data/files/native_db"), "正常路径不误判");
    check(!containsParentTraversal("account_1..db"), "含双点但非整段不误判");

    // [7] SecureKeyBuffer：析构/clear 清零且不可拷贝
    {
        SecureKeyBuffer::Bytes raw(SecureKeyBuffer::kExpectedSize, 0x5A);
        SecureKeyBuffer key(std::move(raw));
        check(key.size() == SecureKeyBuffer::kExpectedSize, "密钥长度 32");
        check(key.bytes()[0] == 0x5A, "密钥内容可读");
        // 注意：clear() 会释放底层缓冲，之后**不得**再访问其 data()（ASan 会判
        // container-overflow）。这里只验证容器语义，清零行为由实现保证（OPENSSL_cleanse）。
        key.clear();
        check(key.empty(), "clear 后为空");
    }
    {
        // 移动语义可用，拷贝被禁用（编译期保证，这里只验证移动后内容保留）
        SecureKeyBuffer::Bytes raw(SecureKeyBuffer::kExpectedSize, 0x11);
        SecureKeyBuffer a(std::move(raw));
        SecureKeyBuffer b(std::move(a));
        check(b.size() == 32 && b.bytes()[0] == 0x11, "移动后内容保留");
    }

    // [8] 密钥桥结果枚举
    check(std::string(toString(IPlatformKeyBridge::Result::Ok)) == "Ok", "Result::Ok 可读");
    check(std::string(toString(IPlatformKeyBridge::Result::Unavailable)) == "Unavailable",
          "Result::Unavailable 可读");

    if (g_failures == 0) {
        std::cout << "test_database_paths PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_database_paths FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
