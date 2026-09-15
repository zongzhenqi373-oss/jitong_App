// 数据库层压测工具（不走网络，直接驱动 imsrv::Database）。
//
// 场景：
//   A  写吞吐 + 延迟：单线程 saveMessage（走 DbWriteQueue 单写线程）
//   B  读吞吐 + 延迟：roamMessages 分页读（走连接池，poolSize 可配）
//   C  多提交者写：threads 个线程并发提交 saveMessage（验证单写队列稳定性 + 吞吐）
//   D  混合读写：1 写线程持续写 + readThreads 个读线程并发读（WAL 读写分离验证）
//
// 输出 CSV（每场景一行）：
//   scene,ops,total_ms,ops_per_s,p50_ms,p95_ms,p99_ms,max_ms
//
// 用法：
//   bench_db --db /tmp/bench.sqlite --total 100000 --pool 4 \
//            --size 1024 --threads 8 --read-threads 4 --iter 5000

#include "db/Database.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace imsrv;

namespace {

struct Args {
    std::string db = "/tmp/bench_db.sqlite";
    int total = 100000;   // 写场景总条数
    int pool = 4;         // 读连接池大小
    int size = 1024;      // 消息 body 字节数
    int threads = 8;      // 写场景线程数
    int readThreads = 4;  // 混合读写场景读线程数
    int readIter = 5000;  // 读场景/每读线程迭代次数
    int prefill = 0;      // 预填充条数（模拟已有数据规模，验证写吞吐衰减）
};

// 延迟分位数统计（线程安全）
class LatencyStats {
public:
    void add(double ms) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_samples.push_back(ms);
    }
    void mergeFrom(const LatencyStats& o) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_samples.insert(m_samples.end(), o.m_samples.begin(), o.m_samples.end());
    }
    size_t count() const {
        std::lock_guard<std::mutex> lk(m_mu);
        return m_samples.size();
    }
    double percentile(double p) const {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_samples.empty()) return 0.0;
        std::vector<double> v = m_samples;
        std::sort(v.begin(), v.end());
        const size_t idx = static_cast<size_t>(p * (v.size() - 1));
        return v[idx];
    }
    double max() const {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_samples.empty()) return 0.0;
        return *std::max_element(m_samples.begin(), m_samples.end());
    }

private:
    mutable std::mutex m_mu;
    std::vector<double> m_samples;
};

std::atomic<std::int64_t> g_msgSeq{0};

StoredMessage makeMessage(int sender, int receiver, int contentLen, const std::string& prefix)
{
    const std::int64_t n = g_msgSeq.fetch_add(1);
    StoredMessage m;
    m.msgId = prefix + std::to_string(n);
    m.senderId = sender;
    m.receiverId = receiver;
    m.type = 0;
    m.content = std::string(contentLen, 'a');
    m.ts = 1700000000000LL + n;
    return m;
}

struct BenchResult {
    std::string scene;
    long long ops = 0;
    double totalMs = 0;
    LatencyStats lat;
};

void printResult(const BenchResult& r)
{
    const double opsPerSec = r.totalMs > 0 ? (r.ops * 1000.0 / r.totalMs) : 0.0;
    std::cout << r.scene << "," << r.ops << "," << (long long)r.totalMs << ","
              << (long long)opsPerSec << "," << r.lat.percentile(0.50) << ","
              << r.lat.percentile(0.95) << "," << r.lat.percentile(0.99) << ","
              << r.lat.max() << "\n";
}

} // namespace

int main(int argc, char** argv)
{
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (k == "--db") a.db = next();
        else if (k == "--total") a.total = std::stoi(next());
        else if (k == "--pool") a.pool = std::stoi(next());
        else if (k == "--size") a.size = std::stoi(next());
        else if (k == "--threads") a.threads = std::stoi(next());
        else if (k == "--read-threads") a.readThreads = std::stoi(next());
        else if (k == "--read-iter") a.readIter = std::stoi(next());
        else if (k == "--prefill") a.prefill = std::stoi(next());
    }

    std::remove(a.db.c_str());

    Database db;
    if (!db.open(a.db, a.pool)) {
        std::cerr << "打开数据库失败: " << a.db << "\n";
        return 2;
    }
    db.seedIfEmpty();

    std::cout << "# bench_db db=" << a.db << " pool=" << a.pool << " size=" << a.size
              << " total=" << a.total << " threads=" << a.threads
              << " prefill=" << a.prefill << "\n";
    std::cout << "scene,ops,total_ms,ops_per_s,p50_ms,p95_ms,p99_ms,max_ms\n";

    // ---- 预填充（模拟已有数据规模，不计入统计） ----
    if (a.prefill > 0) {
        g_msgSeq.store(0);
        const int perThread = a.prefill / a.threads;
        const auto p0 = std::chrono::steady_clock::now();
        std::vector<std::thread> pts;
        for (int t = 0; t < a.threads; ++t) {
            pts.emplace_back([&, t, perThread]() {
                for (int i = 0; i < perThread; ++i) {
                    StoredMessage m = makeMessage(1, 2, a.size, "p" + std::to_string(t) + "-");
                    db.saveMessage(m, true);
                }
            });
        }
        for (auto& t : pts) t.join();
        const auto p1 = std::chrono::steady_clock::now();
        std::cout << "# prefill_ms="
                  << (long long)std::chrono::duration<double, std::milli>(p1 - p0).count()
                  << "\n";
    }

    // ---- 场景 A：单线程写（吞吐 + 延迟） ----
    {
        BenchResult r;
        r.scene = "A_save_single";
        r.ops = a.total;
        g_msgSeq.store(0);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < a.total; ++i) {
            StoredMessage m = makeMessage(1, 2, a.size, "a");
            const auto s0 = std::chrono::steady_clock::now();
            db.saveMessage(m, true);
            const auto s1 = std::chrono::steady_clock::now();
            r.lat.add(std::chrono::duration<double, std::milli>(s1 - s0).count());
        }
        const auto t1 = std::chrono::steady_clock::now();
        r.totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        printResult(r);
    }

    // ---- 场景 B：读吞吐（roamMessages 分页，limit=20） ----
    {
        BenchResult r;
        r.scene = "B_roam_read";
        r.ops = a.readIter;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < a.readIter; ++i) {
            const std::int64_t before = (i % 5000) * 20 + 21; // 伪随机深分页游标
            const auto s0 = std::chrono::steady_clock::now();
            db.roamMessages(1, 2, before, 20);
            const auto s1 = std::chrono::steady_clock::now();
            r.lat.add(std::chrono::duration<double, std::milli>(s1 - s0).count());
        }
        const auto t1 = std::chrono::steady_clock::now();
        r.totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        printResult(r);
    }

    // ---- 场景 C：多提交者写（验证单写队列稳定性 + 吞吐） ----
    {
        BenchResult r;
        r.scene = "C_save_multi";
        const int perThread = a.total / a.threads;
        r.ops = perThread * a.threads;
        g_msgSeq.store(0);
        std::vector<std::thread> ts;
        std::vector<LatencyStats> perThreadLat(a.threads);
        const auto t0 = std::chrono::steady_clock::now();
        for (int t = 0; t < a.threads; ++t) {
            ts.emplace_back([&, t, perThread]() {
                for (int i = 0; i < perThread; ++i) {
                    StoredMessage m = makeMessage(1, 2, a.size, "c" + std::to_string(t) + "-");
                    const auto s0 = std::chrono::steady_clock::now();
                    db.saveMessage(m, true);
                    const auto s1 = std::chrono::steady_clock::now();
                    perThreadLat[t].add(
                        std::chrono::duration<double, std::milli>(s1 - s0).count());
                }
            });
        }
        for (auto& t : ts) t.join();
        const auto t1 = std::chrono::steady_clock::now();
        r.totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        for (auto& l : perThreadLat) r.lat.mergeFrom(l);
        printResult(r);
    }

    // ---- 场景 D：混合读写（1 写线程 + readThreads 读线程） ----
    {
        BenchResult r;
        r.scene = "D_mixed_read";
        const int writes = a.readIter; // 写线程写 writes 条
        r.ops = a.readThreads * a.readIter; // 观察读吞吐
        g_msgSeq.store(0);
        std::atomic<bool> stop{false};

        // 写线程：持续写
        std::thread writer([&]() {
            for (int i = 0; i < writes; ++i) {
                StoredMessage m = makeMessage(1, 3, a.size, "d");
                db.saveMessage(m, true);
            }
            stop.store(true);
        });

        // 读线程：并发读
        std::vector<std::thread> readers;
        std::vector<LatencyStats> readLat(a.readThreads);
        const auto t0 = std::chrono::steady_clock::now();
        for (int t = 0; t < a.readThreads; ++t) {
            readers.emplace_back([&, t]() {
                int i = 0;
                while (!stop.load() && i < a.readIter) {
                    const auto s0 = std::chrono::steady_clock::now();
                    db.roamMessages(1, 2, 1000000, 20);
                    const auto s1 = std::chrono::steady_clock::now();
                    readLat[t].add(std::chrono::duration<double, std::milli>(s1 - s0).count());
                    ++i;
                }
            });
        }
        writer.join();
        for (auto& t : readers) t.join();
        const auto t1 = std::chrono::steady_clock::now();
        r.totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        r.ops = a.readThreads * a.readIter; // 读操作总数
        for (auto& l : readLat) r.lat.mergeFrom(l);
        printResult(r);
    }

    // ---- 场景 E：离线积压拉取（pullUndelivered，积压 10 万条单次拉取） ----
    {
        const int K = 100000;
        g_msgSeq.store(0);
        for (int i = 0; i < K; ++i) {
            StoredMessage m = makeMessage(1, 2, a.size, "off");
            db.saveMessage(m, false); // delivered=false，积压
        }
        const auto s0 = std::chrono::steady_clock::now();
        const auto msgs = db.pullUndelivered(2);
        const auto s1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(s1 - s0).count();
        // 输出：scene,ops(=拉取条数),total_ms,ops_per_s(=折算吞吐),p50,p95,p99,max
        std::cout << "E_pull_undelivered," << (long long)msgs.size() << "," << (long long)ms
                  << "," << (long long)(ms > 0 ? msgs.size() * 1000.0 / ms : 0)
                  << ",,,\n";
    }

    std::remove(a.db.c_str());
    return 0;
}
