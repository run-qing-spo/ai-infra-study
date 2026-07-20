// 元数据面微基准:验证 E1 冷组里 fs tier 系统性劣化的"元数据"归因假说。
//
// 假说(COMPARE_PLAN E1 节遗留):fs backend 每 block 一个文件, 一万五千多条
// dentry/inode 缓存在 slab 里; ballast 制造的内存压力触发 shrinker 把它们回收,
// 之后每次 load 的 open() 就要重新做路径解析、甚至去盘上读目录块和 inode ——
// 从几微秒的 CPU 操作退化成一次同步设备往返。uring 只有一个 slab 文件、一条
// dentry, 无此作用面。
//
// e2e 里证不了这个假说, 因为 ballast 同时压 page cache 和 slab, 是混杂变量;
// 而且 TTFT 把 open 的成本埋在 prefill 里看不见。这里把它剥成纯 IO 微基准:
// 不需要 GPU、不需要 vLLM, 一台 Linux + 一块盘即可。
//
// 三个布局(--layout):
//   files        N 个小文件, hash 三层子目录, 每次读 open 一个不同的文件   [仿 fs tier]
//   slab         单个大文件, fd 复用, pread 到 offset                       [仿 uring tier]
//   slab-reopen  单个大文件, 但每次读都重开一遍                             [隔离变量用]
//
// slab-reopen 是判据的关键:它和 files 的 open 次数一模一样, 唯一差别是 dentry
// 有 N 条还是只有 1 条。冷条件下若 files 变慢而 slab-reopen 不变, 贵的就不是
// open() 这个 syscall 本身, 而是"N 条 dentry 装不下/被回收"—— 假说坐实。
//
// 三档缓存条件由外面的 run_meta_probe.sh 用 drop_caches 构造:
//   hot     什么都不丢          dcache 全命中, open 是纯 CPU 的快路径
//   drop 2  只丢 dentry/inode   要重建, 但目录块/inode 块可能还在 page cache → 不读盘
//   drop 3  slab + page cache   重建时必须真读盘 → 元数据 IO 显形
// drop2 与 drop3 的差值 = 纯 CPU 重建成本; drop3 与 drop2 的差值 = 盘上元数据 IO。
// ballast 的内存压力两个池子都打, 行为上对应 drop 3 那一档。
//
// 读路径两个布局完全对齐:都是 O_DIRECT、同样的块大小、同样的随机块序列,
// 唯一的自变量是文件布局。分段计时 open / pread / close, 各出 mean/p50/p99/max。
//
// 编译: make meta_probe    (只在 Linux 上编, O_DIRECT)

#ifndef __linux__
#  error "meta_probe requires Linux (O_DIRECT + drop_caches)"
#endif

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// vLLM 默认 block_size=16 token, Qwen2.5-7B 每 token KV 56KB → 0.875MiB/block。
// 是 4096 的 224 倍, 满足 O_DIRECT 对齐。(数据来源: COMPARE_PLAN 第 0 步答案)
constexpr size_t kDefaultBlockSize = 917504;
constexpr size_t kAlign            = 4096;

struct Config {
    std::string mode;                       // build | probe
    std::string layout = "files";           // files | slab | slab-reopen
    std::string dir;                        // 数据根目录
    size_t blocks     = 15000;              // 文件数(files) / slab 内的块数
    size_t block_size = kDefaultBlockSize;
    size_t reads      = 1000;               // probe 阶段随机读多少次
    size_t redrop       = 0;                // 每读多少次重新 drop_caches(0 = 不重 drop)
    int    redrop_level = 3;                // 2 = 只丢 dentry/inode; 3 = 连 page cache
    uint64_t seed     = 42;
};

[[noreturn]] void die(const std::string& what) {
    std::fprintf(stderr, "meta_probe: %s: %s\n", what.c_str(), std::strerror(errno));
    std::exit(1);
}

double now_us() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

// ── 布局 ────────────────────────────────────────────────────────────────
// 仿 vLLM fs backend 的 hash 三层子目录:把 block 序号哈希掉再切三段,
// 目的是让 dentry 分散在多个目录里, 而不是一个巨大的平坦目录 —— 后者的
// 查找特性(尤其 xfs 的大目录 btree)和真实 fs tier 不是一回事。
std::string block_path(const std::string& dir, size_t idx) {
    uint64_t h = std::hash<uint64_t>{}(idx);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02x/%02x/%016llx",
                  static_cast<unsigned>((h >> 56) & 0xff),
                  static_cast<unsigned>((h >> 48) & 0xff),
                  static_cast<unsigned long long>(h));
    return dir + "/" + buf;
}

std::string slab_path(const std::string& dir) { return dir + "/slab.bin"; }

void mkdirs(const std::string& path) {
    // 逐级建目录, 已存在不算错
    for (size_t i = 1; i < path.size(); ++i) {
        if (path[i] != '/') continue;
        std::string prefix = path.substr(0, i);
        if (::mkdir(prefix.c_str(), 0755) != 0 && errno != EEXIST) die("mkdir " + prefix);
    }
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) die("mkdir " + path);
}

void* alloc_aligned(size_t n) {
    void* p = nullptr;
    if (posix_memalign(&p, kAlign, n) != 0) die("posix_memalign");
    return p;
}

// ── build:造数据 ────────────────────────────────────────────────────────
// 必须真写数据, 不能 fallocate 了事:xfs 的 fallocate 留下 unwritten extent,
// 首次 O_DIRECT 读会撞上 extent 转换, 那是另一种开销, 会污染测量。
// (项目里 uring tier 的 prewarm 干的就是这件事, 见 BENCH_ANALYSIS 的 punt 机制一节)
void do_build(const Config& c) {
    char* buf = static_cast<char*>(alloc_aligned(c.block_size));
    std::memset(buf, 0xAB, c.block_size);
    mkdirs(c.dir);

    double t0 = now_us();
    if (c.layout == "files") {
        for (size_t i = 0; i < c.blocks; ++i) {
            std::string p = block_path(c.dir, i);
            mkdirs(p.substr(0, p.rfind('/')));
            int fd = ::open(p.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_DIRECT, 0644);
            if (fd < 0) die("open(build) " + p);
            if (::write(fd, buf, c.block_size) != static_cast<ssize_t>(c.block_size))
                die("write(build) " + p);
            ::close(fd);
            if ((i + 1) % 2000 == 0)
                std::fprintf(stderr, "  已建 %zu/%zu 文件\n", i + 1, c.blocks);
        }
    } else {
        int fd = ::open(slab_path(c.dir).c_str(),
                        O_CREAT | O_WRONLY | O_TRUNC | O_DIRECT, 0644);
        if (fd < 0) die("open(build) slab");
        for (size_t i = 0; i < c.blocks; ++i) {
            if (::pwrite(fd, buf, c.block_size, static_cast<off_t>(i * c.block_size))
                != static_cast<ssize_t>(c.block_size)) die("pwrite(build)");
        }
        ::fsync(fd);
        ::close(fd);
    }
    std::fprintf(stderr, "build 完成: layout=%s blocks=%zu 用时 %.1fs\n",
                 c.layout.c_str(), c.blocks, (now_us() - t0) / 1e6);
    std::free(buf);
}

// ── 统计 ────────────────────────────────────────────────────────────────
struct Stats {
    double mean, p50, p99, max, sum;
};

Stats summarize(std::vector<double> v) {
    Stats s{};
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.sum  = std::accumulate(v.begin(), v.end(), 0.0);
    s.mean = s.sum / v.size();
    s.p50  = v[v.size() / 2];
    s.p99  = v[static_cast<size_t>(v.size() * 0.99)];
    s.max  = v.back();
    return s;
}

// 按时间顺序取一段的均值(不排序!)—— 冷条件衰减检查专用。
//
// drop_caches 是一次性动作, 之后没有持续压力, 缓存会自己长回来:头几次 open
// 把目录块和 inode 表块读进 page cache, 而 xfs 的 inode 是按 chunk 成批存的
// (一个块里装好几个 inode), 后面的 open 就可能白蹭到前面读进来的块。跑到
// 后半段, "冷"可能已经自己捂热了一半。
// 所以每一档都要看前半/后半的 open 均值:两者接近 = 冷条件全程维持, 数可信;
// 后半明显更快 = 冷在衰减, 这一档的均值被稀释了, 要改用 ballast 档(持续压力)
// 或只取前半段做判读。不查这个, 数字就是不可信的。
double mean_slice(const std::vector<double>& v, size_t from, size_t to) {
    if (from >= to || to > v.size()) return 0.0;
    return std::accumulate(v.begin() + from, v.begin() + to, 0.0) / (to - from);
}

// 周期性重 drop:把冷条件从"一次性事件"变成"持续状态"。
//
// 不这么做的话冷态只存在于头 100 次读(约 0.07s):元数据体积很小(15000 个
// inode 才几 MB), 一旦被读回 page cache 就全热了, 后面全是热态 —— 均值被稀释
// (实测 -85%), 而且 iostat 的 1 秒粒度根本抓不到那 0.07 秒的冷窗口。
// 每读 N 次清一遍, 全程冷, 均值直接可读, iostat 也有东西可采。
// 这也更贴 e2e:那里 churn 一直在写新文件, 元数据缓存本来就在被持续冲刷。
//
// 注意 drop 的耗时不计入任何分段计时(它在读与读之间做), 但它确实会占墙钟,
// 所以 --redrop-every 开着时 mibps 不再是设备吞吐, 别拿去比。
void drop_caches(int level) {
    int fd = ::open("/proc/sys/vm/drop_caches", O_WRONLY);
    if (fd < 0) die("open drop_caches(需要 root)");
    ::sync();
    char c = static_cast<char>('0' + level);
    if (::write(fd, &c, 1) != 1) die("write drop_caches");
    ::close(fd);
}

// slab 里 dentry / inode 的对象数:自变量有没有真的动, 全靠它。
// probe 前后各读一次 —— drop_caches 之后如果这两个数没掉, 整个实验就是空转。
void dump_slab_counts(const char* tag) {
    std::ifstream f("/proc/slabinfo");
    if (!f) { std::fprintf(stderr, "  [%s] slabinfo 读不到(需要 root)\n", tag); return; }
    std::string line;
    std::getline(f, line);  // "slabinfo - version"
    std::getline(f, line);  // 表头
    while (std::getline(f, line)) {
        if (line.rfind("dentry ", 0) == 0 || line.rfind("inode_cache ", 0) == 0 ||
            line.rfind("xfs_inode ", 0) == 0 || line.rfind("ext4_inode_cache ", 0) == 0) {
            char name[64]; long active = 0;
            if (std::sscanf(line.c_str(), "%63s %ld", name, &active) == 2)
                std::fprintf(stderr, "  [%s] slab %-18s active_objs=%ld\n", tag, name, active);
        }
    }
}

// ── probe:随机读 + 分段计时 ─────────────────────────────────────────────
void do_probe(const Config& c) {
    // 随机块序列。三个布局吃同一个 seed → 读的是同一批块, 对比才干净。
    std::vector<size_t> order(c.blocks);
    std::iota(order.begin(), order.end(), 0);
    std::mt19937_64 rng(c.seed);
    std::shuffle(order.begin(), order.end(), rng);

    char* buf = static_cast<char*>(alloc_aligned(c.block_size));
    std::vector<double> t_open, t_read, t_close;
    t_open.reserve(c.reads); t_read.reserve(c.reads); t_close.reserve(c.reads);

    dump_slab_counts("probe 前");

    int slab_fd = -1;
    if (c.layout == "slab") {
        // fd 复用:整个 probe 只 open 一次, 不计入循环 —— 这就是 uring tier 的形态
        slab_fd = ::open(slab_path(c.dir).c_str(), O_RDONLY | O_DIRECT);
        if (slab_fd < 0) die("open slab");
    }

    double wall0 = now_us();
    for (size_t k = 0; k < c.reads; ++k) {
        // 重 drop 放在计时窗口之外 —— 它清的是内核缓存, 不该算进 open/read 的账。
        // level 跟着本档的语义走:drop2 档要一直只丢 slab, drop3 档要连 page cache
        if (c.redrop && k && k % c.redrop == 0) drop_caches(c.redrop_level);

        size_t idx = order[k % order.size()];

        if (c.layout == "files") {
            std::string p = block_path(c.dir, idx);
            double a = now_us();
            int fd = ::open(p.c_str(), O_RDONLY | O_DIRECT);
            double b = now_us();
            if (fd < 0) die("open " + p);
            if (::read(fd, buf, c.block_size) != static_cast<ssize_t>(c.block_size))
                die("read " + p);
            double d = now_us();
            ::close(fd);
            double e = now_us();
            t_open.push_back(b - a); t_read.push_back(d - b); t_close.push_back(e - d);

        } else if (c.layout == "slab-reopen") {
            // 每次重开同一个文件:open 次数与 files 相同, 但 dentry 只有一条。
            // files 与它的差值 = "N 条 dentry" 的代价, 与 open() 本身无关。
            std::string p = slab_path(c.dir);
            double a = now_us();
            int fd = ::open(p.c_str(), O_RDONLY | O_DIRECT);
            double b = now_us();
            if (fd < 0) die("open slab(reopen)");
            if (::pread(fd, buf, c.block_size, static_cast<off_t>(idx * c.block_size))
                != static_cast<ssize_t>(c.block_size)) die("pread slab(reopen)");
            double d = now_us();
            ::close(fd);
            double e = now_us();
            t_open.push_back(b - a); t_read.push_back(d - b); t_close.push_back(e - d);

        } else {  // slab
            double b = now_us();
            if (::pread(slab_fd, buf, c.block_size, static_cast<off_t>(idx * c.block_size))
                != static_cast<ssize_t>(c.block_size)) die("pread slab");
            double d = now_us();
            t_read.push_back(d - b);
        }
    }
    double wall = (now_us() - wall0) / 1e6;
    if (slab_fd >= 0) ::close(slab_fd);

    dump_slab_counts("probe 后");

    // 衰减检查要在 summarize 之前切 —— summarize 会排序, 时间顺序就没了
    size_t half = t_open.size() / 2;
    double open_h1 = mean_slice(t_open, 0, half);
    double open_h2 = mean_slice(t_open, half, t_open.size());

    // 分桶衰减曲线(每 1/10 一桶)。前后两半只能告诉你"衰减了", 这条曲线
    // 才能告诉你"衰减到哪儿收敛"—— 收敛值就是稳态的冷成本, 是能拿去和
    // e2e 对账的那个数; 首桶则是"元数据全冷"的上界。
    std::string decay = "[";
    size_t nb = 10, bs = t_open.size() / nb;
    for (size_t b = 0; bs > 0 && b < nb; ++b) {
        char tmp[32];
        std::snprintf(tmp, sizeof(tmp), "%s%.1f", b ? "," : "",
                      mean_slice(t_open, b * bs, (b + 1) * bs));
        decay += tmp;
    }
    decay += "]";

    Stats o = summarize(t_open), r = summarize(t_read), cl = summarize(t_close);
    double mib = static_cast<double>(c.reads) * c.block_size / (1 << 20);

    // JSON 一行, 给 run_meta_probe.sh 汇总
    std::printf(
        "{\"layout\":\"%s\",\"blocks\":%zu,\"reads\":%zu,\"block_size\":%zu,"
        "\"wall_s\":%.3f,\"mib\":%.1f,\"mibps\":%.1f,"
        "\"open_us\":{\"mean\":%.2f,\"p50\":%.2f,\"p99\":%.2f,\"max\":%.2f,\"total_ms\":%.1f,"
        "\"first_half\":%.2f,\"second_half\":%.2f,\"decay\":%s},"
        "\"read_us\":{\"mean\":%.2f,\"p50\":%.2f,\"p99\":%.2f,\"max\":%.2f,\"total_ms\":%.1f},"
        "\"close_us\":{\"mean\":%.2f,\"p50\":%.2f,\"p99\":%.2f,\"max\":%.2f,\"total_ms\":%.1f}}\n",
        c.layout.c_str(), c.blocks, c.reads, c.block_size, wall, mib, mib / wall,
        o.mean, o.p50, o.p99, o.max, o.sum / 1e3, open_h1, open_h2, decay.c_str(),
        r.mean, r.p50, r.p99, r.max, r.sum / 1e3,
        cl.mean, cl.p50, cl.p99, cl.max, cl.sum / 1e3);

    std::free(buf);
}

void usage() {
    std::fprintf(stderr,
        "用法:\n"
        "  meta_probe build --dir D --layout files|slab [--blocks N] [--block-size B]\n"
        "  meta_probe probe --dir D --layout files|slab|slab-reopen [--blocks N]\n"
        "                   [--block-size B] [--reads M] [--seed S] [--redrop-every N]\n"
        "\n"
        "--redrop-every N: 每读 N 次重新 drop_caches(需 root), 维持全程冷条件。\n"
        "  不开的话冷态只存在于头 ~100 次读, 均值会被后面的热态稀释(实测 -85%%)。\n"
        "\n"
        "注意: slab 与 slab-reopen 共用同一份 build(--layout slab)。\n"
        "缓存条件(hot / drop2 / drop3)由外部 drop_caches 构造, 见 run_meta_probe.sh。\n");
    std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) usage();
    Config c;
    c.mode = argv[1];
    for (int i = 2; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if      (k == "--dir")        c.dir = v;
        else if (k == "--layout")     c.layout = v;
        else if (k == "--blocks")     c.blocks = std::stoul(v);
        else if (k == "--block-size") c.block_size = std::stoul(v);
        else if (k == "--reads")      c.reads = std::stoul(v);
        else if (k == "--redrop-every") c.redrop = std::stoul(v);
        else if (k == "--redrop-level") c.redrop_level = std::stoi(v);
        else if (k == "--seed")       c.seed = std::stoull(v);
        else usage();
    }
    if (c.dir.empty()) usage();
    if (c.block_size % kAlign != 0) {
        std::fprintf(stderr, "block-size 必须是 %zu 的倍数(O_DIRECT 对齐)\n", kAlign);
        return 2;
    }
    if (c.mode == "build")      do_build(c);
    else if (c.mode == "probe") do_probe(c);
    else usage();
    return 0;
}
