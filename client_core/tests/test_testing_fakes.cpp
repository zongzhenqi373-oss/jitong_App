// P7-G1 Harness 自检：Fake 基础设施本身必须可用且确定性。
//
// 验收点（对应 v2 §3 P7-G1）：
//   - 测试不依赖真实 sleep（时间由 DeterministicClock/TestScheduler 控制）；
//   - 随机值可复现（同 seed 同序列）；
//   - 故障可注入（磁盘满、IO 错误、MAC 损坏、镜像丢失）；
//   - fsync 可计数（供 ADR-03 强杀恢复矩阵使用）。

#include "client_core/testing/DeterministicClock.h"
#include "client_core/testing/FakeCodec.h"
#include "client_core/testing/FakeFileSystem.h"
#include "client_core/testing/FakeSecureKv.h"
#include "client_core/testing/FakeTransport.h"
#include "client_core/testing/SeededRandom.h"
#include "client_core/testing/TestScheduler.h"

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

using namespace im::testing;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& name)
{
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << std::endl;
    if (!cond) ++g_failures;
}
} // namespace

int main()
{
    std::cout << "=== test_testing_fakes ===" << std::endl;

    // [1] DeterministicClock：时间由测试控制，不依赖真实时钟
    {
        DeterministicClock clock(1000);
        check(clock.nowMs() == 1000, "初始时间 1000");
        clock.advance(500);
        check(clock.nowMs() == 1500, "advance(500) 后为 1500");
        clock.advance(-100);
        check(clock.nowMs() == 1500, "负向 advance 不回退（单调）");
        // 不推进则时间不变：证明不依赖真实时间流逝
        const auto t1 = clock.nowMs();
        const auto t2 = clock.nowMs();
        check(t1 == t2, "不推进时时间不变（不依赖真实时钟）");
    }

    // [2] TestScheduler：虚拟时间调度，无真实等待
    {
        DeterministicClock clock(0);
        TestScheduler sched(&clock);
        int ran = 0;

        sched.post([&]() { ++ran; });
        check(sched.pendingCount() == 1, "post 后有 1 个待执行任务");
        sched.runDue();
        check(ran == 1, "立即任务在 runDue 后执行");

        sched.postDelayed(100, [&]() { ++ran; });
        sched.advance(50);
        check(ran == 1, "advance(50) 未到期，任务未执行");
        sched.advance(50);
        check(ran == 2, "advance 到 100 后任务执行");

        // cancel
        const auto id = sched.postDelayed(100, [&]() { ++ran; });
        check(sched.cancel(id), "cancel 成功");
        sched.advance(200);
        check(ran == 2, "已取消任务不再执行");

        // 任务内 post 的新任务同批次执行
        int inner = 0;
        sched.post([&]() {
            sched.post([&]() { ++inner; });
        });
        sched.runDue();
        check(inner == 1, "任务内新 post 的任务同批次执行");

        // 真实耗时应极短（证明无真实 sleep）
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 1000; ++i) sched.postDelayed(1, []() {});
        sched.advance(1000);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        std::cout << "      1000 个虚拟定时任务耗时 " << ms << "ms" << std::endl;
        check(ms < 1000, "1000 个虚拟定时任务不产生真实等待（<1s）");
    }

    // [3] SeededRandom：同 seed 必同序列
    {
        SeededRandom r1(42);
        SeededRandom r2(42);
        bool same = true;
        for (int i = 0; i < 16; ++i) {
            if (r1.nextU64() != r2.nextU64()) same = false;
        }
        check(same, "同 seed 产生相同 U64 序列");

        SeededRandom r3(42);
        SeededRandom r4(42);
        check(r3.nextHex(16) == r4.nextHex(16), "同 seed 产生相同 hex（msg_id 可复现）");

        SeededRandom r5(7);
        SeededRandom r6(8);
        check(r5.nextU64() != r6.nextU64(), "不同 seed 序列不同");
    }

    // [4] FakeFileSystem：fsync 计数 + 故障注入
    {
        FakeFileSystem fs;
        const std::vector<unsigned char> data{1, 2, 3, 4};
        check(fs.write("/a.bin", data), "写入成功");
        check(fs.exists("/a.bin"), "文件存在");
        check(fs.size("/a.bin") == 4, "大小正确");

        std::vector<unsigned char> out;
        check(fs.read("/a.bin", &out) && out == data, "读回内容一致");

        fs.fsync("/a.bin");
        fs.fsync("/a.bin");
        check(fs.fsyncCount("/a.bin") == 2, "fsync 计数为 2");

        // 磁盘满
        fs.setDiskFull(true);
        check(!fs.write("/b.bin", data), "磁盘满时写入失败");
        fs.setDiskFull(false);

        // 原子替换
        check(fs.write("/tmp.bin", data), "写临时文件");
        check(fs.renameReplace("/tmp.bin", "/final.bin"), "renameReplace 成功");
        check(fs.exists("/final.bin") && !fs.exists("/tmp.bin"), "替换后源不存在、目标存在");
        check(!fs.renameReplace("/nope.bin", "/x.bin"), "源不存在时替换失败");
    }

    // [5] FakeSecureKv：MAC 镜像与 fail-close（ADR-03）
    {
        FakeSecureKv kv;
        kv.put("journal", "PREPARED");
        std::string v;
        check(kv.get("journal", &v) && v == "PREPARED", "基本 KV 读写");

        kv.putMirrored("journal", "PREPARED", "mac-123");
        auto r1 = kv.getMirrored("journal", "mac-123", &v);
        check(r1 == FakeSecureKv::MirrorResult::Ok, "MAC 一致 → Ok");

        auto r2 = kv.getMirrored("journal", "mac-wrong", &v);
        check(r2 == FakeSecureKv::MirrorResult::MacMismatch, "MAC 不一致 → MacMismatch");

        kv.corruptMac("journal");
        auto r3 = kv.getMirrored("journal", "mac-123", &v);
        check(r3 == FakeSecureKv::MirrorResult::MacMismatch, "MAC 被损坏 → MacMismatch");

        kv.dropMirror("journal");
        auto r4 = kv.getMirrored("journal", "mac-123", &v);
        check(r4 == FakeSecureKv::MirrorResult::Missing, "镜像丢失 → Missing");
    }

    // [6] FakeCodec：确定性 + 格式如实反映
    {
        FakeCodec codec;
        EncodeRequest req;
        req.srcW = 4000;
        req.srcH = 3000;
        req.targetW = 640;
        req.targetH = 480;
        req.quality = 80;
        req.mime = "image/avif";

        const auto a = codec.encode(req);
        const auto b = codec.encode(req);
        check(a.ok && b.ok, "编码成功");
        check(a.bytes == b.bytes, "同参数编码输出确定一致");

        // 关键：禁止 JPEG 字节标成 AVIF
        EncodeRequest jpegReq = req;
        jpegReq.mime = "image/jpeg";
        const auto j = codec.encode(jpegReq);
        check(j.actualMime == "image/jpeg", "jpeg 请求如实返回 image/jpeg（不冒充 avif）");
        check(a.actualMime == "image/avif", "avif 请求返回 image/avif");

        EncodeRequest bad = req;
        bad.mime = "image/png";
        check(!codec.encode(bad).ok, "不支持的格式编码失败");

        check(codec.encodeCalls() == 4, "编码调用计数正确");
    }

    // [7] FakeTransport：发送记录 + 注入接收 + 故障
    {
        FakeTransport tr;
        check(tr.connect(), "连接成功");
        check(tr.send(1002, "login-payload"), "发送帧成功");
        check(tr.send(1010, "hb"), "发送第二帧");
        check(tr.sentCount() == 2, "记录 2 帧");
        check(tr.sentType(1002), "发出过 1002");
        check(tr.countType(1010) == 1, "1010 发出 1 次");

        // 注入入站
        std::uint32_t gotType = 0;
        std::string gotPayload;
        tr.setReceiveHandler([&](std::uint32_t t, const std::string& p) {
            gotType = t;
            gotPayload = p;
        });
        tr.injectInbound(1003, "login-rs");
        check(gotType == 1003 && gotPayload == "login-rs", "入站注入回调正确");

        // IO 错误注入
        tr.setIoError(true);
        check(!tr.send(1002, "x"), "IO 错误时发送失败");
        tr.setIoError(false);

        // 未连接时发送失败
        tr.disconnect();
        check(!tr.send(1002, "x"), "未连接时发送失败");
    }

    if (g_failures == 0) {
        std::cout << "test_testing_fakes PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_testing_fakes FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
