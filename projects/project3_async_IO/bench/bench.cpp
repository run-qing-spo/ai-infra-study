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
#include <vector>

using namespace p3;
using clk = std::chrono::steady_clock;

struct Args {
    std::string backend     = "sync";              // sync | uring
    std::string file        = "/tmp/p3_bench.dat";
    uint64_t    file_size   = 4ull << 30;          // 4 GiB
    uint32_t    block_size  = 4096;
    std::string op          = "read";              // read | write | mixed
    std::string pattern     = "rand";              // seq | rand
    uint32_t    duration_s  = 10;
    uint32_t    qd          = 32;                  // queue depth / worker count
    bool        o_direct    = false;
};

static void usage() {
    std::fprintf(stderr,
        "usage: bench [--backend sync|uring] [--file PATH] [--file-size BYTES]\n"
        "             [--block-size BYTES] [--op read|write|mixed]\n"
        "             [--pattern seq|rand] [--duration SECS] [--qd N] [--direct]\n");
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

    std::unique_ptr<IoBackend> backend;
    if (a.backend == "sync") {
        backend = std::make_unique<SyncBackend>(a.qd);
    }
#ifdef __linux__
    else if (a.backend == "uring") {
        backend = std::make_unique<UringBackend>(a.qd);
    }
#endif
    else {
        std::fprintf(stderr, "unknown backend: %s\n", a.backend.c_str());
        return 1;
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

    // 每个 slot 记一个提交时刻,完成时算延迟。user_data 编码成 next_id,
    // slot = next_id % qd → 槽位永远对得上(只要管线深度严格 <= qd 就不冲突)
    std::vector<clk::time_point> submit_ts(a.qd);

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
        uint32_t slot = static_cast<uint32_t>(next_id % a.qd);
        IoRequest r{};
        r.op        = pick_op(next_id);
        r.fd        = fd;
        r.offset    = pick_offset(next_id);
        r.buf       = bufs[slot];
        r.size      = a.block_size;
        r.user_data = next_id;
        submit_ts[slot] = clk::now();
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

        // 关键:先把整批的 lat 算完再统一 submit_one(),不能在循环里混着做。
        // 原因:submit_one() 内部会写 submit_ts[next_id % qd] = clk::now(),这个
        // 时间戳比当前 batch 共用的 now 更晚。如果 sync 后端乱序完成,同一批 reap
        // 里既有"较新 id"又有"较旧 id",前者触发的 submit_one 会覆盖后者依赖的
        // submit_ts[slot],于是 lat = now - submit_ts[slot] 变成负的几十 ns,
        // 强转 uint64_t 下溢成 ~UINT64_MAX 进 max 统计。
        size_t to_resubmit = 0;
        for (size_t i = 0; i < k; ++i) {
            uint64_t id  = cqe_buf[i].user_data;
            int32_t  res = cqe_buf[i].res;
            if (res < 0) {
                std::fprintf(stderr, "IO err: %s\n", std::strerror(-res));
                // 注意:出错路径也要补 in-flight,否则管线深度会永久少 1
                ++to_resubmit;
                continue;
            }
            uint32_t slot = static_cast<uint32_t>(id % a.qd);
            auto lat = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           now - submit_ts[slot]).count();
            latencies_ns.push_back(static_cast<uint64_t>(lat));
            ++completed;
            bytes += static_cast<uint64_t>(res);
            ++to_resubmit;
        }

        // 整批 lat 都算完了,submit_ts 可以安全被覆盖
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

    for (auto* b : bufs) ::free(b);
    ::close(fd);
    return 0;
}
