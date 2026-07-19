#pragma once

// PoolTierEngine — 对照组引擎:std::thread 线程池 + 同步 pread/pwrite。
//
// 存在的唯一理由(BENCH_ANALYSIS §4):bench 里的 pool/pool-slab 是 Python
// 线程池, 拿它们和 uring 引擎对比, 等于同时比 "C++ vs Python(GIL)" 和
// "io_uring vs 线程池" 两个变量, 归因拆不开。本类把 Python 变量拆掉:
//
//   - 和 KvTierEngine 完全相同:单大文件 + fallocate、O_DIRECT、同一套
//     JobDesc/JobResult/EngineStats、同样从调度线程 submit/poll;
//   - 唯一差异:worker 侧是 N 个线程各自同步 pread/pwrite(每 block 一次
//     syscall), 而不是单线程 io_uring 批量提交。
//
// 对照关系:cpp-pool vs pool-slab 的差 = GIL/Python 开销;
//          uring   vs cpp-pool  的差 = 提交模型(io_uring vs 同步 syscall),
//          这才是 apples-to-apples 的 io_uring 归因。
//
// 线程模型:调度线程 submit() 把 job 拆成 block 任务挂进共享队列(mutex 保
// 护), N 个 worker 抢任务、同步 IO、最后一个 block 落定的 worker 报 JobResult。
// 有意用锁队列而不是 SPSC —— 线程池本来就得多消费者, 锁竞争是这个提交模型
// 自带的成本, 属于被测对象的一部分, 不该"优化"掉。

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "kv_tier_engine.hpp"   // 复用 JobDesc / JobResult / EngineStats

namespace p5 {

class PoolTierEngine {
public:
    // 参数语义与 KvTierEngine 一致, 只把 queue_depth 换成 num_threads:
    // 线程池的"并发深度"就是线程数(每线程同一时刻一个同步 IO 在飞)。
    // prewarm 同 KvTierEngine —— 对照组必须吃同样的 extent 状态。
    PoolTierEngine(const std::string& path, uint64_t file_bytes,
                   void* mem_base, size_t mem_bytes,
                   uint32_t block_bytes, size_t num_threads, bool use_odirect,
                   bool prewarm = false);
    ~PoolTierEngine();

    PoolTierEngine(const PoolTierEngine&) = delete;
    PoolTierEngine& operator=(const PoolTierEngine&) = delete;

    // 调度线程调用。任务队列无界, 永远返回 true —— uring 版的 false 语义
    // (环满)在这里不存在, 背压表现为任务在队列里排队。
    bool submit(JobDesc&& job);

    // 调度线程调用。非阻塞, 最多取 max_n 个完成的 job。
    size_t poll(std::vector<JobResult>& out, size_t max_n);

    // 阻塞到所有已接受的 job 出结果(结果仍从 poll 取)。
    void drain();

    EngineStats stats() const;

    // per-job 账本, 语义同 KvTierEngine::drain_records(pybind 模板要求两个
    // 引擎接口同构)。"first issue" 在本引擎 = 第一个 block 被 worker 拿起
    // 做同步 IO 的时刻;dev_inflight = 此刻正在 IO 里的 block 数(= 忙线程数)。
    std::vector<JobRecord> drain_records();

private:
    void worker_loop();

    // job 的完成记账。remaining 在 mu_ 保护下修改, 不需要 atomic。
    struct JobState {
        uint64_t job_id;
        uint32_t remaining;        // 还没落定的 block 数
        bool     failed = false;
        // per-job 账本字段(kv_tier_engine.hpp 的 JobRecord 注释)
        bool     is_write = false;
        uint32_t n_blocks = 0;
        double   t_submit = 0.0;
        double   t_first_issue = 0.0;   // 0 = 还没有 worker 碰过它
        uint32_t q_jobs_at_submit = 0;
        uint32_t dev_inflight_at_issue = 0;
    };
    // 一个 block 的搬运任务 = worker 的抢单粒度。
    struct BlockTask {
        uint64_t  seq;             // 指回 jobs_ 里的 JobState
        BlockXfer x;
        bool      is_write;
    };

    int        fd_ = -1;
    std::byte* mem_base_;
    size_t     mem_bytes_;
    uint32_t   block_bytes_;

    // 一把锁护住 tasks_/jobs_/results_ 三件套。临界区都是指针搬运级别,
    // 相对 1MB 量级的同步 IO(百微秒)可忽略;真要出现在 profile 里,
    // 那也是线程池模型的真实成本(见头注)。
    std::mutex mu_;
    std::condition_variable work_cv_;    // 生产者 → worker:有新任务
    std::condition_variable drain_cv_;   // worker → drain 调用方:有 job 完成
    std::deque<BlockTask> tasks_;
    std::unordered_map<uint64_t, JobState> jobs_;
    std::deque<JobResult> results_;
    // 完成 job 的账本(挂在 mu_ 下, 不另设锁 —— 本引擎三件套本来就共一把锁,
    // records 的读写全在既有临界区里, 顺路)
    std::vector<JobRecord> records_;
    uint64_t next_seq_ = 0;

    std::atomic<uint64_t> pending_jobs_{0};
    std::atomic<bool>     stop_{false};
    // 正在同步 IO 里的 block 数(锁外自增减, 只做 dev_inflight 快照观测)
    std::atomic<uint32_t> io_inflight_{0};

    // stats 用 relaxed atomic:观测量, 不参与同步(同 KvTierEngine)。
    // 注意 submit_calls 在本引擎的语义是 pread/pwrite 调用数 = 每 block
    // 一次 syscall —— 这正是和 uring 引擎对照的那条轴, 真实计数不估算。
    std::atomic<uint64_t> st_jobs_submitted_{0}, st_jobs_completed_{0},
                          st_jobs_failed_{0}, st_ops_completed_{0},
                          st_bytes_written_{0}, st_bytes_read_{0},
                          st_submit_calls_{0}, st_sq_full_events_{0};

    std::vector<std::thread> workers_;
};

} // namespace p5
