// ReconnectPolicy 单元测试（P5-T05）。
// 验证指数退避序列(封顶)、jitter 叠加、reset、disable。

#include "client_core/ReconnectPolicy.h"

#include <iostream>
#include <memory>
#include <string>

using namespace im::account;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& name)
{
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << std::endl;
    if (!cond) ++g_failures;
}

class FixedJitter : public IJitter {
public:
    double value = 0.0;
    double next01() const override { return value; }
};

void testExponentialCapped()
{
    std::cout << "[1] 指数退避 1/2/4/8/16/30 封顶（无 jitter）" << std::endl;
    ReconnectPolicy p(/*base=*/1000, /*max=*/30000, /*jitterRatio=*/0.0);
    check(p.nextDelayMs() == 1000, "第1次 1000ms");
    check(p.nextDelayMs() == 2000, "第2次 2000ms");
    check(p.nextDelayMs() == 4000, "第3次 4000ms");
    check(p.nextDelayMs() == 8000, "第4次 8000ms");
    check(p.nextDelayMs() == 16000, "第5次 16000ms");
    check(p.nextDelayMs() == 30000, "第6次 封顶 30000ms");
    check(p.nextDelayMs() == 30000, "第7次 仍 30000ms");
}

void testReset()
{
    std::cout << "[2] reset 后从头开始" << std::endl;
    ReconnectPolicy p(1000, 30000, 0.0);
    p.nextDelayMs();
    p.nextDelayMs();
    check(p.attempt() == 2, "已退避 2 次");
    p.reset();
    check(p.attempt() == 0, "reset 清零");
    check(p.nextDelayMs() == 1000, "reset 后重新从 1000ms 开始");
}

void testDisable()
{
    std::cout << "[3] disable → 返回 -1" << std::endl;
    ReconnectPolicy p(1000, 30000, 0.0);
    p.nextDelayMs();
    p.disable();
    check(!p.enabled(), "已禁用");
    check(p.nextDelayMs() == -1, "禁用后返回 -1（不重连）");
    p.enable();
    check(p.nextDelayMs() >= 0, "重新启用后可重连");
}

void testJitter()
{
    std::cout << "[4] jitter 叠加（固定比例）" << std::endl;
    auto j = std::make_shared<FixedJitter>();
    j->value = 0.5; // 固定 0.5
    ReconnectPolicy p(1000, 30000, /*jitterRatio=*/0.25, j);
    // 第1次 base=1000，jitter = 0.5 * (1000 * 0.25) = 125 → 1125
    check(p.nextDelayMs() == 1125, "第1次 1000 + 125 抖动 = 1125");
    // 第2次 base=2000，jitter 同样 125 → 2125
    check(p.nextDelayMs() == 2125, "第2次 2000 + 125 = 2125");

    // jitter=0 时应与无抖动一致
    auto j0 = std::make_shared<FixedJitter>();
    j0->value = 0.0;
    ReconnectPolicy p2(1000, 30000, 0.25, j0);
    check(p2.nextDelayMs() == 1000, "jitter=0 → 无叠加");
}

} // namespace

int main()
{
    std::cout << "=== test_reconnect_policy ===" << std::endl;
    testExponentialCapped();
    testReset();
    testDisable();
    testJitter();

    if (g_failures == 0) {
        std::cout << "test_reconnect_policy PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_reconnect_policy FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
