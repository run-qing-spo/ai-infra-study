#pragma once

// KvTierEngine — vLLM secondary tier 的磁盘 IO 引擎。
//
// 定位:把 project2 (SPSC) + project3 (io_uring backend) + project4 (slot 布局)
// 组装成一个能挂进 vLLM TieringOffloadingSpec 的数据面。分层跟 P4 一致:
//   策略层(Python UringSecondaryTierManager):key→slot 账本、LRU 淘汰、job 记账
//   数据层(本类):只认字节 —— (mem_offset, disk_offset, block_bytes) 三元组
//
// 线程模型(这是本项目要讲的核心故事):
//   调度线程(vLLM scheduler 进程里调 submit/poll 的那个线程)
//        │ SPSC in_q_ (无锁, 单生产者=调度线程, 单消费者=worker)
//        ▼
//   worker 线程(唯一碰 io_uring 的线程)
//        │ 组批 → UringBackend::submit(一次 syscall N 个块) → reap
//        ▼ SPSC out_q_ (单生产者=worker, 单消费者=调度线程)
//   调度线程 poll() 收割 JobResult
//
//   为什么单线程 submit:io_uring 的 ring 本身不是 MT-safe(见 P3 注释),
//   多线程要么每线程一个 ring(fd 队列深度分裂), 要么加锁(回到 fs tier
//   线程池的老路)。SPSC + 单 submitter 让数据面完全无锁, 提交批量化,
//   这正是对照 vLLM 自带 fs tier (16+16 线程池, 每块一次 pwrite) 的差异点。
//
// 对齐契约(O_DIRECT 的前提, 由 vLLM 侧天然保证, 构造时仍然验证):
//   - mem_base: SharedOffloadRegion 是 mmap → 页对齐
//   - block_bytes: spec 里 round_up 到 BLOCK_SIZE_ALIGNMENT (= PAGESIZE)
//   - 磁盘 offset = slot * block_bytes, 内存 offset = block_id * block_bytes
//     → 全部 4KB 对齐, 不需要 bounce buffer
//
// 持久性:cache tier 语义, 不 fsync(同 P4 SsdBlockStore 注释)。掉电丢了
// 就退化成 recompute, KV cache 可重算是整个分层设计的安全网。

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "spsc_queue.hpp"   // project2
#include "io_backend.hpp"   // project3

namespace p5 {

// 一个 block 的搬运描述。方向由所属 JobDesc 决定。
struct BlockXfer {
    uint64_t disk_offset;   // backing 文件内字节偏移 (slot * block_bytes)
    uint64_t mem_offset;    // primary CPU 区域内字节偏移 (block_id * block_bytes)
};

// 一个 job = vLLM 提交的一批同方向 block。job 是完成粒度:
// 所有 block 都落定才向上报一次 JobResult —— 跟 SecondaryTierManager
// 的 get_finished_jobs() 语义对齐。
struct JobDesc {
    uint64_t job_id;
    bool     is_write;              // true: store(内存→盘)  false: load(盘→内存)
    std::vector<BlockXfer> xfers;
    // per-job 账本的提交侧字段, submit() 里填, 调用方不用管
    double   t_submit = 0.0;        // CLOCK_REALTIME 秒
    uint32_t q_jobs_at_submit = 0;  // 提交时已在途的 job 数(不含自己)
};

struct JobResult {
    uint64_t job_id;
    bool     success;               // 任一 block 短读/短写/errno 即 false
};

// per-job 账本(COMPARE_PLAN E2 工具缺口之二):每个 job 完成时记一条,
// Python 侧 drain_records() 低频批量取走。时间戳统一 CLOCK_REALTIME 的
// 秒 —— 必须能和 bench 的 t_wall(time.time())、iostat 时间轴直接对齐,
// 这是"+10ms 通路定位不能靠 iostat 从外面猜"的前提。两段分解:
//   queue   = t_first_issue - t_submit(SPSC 排队 + worker 忙 + gather 等待)
//   service = t_done - t_first_issue(ring 里飞的时间, 含 io-wq punt)
struct JobRecord {
    uint64_t job_id;
    bool     is_write;
    uint32_t n_blocks;
    double   t_submit;               // 调度线程 push 进 in_q_ 的时刻
    double   t_first_issue;          // worker 首次为它向 ring 下发的时刻
    double   t_done;                 // 最后一个 block 落定的时刻
    uint32_t q_jobs_at_submit;       // 提交时已在途的 job 数(不含自己)
    uint32_t dev_inflight_at_issue;  // 首次下发时设备在飞的 block 数
};

// fallocate 预留出来的 extent 是 unwritten 状态, 首写要做状态转换(元数据
// 变更), 文件系统层 NOWAIT 会失败 → IO 被 punt 给 io-wq(BENCH_ANALYSIS
// 的 punt 机制一节:预写后 store iou_wrk 6→0)。本函数把整个文件真写一遍
// 零, 把 extent 全部翻成 written —— 当年 dd 预写的代码内等价物。注意
// posix_fallocate / FALLOC_FL_ZERO_RANGE 造出来的都还是 unwritten, 绕不
// 过去, 必须真写。成本 = 文件大小 / 盘顺序写带宽(20G 按 dd 标定的
// 2.2GB/s 约 9s), 一次性启动开销, 顺带兼当"盘真的能写"的启动自检。
// 同步 pwrite、不走 ring:不污染 EngineStats 的 bench 记账。
void prewarm_file(int fd, uint64_t file_bytes);

// 账本时钟(两个引擎共用, 同 prewarm_file 的共享模式)。CLOCK_REALTIME
// 而非 steady:NTP 步进的精度损失对 ms 级分析无感, 换来和 bench 的
// t_wall(time.time())、iostat 时间轴零换算对齐。
double now_realtime();

// 全部单调递增, 调度线程读个近似值做观测就够了
struct EngineStats {
    uint64_t jobs_submitted = 0;
    uint64_t jobs_completed = 0;
    uint64_t jobs_failed    = 0;
    uint64_t ops_completed  = 0;    // 完成的 block IO 数
    uint64_t bytes_written  = 0;
    uint64_t bytes_read     = 0;
    uint64_t submit_calls   = 0;    // UringBackend::submit 调用次数(≈syscall 数)
    uint64_t sq_full_events = 0;    // 想发但 ring 满被顶回的次数(压力观测)
};

class KvTierEngine {
public:
    // path       : backing 文件路径, O_CREAT|O_RDWR 打开并预分配到 file_bytes
    // mem_base   : primary tier CPU 区域基址(vLLM 的 shm mmap, 生命周期由调用方保证)
    // block_bytes: 一个 offloaded block 的字节数(= primary view 的 stride)
    // queue_depth: io_uring SQ/CQ 深度, 也是 in-flight block IO 上限
    // use_odirect: 要求对齐契约成立, 否则构造抛 std::invalid_argument,
    //              调用方可以降级 use_odirect=false 重试(走 page cache)
    // prewarm    : 构造时把整个文件预写一遍零(见 prewarm_file 注释)。
    //              默认 false 保持微基准的既有协议(预写与否由 bench 控制);
    //              e2e 场景由 Python manager 默认打开。
    KvTierEngine(const std::string& path, uint64_t file_bytes,
                 void* mem_base, size_t mem_bytes,
                 uint32_t block_bytes, size_t queue_depth, bool use_odirect,
                 bool prewarm = false);
    ~KvTierEngine();

    KvTierEngine(const KvTierEngine&) = delete;
    KvTierEngine& operator=(const KvTierEngine&) = delete;

    // 调度线程调用(单生产者)。非阻塞;in_q_ 满返回 false, 调用方按 job 失败处理。
    bool submit(JobDesc&& job);

    // 调度线程调用(单消费者)。非阻塞, 最多取 max_n 个完成的 job。
    size_t poll(std::vector<JobResult>& out, size_t max_n);

    // 阻塞到所有已接受的 job 出结果(结果仍从 poll 取)。对应 drain_jobs()。
    void drain();

    EngineStats stats() const;

    // 取走并清空已完成 job 的账本。调用方(Python dump 循环)~10s 来一次,
    // 低频, 锁开销可忽略。
    std::vector<JobRecord> drain_records();

private:
    void worker_loop();

    int          fd_ = -1;
    std::byte*   mem_base_;
    size_t       mem_bytes_;
    uint32_t     block_bytes_;
    size_t       queue_depth_;

    spsc::SpscQueue<JobDesc>   in_q_;
    spsc::SpscQueue<JobResult> out_q_;

    // 控制面锁:只用来"叫醒睡着的 worker"和"drain 等完成", 不在数据面路径上。
    // worker 忙的时候生产者 push 完拿一下锁再 notify, 开销可忽略;
    // 数据本身始终走无锁 SPSC。
    std::mutex              wake_mu_;
    std::condition_variable wake_cv_;    // 生产者 → worker:有新活
    std::condition_variable drain_cv_;   // worker → drain 调用方:有 job 完成

    // submit 接受 +1, worker 把结果推进 out_q_ 后 -1。drain 等它归零。
    std::atomic<uint64_t> pending_jobs_{0};
    std::atomic<bool>     stop_{false};

    // stats 用 relaxed atomic:观测量, 不参与同步
    std::atomic<uint64_t> st_jobs_submitted_{0}, st_jobs_completed_{0},
                          st_jobs_failed_{0}, st_ops_completed_{0},
                          st_bytes_written_{0}, st_bytes_read_{0},
                          st_submit_calls_{0}, st_sq_full_events_{0};

    // per-job 账本:worker 在 finish_job 时 push(每 job 一次, 不在 block
    // 热路径), Python 低频 drain。一把小锁最简单可靠, 不值得上第三条 SPSC
    // (SPSC 定容会丢账, 账本丢一条对不上 jobs_submitted 就没法对账了)。
    std::mutex             records_mu_;
    std::vector<JobRecord> records_;

    std::thread worker_;
};

} // namespace p5
