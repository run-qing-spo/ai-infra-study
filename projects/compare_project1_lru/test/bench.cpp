// Layer 4: 性能基准
//
// 用法: make bench
// 编译选项: -O2,不带 sanitizer / LRU_TEST_HOOKS。
//   - sanitizer: 真的会插桩拖慢热路径,必须关。
//   - LRU_TEST_HOOKS: 当前只门控 audit() 方法,bench 没调用,关不关对当前数据没差;
//     关掉是防御性纪律 —— 防止未来 audit 类钩子被加进热路径后悄悄污染 bench。
//
// 报告三组数据:
//   1) 单线程吞吐: lru_base vs lru_mutex (看 mutex 净开销)
//   2) lru_mutex 多线程扩展: 1/2/4/8 线程的吞吐曲线
//   3) lru_mutex 单线程延迟分位数: p50/p95/p99/p999
//
// 注意事项:
//   * steady_clock::now() 调用本身有 ~20-40ns 开销,延迟绝对值偏高。
//     数字只用于横向对比(同一台机器同一次跑)。
//   * 多次跑会有 5-15% 波动,得出结论前看几次。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "lru_base.hpp"
#include "lru_mutex.hpp"
#include "lru_sharded.hpp"

namespace {

struct Workload {
    const char* name;
    int get_pct;    // 0-100
    int push_pct;   // 0-100,三者和为 100
    int erase_pct;
};

constexpr Workload kWorkloads[] = {
    {"read-heavy",  95,  5,  0},
    {"balanced",    50, 30, 20},
    {"write-heavy", 10, 70, 20},
};

template <typename RNG>
int pick_op(RNG& rng, const Workload& w) {
    int r = static_cast<int>(rng() % 100);
    if (r < w.get_pct) return 0;
    if (r < w.get_pct + w.push_pct) return 1;
    return 2;
}

struct ThroughputResult {
    uint64_t ops;
    uint64_t hits;
    uint64_t gets;
    double seconds;
    double ops_per_sec() const { return ops / seconds; }
    double hit_rate() const { return gets > 0 ? static_cast<double>(hits) / gets : 0.0; }
};

template <typename Cache>
void warm_up(Cache& cache, int capacity, int key_space) {
    std::mt19937 rng(0xc0ffee);
    for (int i = 0; i < capacity * 2; ++i) {
        int k = static_cast<int>(rng() % key_space);
        cache.push(k, std::make_shared<int>(k));
    }
}

template <typename Cache>
ThroughputResult run_throughput_single(
    int capacity, int key_space, const Workload& w,
    std::chrono::milliseconds duration)
{
    Cache cache(capacity);
    warm_up(cache, capacity, key_space);

    std::mt19937 rng(42);
    uint64_t ops = 0, hits = 0, gets = 0;
    constexpr int batch = 64;  // 减少 now() 调用频率
    auto start = std::chrono::steady_clock::now();
    auto deadline = start + duration;
    while (true) {
        for (int i = 0; i < batch; ++i) {
            int k = static_cast<int>(rng() % key_space);
            int op = pick_op(rng, w);
            switch (op) {
                case 0: {
                    auto sp = cache.get(k);
                    ++gets;
                    if (sp) ++hits;
                    break;
                }
                case 1:
                    cache.push(k, std::make_shared<int>(k));
                    break;
                case 2:
                    cache.erase(k);
                    break;
            }
        }
        ops += batch;
        if (std::chrono::steady_clock::now() >= deadline) break;
    }
    auto end = std::chrono::steady_clock::now();
    return {ops, hits, gets, std::chrono::duration<double>(end - start).count()};
}

// 多线程吞吐核心实现。模板化是为了同时给 lru_mutex 和 lru_sharded 用,
// 两者构造签名不同(一个 cap,一个 cap+shard_count),所以由外层 wrapper 负责构造,
// 这里只接 cache&。
template <typename Cache>
ThroughputResult run_throughput_multi_impl(
    Cache& cache,
    int capacity, int key_space, const Workload& w,
    std::chrono::milliseconds duration, int n_threads)
{
    warm_up(cache, capacity, key_space);

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::vector<uint64_t> per_ops(n_threads, 0);
    std::vector<uint64_t> per_hits(n_threads, 0);
    std::vector<uint64_t> per_gets(n_threads, 0);
    std::vector<std::thread> ts;
    ts.reserve(n_threads);

    for (int i = 0; i < n_threads; ++i) {
        ts.emplace_back([&, i]{
            std::mt19937 rng(static_cast<uint32_t>(i + 1));
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) { /* spin */ }

            uint64_t ops = 0, hits = 0, gets = 0;
            constexpr int batch = 64;
            while (!stop.load(std::memory_order_relaxed)) {
                for (int j = 0; j < batch; ++j) {
                    int k = static_cast<int>(rng() % key_space);
                    int op = pick_op(rng, w);
                    switch (op) {
                        case 0: {
                            auto sp = cache.get(k);
                            ++gets;
                            if (sp) ++hits;
                            break;
                        }
                        case 1:
                            cache.push(k, std::make_shared<int>(k));
                            break;
                        case 2:
                            cache.erase(k);
                            break;
                    }
                }
                ops += batch;
            }
            per_ops[i] = ops;
            per_hits[i] = hits;
            per_gets[i] = gets;
        });
    }

    while (ready.load(std::memory_order_acquire) < n_threads) { /* spin */ }
    auto start = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    std::this_thread::sleep_for(duration);
    stop.store(true, std::memory_order_relaxed);
    auto end = std::chrono::steady_clock::now();
    for (auto& t : ts) t.join();

    uint64_t ops = 0, hits = 0, gets = 0;
    for (int i = 0; i < n_threads; ++i) {
        ops += per_ops[i];
        hits += per_hits[i];
        gets += per_gets[i];
    }
    return {ops, hits, gets, std::chrono::duration<double>(end - start).count()};
}

// 单一互斥锁版的多线程吞吐
ThroughputResult run_throughput_multi(
    int capacity, int key_space, const Workload& w,
    std::chrono::milliseconds duration, int n_threads)
{
    lru_mutex::lrucache_mutex<int, int> cache(capacity);
    return run_throughput_multi_impl(cache, capacity, key_space, w, duration, n_threads);
}

// 分片版的多线程吞吐
ThroughputResult run_throughput_multi_sharded(
    int capacity, int shard_count, int key_space, const Workload& w,
    std::chrono::milliseconds duration, int n_threads)
{
    lru_sharded::lrucache_sharded<int, int> cache(
        static_cast<std::size_t>(capacity), static_cast<std::size_t>(shard_count));
    return run_throughput_multi_impl(cache, capacity, key_space, w, duration, n_threads);
}

struct LatencyResult {
    std::vector<uint64_t> get_ns;
    std::vector<uint64_t> push_ns;
    std::vector<uint64_t> erase_ns;
};

LatencyResult run_latency_single(
    int capacity, int key_space, const Workload& w, int total_iters)
{
    lru_mutex::lrucache_mutex<int, int> cache(capacity);
    warm_up(cache, capacity, key_space);

    LatencyResult res;
    // 预分配防止 push_back 触发 realloc 干扰计时
    res.get_ns.reserve(static_cast<size_t>(total_iters * w.get_pct / 100 + 1024));
    res.push_ns.reserve(static_cast<size_t>(total_iters * w.push_pct / 100 + 1024));
    res.erase_ns.reserve(static_cast<size_t>(total_iters * w.erase_pct / 100 + 1024));

    std::mt19937 rng(42);
    for (int i = 0; i < total_iters; ++i) {
        int k = static_cast<int>(rng() % key_space);
        int op = pick_op(rng, w);
        switch (op) {
            case 0: {
                auto t0 = std::chrono::steady_clock::now();
                auto sp = cache.get(k);
                auto t1 = std::chrono::steady_clock::now();
                res.get_ns.push_back(static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
                (void)sp;
                break;
            }
            case 1: {
                auto sp_arg = std::make_shared<int>(k);  // 分配排除在计时外
                auto t0 = std::chrono::steady_clock::now();
                cache.push(k, std::move(sp_arg));
                auto t1 = std::chrono::steady_clock::now();
                res.push_ns.push_back(static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
                break;
            }
            case 2: {
                auto t0 = std::chrono::steady_clock::now();
                cache.erase(k);
                auto t1 = std::chrono::steady_clock::now();
                res.erase_ns.push_back(static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
                break;
            }
        }
    }
    return res;
}

struct Percentiles { uint64_t p50, p95, p99, p999; };

Percentiles compute_percentiles(std::vector<uint64_t>& samples) {
    if (samples.empty()) return {0, 0, 0, 0};
    std::sort(samples.begin(), samples.end());
    auto pick = [&](double q) {
        size_t idx = std::min(samples.size() - 1, static_cast<size_t>(q * samples.size()));
        return samples[idx];
    };
    return {pick(0.50), pick(0.95), pick(0.99), pick(0.999)};
}

}  // namespace

int main() {
    constexpr int capacity = 1024;
    constexpr int key_space = 2048;       // 2x capacity → 稳态命中率约 50%
    constexpr auto duration = std::chrono::milliseconds(1500);
    constexpr int latency_iters = 1'000'000;

    std::cout << "=== Layer 4: Performance ===\n"
              << "build: -O2, no sanitizers, no LRU_TEST_HOOKS\n"
              << "capacity=" << capacity << " key_space=" << key_space
              << " duration=" << duration.count() << "ms\n\n";

    // --- 1. 单线程吞吐:base vs mutex ---
    std::cout << "=== Throughput (single thread, lru_base vs lru_mutex) ===\n";
    std::cout << std::left
              << std::setw(14) << "workload"
              << std::right
              << std::setw(14) << "base (M/s)"
              << std::setw(14) << "mutex (M/s)"
              << std::setw(14) << "mutex cost"
              << std::setw(12) << "hit rate"
              << "\n";
    for (const auto& w : kWorkloads) {
        auto b = run_throughput_single<lru_base::lrucache_base<int, int>>(
            capacity, key_space, w, duration);
        auto m = run_throughput_single<lru_mutex::lrucache_mutex<int, int>>(
            capacity, key_space, w, duration);
        double b_mps = b.ops_per_sec() / 1e6;
        double m_mps = m.ops_per_sec() / 1e6;
        double overhead = (b_mps > 0) ? (1.0 - m_mps / b_mps) * 100.0 : 0;
        std::cout << std::left << std::setw(14) << w.name
                  << std::right << std::fixed << std::setprecision(2)
                  << std::setw(14) << b_mps
                  << std::setw(14) << m_mps
                  << std::setw(13) << std::setprecision(1) << overhead << "%"
                  << std::setw(11) << (m.hit_rate() * 100) << "%"
                  << "\n";
    }

    // --- 2. lru_mutex 多线程扩展 ---
    std::cout << "\n=== Throughput (lru_mutex, multi-thread scaling) ===\n";
    std::cout << std::left << std::setw(14) << "workload"
              << std::right << std::setw(9) << "threads"
              << std::setw(14) << "total (M/s)"
              << std::setw(15) << "per-thread"
              << std::setw(11) << "speedup"
              << std::setw(12) << "hit rate"
              << "\n";
    constexpr int thread_counts[] = {1, 2, 4, 8};
    for (const auto& w : kWorkloads) {
        double single_mps = 0;
        for (int nt : thread_counts) {
            auto r = run_throughput_multi(capacity, key_space, w, duration, nt);
            double mps = r.ops_per_sec() / 1e6;
            if (nt == 1) single_mps = mps;
            double speedup = (single_mps > 0) ? mps / single_mps : 0;
            std::cout << std::left << std::setw(14) << w.name
                      << std::right << std::setw(9) << nt
                      << std::fixed << std::setprecision(2)
                      << std::setw(14) << mps
                      << std::setw(15) << (mps / nt)
                      << std::setw(10) << speedup << "x"
                      << std::setw(11) << std::setprecision(1) << (r.hit_rate() * 100) << "%"
                      << "\n";
        }
    }

    // --- 3. lru_sharded 分片数扫描 ---
    // 固定 8 线程,扫 shard ∈ {1, 2, 4, 8, 16, 32, 64, 128}。
    // 预期:
    //   * shards=1 ≈ lru_mutex 8 线程基线(略低,因为多了 mod 和函数转发开销)
    //   * shards 增加 → 吞吐爬升,直到 shard 数 ≫ 线程数后饱和
    //   * write-heavy 改善幅度最大(临界区最长,锁竞争最显著)
    //   * 命中率随 shards 上升而略降 —— 全局容量被切碎,热点 key 簇聚提前 evict
    std::cout << "\n=== Sharded scaling (8 threads, varying shard count) ===\n";
    std::cout << std::left << std::setw(14) << "workload"
              << std::right << std::setw(9) << "shards"
              << std::setw(14) << "total (M/s)"
              << std::setw(12) << "hit rate"
              << "\n";
    constexpr int shard_counts[] = {1, 2, 4, 8, 16, 32, 64, 128};
    constexpr int sharded_threads = 8;
    for (const auto& w : kWorkloads) {
        for (int sc : shard_counts) {
            auto r = run_throughput_multi_sharded(
                capacity, sc, key_space, w, duration, sharded_threads);
            double mps = r.ops_per_sec() / 1e6;
            std::cout << std::left << std::setw(14) << w.name
                      << std::right << std::setw(9) << sc
                      << std::fixed << std::setprecision(2)
                      << std::setw(14) << mps
                      << std::setw(11) << std::setprecision(1) << (r.hit_rate() * 100) << "%"
                      << "\n";
        }
    }

    // --- 4. 延迟分位数 (lru_mutex, 单线程, balanced) ---
    std::cout << "\n=== Latency percentiles (lru_mutex, single thread, balanced) ===\n";
    std::cout << "iters: " << latency_iters << "\n";
    auto lat = run_latency_single(capacity, key_space, kWorkloads[1], latency_iters);
    auto print_lat = [](const char* name, std::vector<uint64_t>& samples) {
        auto p = compute_percentiles(samples);
        std::cout << std::left << std::setw(8) << name
                  << " n=" << std::right << std::setw(7) << samples.size()
                  << "  p50=" << std::setw(5) << p.p50 << "ns"
                  << "  p95=" << std::setw(5) << p.p95 << "ns"
                  << "  p99=" << std::setw(6) << p.p99 << "ns"
                  << "  p999=" << std::setw(6) << p.p999 << "ns"
                  << "\n";
    };
    print_lat("get", lat.get_ns);
    print_lat("push", lat.push_ns);
    print_lat("erase", lat.erase_ns);

    return 0;
}
