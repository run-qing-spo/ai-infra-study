// bench.cpp — SPSC 队列性能基线
//
// 目的:
//   给 spsc_queue.hpp 顶部 "仍留待后续单独讨论" 那几项优化提供 baseline,
//   每动一项就跑同一个 bench 对比, 用数据决定要不要保留, 而不是凭直觉。
//   候选优化 (按预期收益排):
//     · alignas(64) 拆 head_ / tail_, 避免 false sharing
//     · 位运算替代 % cap_ (要求 cap 是 2 的幂)
//     · 生产者本地缓存 tail_, 消费者本地缓存 head_, 减少跨核 load
//
// 怎么编 / 跑:
//     make bench
//   编译器开关在 Makefile 里:
//     -O2  打开优化 (audit hooks 关掉, 拒绝调试干扰)
//
// 指标:
//   · ns/op       : 每次 "push 一个 + pop 一个" 端到端的纳秒数, 越小越好
//   · M ops/sec   : 每秒百万次完整传输 (= 1000 / ns_per_op)
//
// 测量方法:
//   · 1 个生产者 + 1 个消费者, 持续推 / 拉 N 个 int64
//   · 用 steady_clock 包住两个线程的 start..join 区间
//   · 每个 (cap) 跑 ROUNDS 轮, 取 **最小耗时**:
//       最小 ≈ 当时硬件实际能跑多快, 没有调度抖动 / 中断 / 其它进程干扰
//       平均会被偶发噪声拉高, 反而看不清优化的真实增量
//
// 防优化:
//   · 累加 checksum 并在结尾对账 (n-1)*n/2, 让 pop 的返回值不能被消掉
//   · 失败直接 exit 退出, 不让 bench 报出 "假快" 的数据

#include "spsc_queue.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

struct Result {
    double ns_per_op;
    double mops_per_sec;
};

static Result run_once(size_t cap, std::int64_t n) {
    spsc::SpscQueue<std::int64_t> q(cap);
    std::int64_t checksum = 0;

    auto start = Clock::now();

    std::thread prod([&] {
        for (std::int64_t i = 0; i < n; ++i) {
            while (!q.push(i)) {
                // busy spin: bench 测的就是无睡眠的极限吞吐
            }
        }
    });

    std::thread cons([&] {
        std::int64_t v = 0;
        for (std::int64_t i = 0; i < n; ++i) {
            while (!q.pop(v)) {}
            checksum += v; // 副作用, 防 -O2 把 pop 链路整个删掉
        }
    });

    prod.join();
    cons.join();

    auto end = Clock::now();

    // 对账: 0 + 1 + ... + (n-1) = n*(n-1)/2
    const std::int64_t expected = (n - 1) * n / 2;
    if (checksum != expected) {
        std::fprintf(stderr,
                     "BENCH FAILED (cap=%zu, n=%lld): checksum=%lld expected=%lld\n",
                     cap, (long long)n, (long long)checksum, (long long)expected);
        std::exit(1);
    }

    double ns = std::chrono::duration<double, std::nano>(end - start).count();
    double ns_per_op = ns / static_cast<double>(n);
    double mops_per_sec = 1000.0 / ns_per_op; // ns/op → M ops/sec
    return {ns_per_op, mops_per_sec};
}

static Result run_best(size_t cap, std::int64_t n, int rounds) {
    Result best{1e18, 0.0};
    for (int r = 0; r < rounds; ++r) {
        Result cur = run_once(cap, n);
        if (cur.ns_per_op < best.ns_per_op) best = cur;
    }
    return best;
}

int main() {
    constexpr std::int64_t N = 10'000'000;
    constexpr int ROUNDS = 5;

    // cap 选了 4 个量级: 极小 (高争用, 几乎每次都满 / 空)
    //   → 中 → 大 → 很大 (基本不打满, 主要测裸 atomic 路径)
    const std::vector<size_t> caps = {4, 64, 1024, 65536};

    std::printf("SPSC bench  |  N=%lld per run  |  %d rounds, best taken\n",
                (long long)N, ROUNDS);
    std::printf("%-10s %-14s %-14s\n", "cap", "ns/op", "M ops/sec");
    std::printf("----------------------------------------\n");
    for (size_t cap : caps) {
        Result r = run_best(cap, N, ROUNDS);
        std::printf("%-10zu %-14.2f %-14.2f\n", cap, r.ns_per_op, r.mops_per_sec);
    }
    return 0;
}
