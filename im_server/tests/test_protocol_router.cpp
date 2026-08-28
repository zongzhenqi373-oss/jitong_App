#include "routing/ProtocolRouter.h"

#include <cassert>
#include <stdexcept>
#include <string>

int main()
{
    imsrv::ProtocolRouter router;
    int calls = 0;
    std::string received;
    router.add(100, [&](const auto&, const std::string& payload) {
        ++calls;
        received = payload;
    });

    assert(router.size() == 1);
    assert(router.contains(100));
    assert(!router.contains(101));
    router.dispatch(nullptr, 100, "payload");
    assert(calls == 1);
    assert(received == "payload");

    // 未知协议必须被安全忽略，不能误调用已有处理器。
    router.dispatch(nullptr, 999, "ignored");
    assert(calls == 1);

    bool duplicateRejected = false;
    try {
        router.add(100, [](const auto&, const auto&) {});
    } catch (const std::logic_error&) {
        duplicateRejected = true;
    }
    assert(duplicateRejected);

    bool emptyRejected = false;
    try {
        router.add(200, {});
    } catch (const std::invalid_argument&) {
        emptyRejected = true;
    }
    assert(emptyRejected);

    // Handler 异常不得越过路由层导致业务线程 terminate。
    router.add(300, [](const auto&, const auto&) { throw std::runtime_error("boom"); });
    router.dispatch(nullptr, 300, "");
    return 0;
}
