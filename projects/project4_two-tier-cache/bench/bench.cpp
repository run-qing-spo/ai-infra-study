// P4 benchmark:对比 DRAM-only cache 和 DRAM+SSD tiered cache。
//
// 目的不是"跑分",是拿到"命中率提升 vs 访问延迟代价"的定量数据
// 讨论"为啥要做两级"时能有个数字支撑,而不是纯理论。
//
// Workload:Zipf(theta) 访问序列(N 个唯一 id,M 次访问)。
//   theta ≈ 1 是典型 KV cache / web hot data 分布(小部分 id 承担多数访问)。
//   theta 越大越极端,tier 的价值越小(热数据全塞进 L1 就够);
//   theta 越小越均匀,tier 的价值越显著(L2 才有空间承接长尾)。
//
// 对比设定 —— 三组一起看才能讲清 tier 的适用场景:
//   A. DRAM-tight:    只有 50 blocks 的 DRAM 预算(现实约束)
//   B. Tiered:        50 blocks DRAM (同 A 预算) + 50 blocks SSD 扩容
//   C. DRAM-luxury:   假设有 100 blocks DRAM 预算(理想上限)
//
// 期望:
//   - hit_rate 排序:C ≥ B > A (tier 用便宜的 SSD 把命中率往 C 靠)
//   - mean_lat 排序:C < A < B (tier 换命中率的代价是 IO 延迟)
//   - 结论:tier 不是"跟 DRAM 比谁快",是"DRAM 装不下时用 SSD 兜住,
//     以延迟换命中率"。面试问"tier 什么时候划算"就答工作集 >> DRAM 的时候。
//
// 单次 op:先 get,miss 则模拟"生产数据"并 put 回去。这是典型 look-aside
// cache 使用模式(vLLM prefix cache 之类都是这个 shape)。

#include "cache.hpp"
#include "dram_block_store.hpp"
#include "lru_policy.hpp"
#include "ssd_block_store.hpp"
#include "tiered_cache.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace p4;
using clk = std::chrono::steady_clock;

namespace {

// Zipfian 生成器:前缀和 CDF + 二分。O(N) 预算,O(logN) 每次采样。
// N ~ 数千级用这个足够;更大规模才需要 rejection sampling 类的 O(1) 方法。
class ZipfGen {
public:
    ZipfGen(size_t n, double theta, uint64_t seed)
        : cdf_(n), rng_(seed) {
        double sum = 0.0;
        for (size_t i = 0; i < n; ++i) {
            sum += 1.0 / std::pow(static_cast<double>(i + 1), theta);
            cdf_[i] = sum;
        }
        for (auto& c : cdf_) c /= sum;
    }
    size_t next() {
        double u = std::uniform_real_distribution<double>(0.0, 1.0)(rng_);
        auto it = std::lower_bound(cdf_.begin(), cdf_.end(), u);
        return static_cast<size_t>(it - cdf_.begin());
    }
private:
    std::vector<double> cdf_;
    std::mt19937_64     rng_;
};

// 用 id 生成"数据"内容,跟 smoke 里一样,便于验证 hit 拿到的确实是那个 id 的字节
void fill_block(std::vector<std::byte>& buf, BlockId id) {
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<std::byte>((id * 31 + i) & 0xff);
    }
}

struct Stats {
    size_t   hits   = 0;
    size_t   misses = 0;
    double   total_ms = 0.0;
    std::vector<uint64_t> lat_ns;   // per-op

    double hit_rate() const {
        return double(hits) / double(hits + misses);
    }
    // 直接改动 lat_ns(会排序),bench 结束后不再用
    uint64_t percentile(double p) {
        if (lat_ns.empty()) return 0;
        std::sort(lat_ns.begin(), lat_ns.end());
        size_t idx = static_cast<size_t>(p * (lat_ns.size() - 1));
        return lat_ns[idx];
    }
    double mean_us() const {
        if (lat_ns.empty()) return 0;
        uint64_t sum = 0;
        for (auto v : lat_ns) sum += v;
        return double(sum) / lat_ns.size() / 1000.0;
    }
};

// GetFn: bool(BlockId, std::byte*)
// PutFn: void(BlockId, const std::byte*)
template<typename GetFn, typename PutFn>
Stats run_workload(const std::vector<BlockId>& trace,
                   size_t block_size,
                   GetFn get_fn, PutFn put_fn) {
    Stats st;
    st.lat_ns.reserve(trace.size());
    std::vector<std::byte> buf(block_size);
    std::vector<std::byte> out(block_size);

    auto t0 = clk::now();
    for (BlockId id : trace) {
        auto op_t0 = clk::now();
        if (get_fn(id, out.data())) {
            ++st.hits;
        } else {
            ++st.misses;
            fill_block(buf, id);      // 模拟"从下游生产 4KB 数据"
            put_fn(id, buf.data());
        }
        auto op_t1 = clk::now();
        st.lat_ns.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(op_t1 - op_t0).count());
    }
    auto t1 = clk::now();
    st.total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return st;
}

void print_row(const char* name, Stats& s) {
    std::printf("%-16s  hit=%6.2f%%  total=%8.2f ms  mean=%7.2f us  p50=%7.2f us  p95=%7.2f us  p99=%8.2f us\n",
                name,
                s.hit_rate() * 100,
                s.total_ms,
                s.mean_us(),
                s.percentile(0.50) / 1000.0,
                s.percentile(0.95) / 1000.0,
                s.percentile(0.99) / 1000.0);
}

} // namespace

int main() {
    // ── 配置 ────────────────────────────────────────────────
    constexpr size_t kBlockSize    = 4096;
    constexpr size_t kDramTight    = 50;       // 现实 DRAM 预算
    constexpr size_t kL1Cap        = 50;       // tiered: DRAM 同预算
    constexpr size_t kL2Cap        = 50;       // tiered: SSD 扩容
    constexpr size_t kDramLuxury   = 100;      // 理想 DRAM 预算(≈ tier 总容量)
    constexpr size_t kUniqueIds    = 1000;     // 工作集 10x 于 tight
    constexpr size_t kNumOps       = 100'000;
    constexpr double kTheta        = 0.99;
    constexpr uint64_t kSeed       = 42;

    // 生成 trace(两轮实验用同一份,保证公平)
    std::vector<BlockId> trace;
    trace.reserve(kNumOps);
    {
        ZipfGen zipf(kUniqueIds, kTheta, kSeed);
        for (size_t i = 0; i < kNumOps; ++i) {
            trace.push_back(static_cast<BlockId>(zipf.next()));
        }
    }

    std::printf("workload: N=%zu unique, M=%zu ops, zipf theta=%.2f, block=%zu B\n",
                kUniqueIds, kNumOps, kTheta, kBlockSize);
    std::printf("layouts:  A(DRAM-tight)=%zu   B(Tiered L1=%zu+L2=%zu)   C(DRAM-luxury)=%zu\n\n",
                kDramTight, kL1Cap, kL2Cap, kDramLuxury);

    // ── A. DRAM-tight:现实 DRAM 预算受限 ────────────────────
    {
        DramBlockStore store(kBlockSize, kDramTight);
        LruPolicy      policy;
        Cache          cache(store, policy);

        Stats s = run_workload(trace, kBlockSize,
            [&](BlockId id, std::byte* dst) { return cache.get(id, dst); },
            [&](BlockId id, const std::byte* src) { cache.put(id, src); });
        print_row("A DRAM-tight", s);
    }

    // ── B. Tiered:同 DRAM 预算 + SSD 扩容 ───────────────────
    {
        DramBlockStore l1(kBlockSize, kL1Cap);
        LruPolicy      l1_policy;
        SsdBlockStore  l2(kBlockSize, kL2Cap, "/tmp/p4_bench_l2.dat");
        LruPolicy      l2_policy;
        TieredCache    tc(l1, l1_policy, l2, l2_policy);

        Stats s = run_workload(trace, kBlockSize,
            [&](BlockId id, std::byte* dst) { return tc.get(id, dst); },
            [&](BlockId id, const std::byte* src) { tc.put(id, src); });
        print_row("B Tiered", s);
    }

    // ── C. DRAM-luxury:理想 DRAM 预算(不切实际,但作 hit rate 上限参考)
    {
        DramBlockStore store(kBlockSize, kDramLuxury);
        LruPolicy      policy;
        Cache          cache(store, policy);

        Stats s = run_workload(trace, kBlockSize,
            [&](BlockId id, std::byte* dst) { return cache.get(id, dst); },
            [&](BlockId id, const std::byte* src) { cache.put(id, src); });
        print_row("C DRAM-luxury", s);
    }

    return 0;
}
