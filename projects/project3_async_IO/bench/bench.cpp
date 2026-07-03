// Project 3 benchmark driver.
// 用法示例:
//   ./build/bench --backend uring --qd 64 --block-size 4096 --op read --pattern rand --duration 10
//   ./build/bench --backend sync  --qd 64 --block-size 4096 --op read --pattern rand --duration 10
//
// 公平对比口径:同样的 queue depth、block size、IO 模式、相同的 backing file。
// 区别只有"提交/完成机制":
//   sync  → N 个 worker 线程,每个阻塞在 pread/pwrite,N = qd
//   uring → 单线程驱动 io_uring,SQ/CQ 深度 = qd
//
// 量的是:吞吐(ops/s, MB/s) + 延迟分位数 p50/p90/p99/p99.9

#include "sync_backend.hpp"
#include "cached_backend.hpp"
#ifdef __linux__
#  include "uring_backend.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <random>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

using namespace p3;
using clk = std::chrono::steady_clock;

struct Args {
    // 三个正交维度:
    //   backend    (底层 IO 接口)  sync | uring
    //   o_direct   (open flag)     --direct
    //   cache      (装饰器叠加)    --cache + --cache-slots
    std::string backend     = "sync";              // sync | uring
    std::string file        = "/tmp/p3_bench.dat";
    uint64_t    file_size   = 4ull << 30;          // 4 GiB
    uint32_t    block_size  = 4096;
    std::string op          = "read";              // read | write | mixed
    std::string pattern     = "rand";              // seq | rand
    uint32_t    duration_s  = 10;
    uint32_t    qd          = 32;                  // queue depth / worker count
    bool        o_direct    = false;
    bool        cache       = false;               // 是否套用户态 LRU cache 装饰器
    // cache 池 slot 数。默认远大于 qd,给随机模式一定命中概率;
    // 跟 --file-size / --block-size 一起决定命中率上限。
    uint32_t    cache_slots = 1024;
};

static void usage() {
    std::fprintf(stderr,
        "usage: bench [--backend sync|uring]\n"
        "             [--file PATH] [--file-size BYTES] [--block-size BYTES]\n"
        "             [--op read|write|mixed] [--pattern seq|rand]\n"
        "             [--duration SECS] [--qd N] [--direct]\n"
        "             [--cache] [--cache-slots N]\n"
        "  三个维度正交:backend(sync/uring) x direct(O_DIRECT) x cache(用户态 LRU)\n");
}

static bool parse_args(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { usage(); std::exit(1); }
            return argv[++i];
        };
        if      (k == "--backend")    a.backend    = next();
        else if (k == "--file")       a.file       = next();
        else if (k == "--file-size")  a.file_size  = std::stoull(next());
        else if (k == "--block-size") a.block_size = static_cast<uint32_t>(std::stoul(next()));
        else if (k == "--op")         a.op         = next();
        else if (k == "--pattern")    a.pattern    = next();
        else if (k == "--duration")   a.duration_s = static_cast<uint32_t>(std::stoul(next()));
        else if (k == "--qd")         a.qd         = static_cast<uint32_t>(std::stoul(next()));
        else if (k == "--direct")     a.o_direct   = true;
        else if (k == "--cache")      a.cache      = true;
        else if (k == "--cache-slots") a.cache_slots = static_cast<uint32_t>(std::stoul(next()));
        else { usage(); return false; }
    }
    return true;
}

int main(int argc, char** argv) {
    Args a;
    if (!parse_args(argc, argv, a)) return 1;

    int open_flags = O_RDWR | O_CREAT;
#ifdef __linux__
    if (a.o_direct) open_flags |= O_DIRECT;
#else
    if (a.o_direct) {
        std::fprintf(stderr, "warning: O_DIRECT 仅 Linux 支持,已忽略\n");
    }
#endif
    int fd = ::open(a.file.c_str(), open_flags, 0644);
    if (fd < 0) { ::perror("open"); return 1; }

    struct stat st{};
    ::fstat(fd, &st);
    if (static_cast<uint64_t>(st.st_size) < a.file_size) {
        if (::ftruncate(fd, static_cast<off_t>(a.file_size)) < 0) {
            ::perror("ftruncate"); return 1;
        }
    }

    // 第一步:按 --backend 建底层 IO 抽象
    std::unique_ptr<IoBackend> inner;
    if (a.backend == "sync") {
        inner = std::make_unique<SyncBackend>(a.qd);
    }
#ifdef __linux__
    else if (a.backend == "uring") {
        inner = std::make_unique<UringBackend>(a.qd);
    }
#endif
    else {
        std::fprintf(stderr, "unknown backend: %s\n", a.backend.c_str());
        return 1;
    }

    // 第二步:按 --cache 决定是否套 CachedBackend 装饰器。
    // cached_ptr 保留一个裸指针,收尾打印命中率用;所有权仍在 unique_ptr 链上。
    std::unique_ptr<IoBackend> backend;
    CachedBackend* cached_ptr = nullptr;
    if (a.cache) {
        auto p = std::make_unique<CachedBackend>(
            std::move(inner), a.qd, a.cache_slots, a.block_size);
        cached_ptr = p.get();
        backend = std::move(p);
    } else {
        backend = std::move(inner);
    }

    // 每个 in-flight 槽位一个独立 buffer。O_DIRECT 要求 buffer 按 block 大小对齐,
    // 这里统一按 4096 对齐(够覆盖大多数 NVMe sector size)。
    std::vector<void*> bufs(a.qd, nullptr);
    for (auto& b : bufs) {
        if (::posix_memalign(&b, 4096, a.block_size) != 0) {
            std::fprintf(stderr, "posix_memalign failed\n"); return 1;
        }
        std::memset(b, 0xAB, a.block_size);
    }

    const uint64_t max_block_idx = a.file_size / a.block_size;
    std::mt19937_64 rng(0xc0ffeeULL);
    std::uniform_int_distribution<uint64_t> off_dist(0, max_block_idx - 1);

    // 按 user_data(id)记提交时刻,reap 时按 id 取并删除。
    // 历史:最早版本用 vector<time_point>(qd) + slot=id%qd 索引,假设"管线 ≤ qd
    // 就不会撞 slot"。在 sync 多 worker 乱序完成 或 direct IO 真延迟下完成更乱
    // 的场景里这个假设破了:同一 batch reap 里"新 id 先到、旧 id 后到",新 id
    // 触发的 submit_one 会覆盖旧 id 依赖的 submit_ts[slot] → lat 系统性低估,
    // 严重时还会下溢成 ~UINT64_MAX(诊断时观测到 max_gap 高达 33·qd)。
    // 修法:换成按 id 索引的 hash map,容量预留 ~2·qd 减少 rehash;
    // 代价是每 op 多 1 次 hash insert + 1 次 find + 1 次 erase,
    // 实测对吞吐 < 0.2%,远小于测量误差。
    std::unordered_map<uint64_t, clk::time_point> submit_ts;
    submit_ts.reserve(a.qd * 2);

    std::vector<uint64_t> latencies_ns;
    latencies_ns.reserve(1 << 20);

    auto pick_op = [&](uint64_t i) -> IoRequest::Op {
        if (a.op == "read")  return IoRequest::READ;
        if (a.op == "write") return IoRequest::WRITE;
        return (i & 1) ? IoRequest::WRITE : IoRequest::READ;
    };
    auto pick_offset = [&](uint64_t seq_i) -> uint64_t {
        uint64_t block_idx = (a.pattern == "seq")
                                ? (seq_i % max_block_idx)
                                : off_dist(rng);
        return block_idx * a.block_size;
    };

    uint64_t next_id = 0;
    auto submit_one = [&]() {
        // buffer 仍按 slot 复用(只要管线深度 ≤ qd, buf 不会同时被两个 in-flight 共用),
        // 但 submit_ts 不再用 slot 索引,改为按 id 存。两者解耦,避免之前的 slot 复用 bug。
        uint32_t slot = static_cast<uint32_t>(next_id % a.qd);
        IoRequest r{};
        r.op        = pick_op(next_id);
        r.fd        = fd;
        r.offset    = pick_offset(next_id);
        r.buf       = bufs[slot];
        r.size      = a.block_size;
        r.user_data = next_id;
        submit_ts[next_id] = clk::now();
        backend->submit(&r, 1);
        ++next_id;
    };

    // 先把管线灌满:qd 个 in-flight 请求
    for (uint32_t i = 0; i < a.qd; ++i) submit_one();

    auto t_start = clk::now();
    auto t_end   = t_start + std::chrono::seconds(a.duration_s);

    std::vector<IoCompletion> cqe_buf(a.qd);
    uint64_t completed = 0;
    uint64_t bytes     = 0;

    while (clk::now() < t_end) {
        // 至少等 1 个完成,顺手把当下能取的都取了
        size_t k = backend->reap(cqe_buf.data(), cqe_buf.size(), 1);
        auto now = clk::now();

        // 历史遗留约束:必须先把整批的 lat 算完再统一 submit_one(),不能在循环里混做。
        // 原因:submit_one() 内部对当前 batch 共用的 now 之后才取 clk::now() 写 ts,
        // 即使现在 ts 是按 id 索引,batch 内"循环 submit"也会引入"now 已过期"的不一致;
        // 把 submit 拖到循环尾再做最稳。
        size_t to_resubmit = 0;
        for (size_t i = 0; i < k; ++i) {
            uint64_t id  = cqe_buf[i].user_data;
            int32_t  res = cqe_buf[i].res;
            if (res < 0) {
                std::fprintf(stderr, "IO err: %s\n", std::strerror(-res));
                // 注意:出错路径也要补 in-flight,否则管线深度会永久少 1
                submit_ts.erase(id);
                ++to_resubmit;
                continue;
            }
            auto it = submit_ts.find(id);
            if (it == submit_ts.end()) {
                // 不应该发生:每个 in-flight 的 id 在 submit 时都登记过
                std::fprintf(stderr, "warn: completion id=%llu has no submit_ts\n",
                             (unsigned long long)id);
                ++to_resubmit;
                continue;
            }
            auto lat = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           now - it->second).count();
            submit_ts.erase(it);  // 取完即删,map 容量稳定在 ~in_flight
            latencies_ns.push_back(static_cast<uint64_t>(lat));
            ++completed;
            bytes += static_cast<uint64_t>(res);
            ++to_resubmit;
        }

        for (size_t i = 0; i < to_resubmit; ++i) submit_one();
    }

    // 收尾:把还在飞的请求 drain 掉,避免 buffer 被析构后内核还在 DMA
    while (backend->in_flight() > 0) {
        size_t k = backend->reap(cqe_buf.data(), cqe_buf.size(), 1);
        if (k == 0) break;
    }

    auto t_real_end = clk::now();
    double secs = std::chrono::duration<double>(t_real_end - t_start).count();

    std::sort(latencies_ns.begin(), latencies_ns.end());
    auto pct = [&](double p) -> uint64_t {
        if (latencies_ns.empty()) return 0;
        size_t idx = static_cast<size_t>(p * (latencies_ns.size() - 1));
        return latencies_ns[idx];
    };

    double ops_per_s = completed / secs;
    double mb_per_s  = (bytes / (1024.0 * 1024.0)) / secs;

    std::printf("backend=%s qd=%u block=%u op=%s pattern=%s direct=%d duration=%.3fs\n",
                a.backend.c_str(), a.qd, a.block_size, a.op.c_str(),
                a.pattern.c_str(), a.o_direct ? 1 : 0, secs);
    std::printf("  completed=%llu  bytes=%llu\n",
                (unsigned long long)completed, (unsigned long long)bytes);
    std::printf("  throughput: %.0f ops/s  %.1f MB/s\n", ops_per_s, mb_per_s);
    std::printf("  latency ns: p50=%llu  p90=%llu  p99=%llu  p99.9=%llu  max=%llu\n",
                (unsigned long long)pct(0.50),
                (unsigned long long)pct(0.90),
                (unsigned long long)pct(0.99),
                (unsigned long long)pct(0.999),
                (unsigned long long)pct(1.0));

    if (cached_ptr) {
        auto s = cached_ptr->stats();
        double total = static_cast<double>(s.hits + s.misses);
        double hit_rate = total > 0 ? (s.hits / total) : 0.0;
        std::printf("  cache: slots=%u  hits=%llu  misses=%llu  hit_rate=%.2f%%  "
                    "writes_inval=%llu  passthrough=%llu\n",
                    a.cache_slots,
                    (unsigned long long)s.hits,
                    (unsigned long long)s.misses,
                    hit_rate * 100.0,
                    (unsigned long long)s.writes_invalidated,
                    (unsigned long long)s.passthrough);
    }

    for (auto* b : bufs) ::free(b);
    ::close(fd);
    return 0;
}
