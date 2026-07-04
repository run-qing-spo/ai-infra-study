// P4 concurrent bench:量化"锁 vs 无锁"的开销,以及 shared_mutex 在多线程下
// 到底能不能兑现"读并行"的承诺。
//
// 三组要一起看,才能把故事讲全:
//   1. Cache (无锁, 单线程):
//      —— 是"如果锁的开销为零"的理论上限。放在最上头当参照系。
//      —— 直接在多线程下跑它是 data race + UB(index_ 会被并发改),
//         所以我们只在 T=1 下测它,拿数字对齐,不做 MT 版本。
//   2. LockedCache (shared_mutex, 单线程):
//      —— 只有一个 reader,shared_lock 拿了立刻放,和 Cache 相比的差值就是
//         "空锁 fast-path 开销"(atomic RMW + 内存屏障)。
//      —— 如果这一步就比 Cache 慢很多,说明 shared_mutex 本身重,后面 T>1
//         就得靠更高的并行度才能盖住。
//   3. LockedCache (shared_mutex, T=2/4/8):
//      —— 才是 shared_mutex 真正的用武之地:多个 reader 走 shared_lock,
//         理想里吞吐随 T 线性上升。put 走 unique_lock 会阻塞所有人,
//         miss rate 高时上限被写路径拉低。
//
// 关键预期:
//   - LockedCache 的 hit rate 会略低于 Cache —— 因为 shared_lock 版本刻意
//     不调 on_access,LRU 热度不更新,退化成 FIFO 味道。
//     这个"命中率低一点"就是"要用 shared_lock 就得放弃 LRU 精确性"的代价,
//     不是 bug。
//   - 多线程下扩展性不是线性:
//     · 命中率高 → 大部分是 shared_lock read → 接近线性;
//     · 命中率低 → 频繁 put 卡 unique_lock → 早早撞墙。
//     所以我们特意选一个 cap 让 hit rate 落在中段,让 put 和 get 都有戏份。

#include "cache.hpp"
#include "dram_block_store.hpp"
#include "kv_trace_gen.hpp"
#include "locked_cache.hpp"
#include "lru_policy.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

using namespace p4;
using clk = std::chrono::steady_clock;

namespace {

// —— Zipf 生成器,和 bench.cpp 一份,便于对齐 trace ——
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

void fill_block(std::vector<std::byte>& buf, BlockId id) {
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<std::byte>((id * 31 + i) & 0xff);
    }
}

// 每个线程一份,末尾再 merge。分开避免所有 hit / miss 计数走 atomic。
struct ThreadStats {
    size_t                hits = 0;
    size_t                misses = 0;
    std::vector<uint64_t> lat_ns;   // per-op 延迟

    void reserve(size_t n) { lat_ns.reserve(n); }
};

struct AggStats {
    size_t   hits = 0;
    size_t   misses = 0;
    double   wall_ms = 0.0;
    std::vector<uint64_t> lat_ns;   // 汇总,末尾统一算分位

    double hit_rate() const {
        return double(hits) / double(hits + misses);
    }
    double throughput_mops() const {
        return double(hits + misses) / (wall_ms * 1000.0);   // Mops/s
    }
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

// GetFn / PutFn 由外层决定 —— Cache 就传 cache.get / cache.put,LockedCache 同。
// 内部纯串行:一个 slice 内的每次 miss 都要落地 put 再继续,和上层业务
// look-aside 模式一致。
template<typename GetFn, typename PutFn>
void run_worker_slice(const BlockId* trace, size_t n_ops, size_t block_size,
                      GetFn get_fn, PutFn put_fn, ThreadStats* out) {
    out->reserve(n_ops);
    std::vector<std::byte> buf(block_size);
    std::vector<std::byte> dst(block_size);
    for (size_t i = 0; i < n_ops; ++i) {
        BlockId id = trace[i];
        auto t0 = clk::now();
        if (get_fn(id, dst.data())) {
            ++out->hits;
        } else {
            ++out->misses;
            fill_block(buf, id);
            put_fn(id, buf.data());
        }
        auto t1 = clk::now();
        out->lat_ns.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
}

// 单线程入口(Cache / LockedCache 都能走)。整段 trace 一把梭。
template<typename GetFn, typename PutFn>
AggStats run_single(const std::vector<BlockId>& trace, size_t block_size,
                    GetFn get_fn, PutFn put_fn) {
    ThreadStats ts;
    auto t0 = clk::now();
    run_worker_slice(trace.data(), trace.size(), block_size, get_fn, put_fn, &ts);
    auto t1 = clk::now();
    AggStats a;
    a.hits = ts.hits;
    a.misses = ts.misses;
    a.wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    a.lat_ns = std::move(ts.lat_ns);
    return a;
}

// 多线程入口(只喂 LockedCache;Cache 无锁跑多线程会 race)。
// 切成 num_threads 份连续 slice,起 num_threads 个 worker,barrier 起跑。
template<typename GetFn, typename PutFn>
AggStats run_multi(const std::vector<BlockId>& trace, size_t block_size,
                   size_t num_threads, GetFn get_fn, PutFn put_fn) {
    std::vector<ThreadStats> per_thread(num_threads);
    std::vector<std::thread> workers;
    workers.reserve(num_threads);

    // 简单 barrier:所有线程构造完再一起启动,减少 warmup skew。
    // 用 atomic<bool> spin,不是最严谨的 barrier,但 num_threads 小时够用。
    std::atomic<bool> go{false};

    const size_t slice = trace.size() / num_threads;
    const BlockId* base = trace.data();

    auto t_start = clk::now();
    for (size_t t = 0; t < num_threads; ++t) {
        const BlockId* p = base + t * slice;
        // 最后一段吃掉整除余数,保证 sum 精确等于 trace.size()。
        size_t n = (t + 1 == num_threads) ? (trace.size() - t * slice) : slice;
        ThreadStats* out = &per_thread[t];
        workers.emplace_back([&, p, n, out]() {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            run_worker_slice(p, n, block_size, get_fn, put_fn, out);
        });
    }
    // 全部线程 spawn 完,再放行 —— 避免"第一批开跑时后一批还没起来"。
    go.store(true, std::memory_order_release);

    for (auto& w : workers) w.join();
    auto t_end = clk::now();

    AggStats a;
    a.wall_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    // 汇总每线程 stats
    size_t total_lat = 0;
    for (auto& ts : per_thread) total_lat += ts.lat_ns.size();
    a.lat_ns.reserve(total_lat);
    for (auto& ts : per_thread) {
        a.hits   += ts.hits;
        a.misses += ts.misses;
        a.lat_ns.insert(a.lat_ns.end(), ts.lat_ns.begin(), ts.lat_ns.end());
    }
    return a;
}

void print_header() {
    std::printf("%-28s  %-8s  hit%%    throughput      wall_ms   mean_us   p50_us   p95_us   p99_us\n",
                "config", "threads");
    std::printf("%s\n", std::string(120, '-').c_str());
}

void print_row(const char* name, size_t threads, AggStats& a) {
    std::printf("%-28s  T=%-6zu  %5.2f%%  %6.2f Mops/s  %8.2f  %8.3f  %7.3f  %7.3f  %7.3f\n",
                name, threads,
                a.hit_rate() * 100,
                a.throughput_mops(),
                a.wall_ms,
                a.mean_us(),
                a.percentile(0.50) / 1000.0,
                a.percentile(0.95) / 1000.0,
                a.percentile(0.99) / 1000.0);
}

// ————————————————— KV workload 版本的 worker / runner —————————————————
//
// 跟通用版并列,区别只有输入类型:通用版是 BlockId 序列(每 op 语义相同,都是
// look-aside get),KV 版是 KVOp 序列(区分 kGet look-aside 和 kPut 无条件写)。
// 之所以要分开写,不复用通用版:如果把 KVOp 塞到 BlockId 序列里,就丢掉了
// "这一步是 prefill 段的 look-aside get / decode 段的 append put" 的区别,
// 后面想加 "prefill vs decode 分段 hit 统计" 就没抓手了。

template<typename GetFn, typename PutFn>
void run_worker_kv_slice(const KVOp* ops, size_t n_ops, size_t block_size,
                         GetFn get_fn, PutFn put_fn, ThreadStats* out) {
    out->reserve(n_ops);
    std::vector<std::byte> buf(block_size);
    std::vector<std::byte> dst(block_size);
    for (size_t i = 0; i < n_ops; ++i) {
        const KVOp& op = ops[i];
        auto t0 = clk::now();
        if (op.kind == OpKind::kGet) {
            // look-aside:miss 时 fill+put,让后续同 prefix 的 request 能命中
            if (get_fn(op.id, dst.data())) {
                ++out->hits;
            } else {
                ++out->misses;
                fill_block(buf, op.id);
                put_fn(op.id, buf.data());
            }
        } else {
            // kPut(decode append):无条件写,没有 hit/miss 语义。
            // 为了不让 hit_rate 统计跑偏,归到 misses(写路径成本跟 miss-then-put 一致)。
            // 后续如果需要更细的分段统计,把 hits/misses 拆成 prefill/decode 四栏。
            ++out->misses;
            fill_block(buf, op.id);
            put_fn(op.id, buf.data());
        }
        auto t1 = clk::now();
        out->lat_ns.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
}

template<typename GetFn, typename PutFn>
AggStats run_single_kv(const std::vector<KVOp>& ops, size_t block_size,
                       GetFn get_fn, PutFn put_fn) {
    ThreadStats ts;
    auto t0 = clk::now();
    run_worker_kv_slice(ops.data(), ops.size(), block_size, get_fn, put_fn, &ts);
    auto t1 = clk::now();
    AggStats a;
    a.hits = ts.hits;
    a.misses = ts.misses;
    a.wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    a.lat_ns = std::move(ts.lat_ns);
    return a;
}

// 多线程 KV:切片必须按 request 边界。把一整个 request 的 prefill+decode 交给
// 同一线程,不然:
//   - decode 段要读该 request 的历史 block,如果历史在别的线程上没落地,
//     就变成假 miss,hit rate 数字失真。
//   - prefill 的 look-aside put 和 decode 的 append 都在同线程,写路径的
//     锁争抢才是真实的 —— 拆开等于人为放大 reader 比例。
// 分片方式:按 request 连续段切(不 round-robin),同一批 request 里前缀重合
// 的概率更高,保留 workload 本来的时间局部性。
template<typename GetFn, typename PutFn>
AggStats run_multi_kv(const std::vector<KVOp>& ops,
                      const std::vector<size_t>& req_starts,
                      size_t block_size, size_t num_threads,
                      GetFn get_fn, PutFn put_fn) {
    const size_t num_reqs = req_starts.size() - 1;   // 末尾是哨兵

    std::vector<ThreadStats> per_thread(num_threads);
    std::vector<std::thread> workers;
    workers.reserve(num_threads);

    std::atomic<bool> go{false};

    auto t_start = clk::now();
    for (size_t t = 0; t < num_threads; ++t) {
        const size_t r_lo = t       * num_reqs / num_threads;
        const size_t r_hi = (t + 1) * num_reqs / num_threads;
        const KVOp* p = ops.data() + req_starts[r_lo];
        const size_t n = req_starts[r_hi] - req_starts[r_lo];
        ThreadStats* out = &per_thread[t];
        workers.emplace_back([&, p, n, out]() {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            run_worker_kv_slice(p, n, block_size, get_fn, put_fn, out);
        });
    }
    go.store(true, std::memory_order_release);

    for (auto& w : workers) w.join();
    auto t_end = clk::now();

    AggStats a;
    a.wall_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    size_t total_lat = 0;
    for (auto& ts : per_thread) total_lat += ts.lat_ns.size();
    a.lat_ns.reserve(total_lat);
    for (auto& ts : per_thread) {
        a.hits   += ts.hits;
        a.misses += ts.misses;
        a.lat_ns.insert(a.lat_ns.end(), ts.lat_ns.begin(), ts.lat_ns.end());
    }
    return a;
}

} // namespace

// 跑一整套场景,共用 print_header 出的表格。
// warmup:先串行放一遍热 id,让每个场景开跑时缓存不是全空 —— 否则第一段路径
// 全是 miss,拉 unique_lock 的份额会被人为放大,冲淡 read-heavy 的效果。
template<typename CacheT>
void warmup(CacheT& cache, const std::vector<BlockId>& trace, size_t block_size,
            size_t warmup_ops) {
    std::vector<std::byte> buf(block_size);
    std::vector<std::byte> dst(block_size);
    for (size_t i = 0; i < warmup_ops && i < trace.size(); ++i) {
        BlockId id = trace[i];
        if (!cache.get(id, dst.data())) {
            fill_block(buf, id);
            cache.put(id, buf.data());
        }
    }
}

// 跑一个配置(kCap 决定 hit rate 区间)—— 三种 case + 多线程扩展。
void run_scenario(const char* label, size_t block_size, size_t cap,
                  const std::vector<BlockId>& trace) {
    std::printf("\n=== %s (cap=%zu) ===\n", label, cap);
    print_header();

    // 1. 无锁 Cache 单线程:锁开销为零的参照。
    {
        DramBlockStore store(block_size, cap);
        LruPolicy      policy;
        Cache          cache(store, policy);
        warmup(cache, trace, block_size, cap * 2);
        AggStats a = run_single(trace, block_size,
            [&](BlockId id, std::byte* d) { return cache.get(id, d); },
            [&](BlockId id, const std::byte* s) { cache.put(id, s); });
        print_row("Cache (no lock)", 1, a);
    }

    // 2. LockedCache 单线程:纯锁开销(空锁 fast-path)。
    {
        DramBlockStore store(block_size, cap);
        LruPolicy      policy;
        LockedCache    cache(store, policy);
        warmup(cache, trace, block_size, cap * 2);
        AggStats a = run_single(trace, block_size,
            [&](BlockId id, std::byte* d) { return cache.get(id, d); },
            [&](BlockId id, const std::byte* s) { cache.put(id, s); });
        print_row("LockedCache (shared_mutex)", 1, a);
    }

    // 3. LockedCache 多线程:看 shared_lock 兑现读并行到什么程度。
    for (size_t T : {2, 4, 8}) {
        DramBlockStore store(block_size, cap);
        LruPolicy      policy;
        LockedCache    cache(store, policy);
        warmup(cache, trace, block_size, cap * 2);
        AggStats a = run_multi(trace, block_size, T,
            [&](BlockId id, std::byte* d) { return cache.get(id, d); },
            [&](BlockId id, const std::byte* s) { cache.put(id, s); });
        print_row("LockedCache (shared_mutex)", T, a);
    }
}

// KV workload 版场景。跟 run_scenario 平行,不 warmup —— prefix cache 的
// warmup 语义应该是 "先跑一遍 prefix pool 的 prefill 让 prefix 落地",
// 直接沿用通用版的 "拿 trace 前段跑一遍" 会污染 hit 统计(拿了一批真实
// request 去当 warmup,那这些请求的 prefix hit 就被算进 warmup 阶段了,
// 主 bench 阶段的数字反而偏低)。这里的取舍:不 warmup,把第一批 request
// 的 tail miss 也算进最终统计 —— 这跟真实冷启动服务的 hit rate 曲线一致,
// 更诚实。
void run_kv_scenario(const char* label, size_t block_size, size_t cap,
                     const std::vector<KVOp>& ops,
                     const std::vector<size_t>& req_starts) {
    std::printf("\n=== %s (cap=%zu blocks) ===\n", label, cap);
    print_header();

    {
        DramBlockStore store(block_size, cap);
        LruPolicy      policy;
        Cache          cache(store, policy);
        AggStats a = run_single_kv(ops, block_size,
            [&](BlockId id, std::byte* d) { return cache.get(id, d); },
            [&](BlockId id, const std::byte* s) { cache.put(id, s); });
        print_row("Cache (no lock)", 1, a);
    }
    {
        DramBlockStore store(block_size, cap);
        LruPolicy      policy;
        LockedCache    cache(store, policy);
        AggStats a = run_single_kv(ops, block_size,
            [&](BlockId id, std::byte* d) { return cache.get(id, d); },
            [&](BlockId id, const std::byte* s) { cache.put(id, s); });
        print_row("LockedCache (shared_mutex)", 1, a);
    }
    for (size_t T : {2, 4, 8}) {
        DramBlockStore store(block_size, cap);
        LruPolicy      policy;
        LockedCache    cache(store, policy);
        AggStats a = run_multi_kv(ops, req_starts, block_size, T,
            [&](BlockId id, std::byte* d) { return cache.get(id, d); },
            [&](BlockId id, const std::byte* s) { cache.put(id, s); });
        print_row("LockedCache (shared_mutex)", T, a);
    }
}

int main() {
    // —— 通用配置 ————————————————————————————————
    constexpr size_t kBlockSize  = 4096;
    constexpr size_t kUniqueIds  = 1000;
    constexpr size_t kNumOps     = 200'000;
    constexpr double kTheta      = 0.99;
    constexpr uint64_t kSeed     = 42;

    // 同一份 trace 复用,场景之间可比。
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

    // 场景 A:cap=100(≈ 10% 工作集)。miss rate 高,put 频繁 —— 是 shared_mutex
    //          的"逆风局":unique_lock 频繁触发,shared_lock 之间夹着排他锁,
    //          reader 拉不上并行,反而每次 shared_lock 都要争同一根 refcount cache line。
    //          预期看到"多线程比单线程还慢"的负扩展。
    run_scenario("A. Write-heavy (小 cache, miss 多, put 抢 unique_lock)",
                 kBlockSize, 100, trace);

    // 场景 B:cap=500(≈ 50% 工作集)。热 id 全塞得下,尾巴少数 miss ——
    //          shared_lock 路径占主导。这里 shared_mutex 真正的用武之地。
    //          预期看到 T 上去,吞吐上去(不会满线性,shared_lock refcount 有
    //          cache-line 抢锁,再加 4KB memcpy 会吃内存带宽)。
    run_scenario("B. Read-heavy (大 cache, hit 多, shared_lock 主导)",
                 kBlockSize, 500, trace);

    // —————— KV cache workload 场景 ——————
    // 不是通用 block cache 的独立 Zipf,是模拟 LLM inference 的 request 流:
    //   - prefix pool 里选一个共享前缀(system prompt / few-shot 变种)
    //   - prompt / output 长度按 log-normal 长尾抽
    //   - prefill 段 look-aside get(前段命中 prefix、后段 tail miss)
    //   - decode 段 append 新 block + sample 历史 block
    // 参数选取:num_prefixes=16、prefix_theta=1.5(system prompt 强集中)、
    //          prefix_blocks=16、num_requests=800、prompt~24 block(≈100KB)、
    //          output~12 block。整个 prefix 池 = 16*16 = 256 block。
    KVTraceConfig kv_cfg;
    kv_cfg.num_prefixes         = 16;
    kv_cfg.prefix_theta         = 1.5;
    kv_cfg.prefix_blocks        = 16;
    kv_cfg.num_requests         = 800;
    kv_cfg.prompt_mean_blocks   = 24.0;
    kv_cfg.prompt_sigma         = 0.6;
    kv_cfg.output_mean_blocks   = 12.0;
    kv_cfg.output_sigma         = 0.5;
    kv_cfg.seed                 = kSeed;

    KVTraceGen kv_gen(kv_cfg);
    std::vector<KVOp> kv_ops = kv_gen.generate();
    const auto& kv_starts = kv_gen.request_starts();

    std::printf("\nKV workload: prefixes=%zu(theta=%.2f, %zu blk each), "
                "requests=%zu, prompt~%.0f blk, output~%.0f blk, total_ops=%zu\n",
                kv_cfg.num_prefixes, kv_cfg.prefix_theta, kv_cfg.prefix_blocks,
                kv_cfg.num_requests, kv_cfg.prompt_mean_blocks,
                kv_cfg.output_mean_blocks, kv_ops.size());

    // 场景 C:cap 刚好装下整个 prefix 池 + 一点空间给 tail —— 检验 "hot prefix
    //          全命中、cold tail 全 miss" 的分层是否兑现。
    run_kv_scenario("C. KV workload (cap ≈ prefix pool + slack)",
                    kBlockSize, kv_gen.prefix_pool_block_count() + 64, kv_ops, kv_starts);

    // 场景 D:cap 小到装不下整个 prefix 池 —— prefix 之间自己开始互相踢,
    //          即使热前缀理论上该命中,也会被 tail put 挤掉。观察 hit rate
    //          崩塌的形状,对应真实 KV serving 里 "prefix cache 容量不够"
    //          的病态区。
    run_kv_scenario("D. KV workload (cap < prefix pool, 前缀互踢)",
                    kBlockSize, kv_gen.prefix_pool_block_count() / 2, kv_ops, kv_starts);

    std::printf("\n观察要点:\n");
    std::printf("  · LockedCache hit%% 略低于 Cache 是预期:shared_lock 版 get 不调 on_access,\n");
    std::printf("    LRU 退化 → 淘汰序不再严格按热度。cap 越大差距越小(尾部误判无所谓)。\n");
    std::printf("  · 场景 A:多线程反而变慢 —— shared_mutex 不是免费午餐。shared_lock 每次\n");
    std::printf("    获取仍是一次 atomic RMW(refcount 增减),在多核间 ping-pong 一根 cache line;\n");
    std::printf("    加上 unique_lock 卡住所有 reader,写多时收益全被吃掉。\n");
    std::printf("  · 场景 B:才是 shared_mutex 的正确使用姿势 —— 读远多于写,shared_lock 走\n");
    std::printf("    fast path,writer 少不会拖后腿,吞吐能真涨。\n");
    std::printf("  · 面试点:'加个 shared_mutex 就能并发'是幻觉。什么时候真赚:\n");
    std::printf("      读写比 > 10:1、临界区里做的活比 shared_lock 本身开销大得多。\n");
    std::printf("      不满足就换 sharded lock(每片 mutex)或 lock-free 结构。\n");
    std::printf("  · KV 场景 C vs D:cap 跨过 prefix 池大小是一道悬崖 —— C 段热\n");
    std::printf("    前缀锁在 cache 里 hit rate 稳,D 段 cap 不够时前缀之间开始互踢,\n");
    std::printf("    hit rate 直接崩。真实 vLLM/SGLang 的 preemption 策略就是在这条\n");
    std::printf("    悬崖上做取舍(踢谁的 sequence、什么时候 swap 到 SSD)。\n");
    std::printf("  · KV 场景多线程:比通用场景更容易看到 shared_lock 兑现读并行,\n");
    std::printf("    因为 prefix hit 段是纯 shared_lock read;但 prefill/decode 的\n");
    std::printf("    put burst 又会周期性地夹 unique_lock,吞吐曲线不会线性 —— 这是\n");
    std::printf("    KV serving 里 continuous batching 存在的动机之一。\n");
    return 0;
}
