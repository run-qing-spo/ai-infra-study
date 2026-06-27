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
#include <string>
#include <thread>
#include <utility>
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

// ============================================================
// 大对象路径 (std::string, 远超 SSO 阈值) — 对比 copy vs move
// ============================================================
//
// 目的: int64 是 trivial 类型, move 退化为 copy, 看不出 move 的真实收益.
// 用 std::string(payload>SSO) 作为 T:
//   copy 路径: 每次 push 都做一次堆内容深拷贝, pop 再深拷贝出来
//   move 路径: push 把堆指针交给槽位 (源置空), pop 再把指针交给 out
// 预期 move 把每次 push/pop 周期里的 2 次 memcpy + 0~2 次 malloc 都省掉.
//
// 注意: 每轮生产者都 new 一个新 string (无论 copy / move 都得有源对象),
// 这次 alloc 的开销是两条路径共有的, 不计入 "copy vs move" 的差异.

template <bool UseMove>
static Result run_once_str(size_t cap, std::int64_t n, size_t payload) {
    spsc::SpscQueue<std::string> q(cap);
    std::size_t total = 0;

    auto start = Clock::now();

    std::thread prod([&] {
        for (std::int64_t i = 0; i < n; ++i) {
            std::string s(payload, 'x'); // 每轮都新分配, 两条路径共有的开销
            if constexpr (UseMove) {
                while (!q.push(std::move(s))) {}
            } else {
                while (!q.push(s)) {} // 左值 → 走 const T& 重载, 深拷贝
            }
        }
    });

    std::thread cons([&] {
        std::string out;
        for (std::int64_t i = 0; i < n; ++i) {
            while (!q.pop(out)) {}
            total += out.size(); // 防优化 + 对账依据
        }
    });

    prod.join();
    cons.join();

    auto end = Clock::now();

    const std::size_t expected = static_cast<std::size_t>(n) * payload;
    if (total != expected) {
        std::fprintf(stderr,
                     "BENCH FAILED (str, cap=%zu, n=%lld, %s): total=%zu expected=%zu\n",
                     cap, (long long)n, UseMove ? "move" : "copy", total, expected);
        std::exit(1);
    }

    double ns = std::chrono::duration<double, std::nano>(end - start).count();
    double ns_per_op = ns / static_cast<double>(n);
    double mops_per_sec = 1000.0 / ns_per_op;
    return {ns_per_op, mops_per_sec};
}

template <bool UseMove>
static Result run_best_str(size_t cap, std::int64_t n, size_t payload, int rounds) {
    Result best{1e18, 0.0};
    for (int r = 0; r < rounds; ++r) {
        Result cur = run_once_str<UseMove>(cap, n, payload);
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

    std::printf("SPSC bench  |  %d rounds, best taken\n", ROUNDS);

    std::printf("\n== int64 (trivial: move 退化为 copy)  N=%lld ==\n", (long long)N);
    std::printf("%-10s %-14s %-14s\n", "cap", "ns/op", "M ops/sec");
    std::printf("----------------------------------------\n");
    for (size_t cap : caps) {
        Result r = run_best(cap, N, ROUNDS);
        std::printf("%-10zu %-14.2f %-14.2f\n", cap, r.ns_per_op, r.mops_per_sec);
    }

    // 大对象路径: N 缩小 10 倍, 每次都涉及堆分配, 跑 10M 会非常慢且 heap 噪声大.
    // payload=256 远超 libc++/libstdc++ 的 SSO 阈值 (15/22 字节), 保证走堆.
    constexpr std::int64_t N_STR = 1'000'000;
    constexpr size_t kPayload = 256;
    const std::vector<size_t> caps_str = {64, 1024};

    std::printf("\n== std::string(payload=%zu)  N=%lld  copy vs move ==\n",
                kPayload, (long long)N_STR);
    std::printf("%-10s %-14s %-14s %-10s\n",
                "cap", "copy ns/op", "move ns/op", "speedup");
    std::printf("------------------------------------------------\n");
    for (size_t cap : caps_str) {
        Result cpy = run_best_str<false>(cap, N_STR, kPayload, ROUNDS);
        Result mov = run_best_str<true>(cap, N_STR, kPayload, ROUNDS);
        double speedup = cpy.ns_per_op / mov.ns_per_op;
        std::printf("%-10zu %-14.2f %-14.2f %.2fx\n",
                    cap, cpy.ns_per_op, mov.ns_per_op, speedup);
    }
    return 0;
}
