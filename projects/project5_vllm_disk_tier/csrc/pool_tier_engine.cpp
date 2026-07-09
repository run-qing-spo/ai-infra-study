#include "pool_tier_engine.hpp"

#ifndef __linux__
#  error "PoolTierEngine requires Linux (O_DIRECT)"
#endif

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace p5 {

namespace {
constexpr size_t kAlign = 4096;   // O_DIRECT 对齐要求(同 KvTierEngine)
} // namespace

PoolTierEngine::PoolTierEngine(const std::string& path, uint64_t file_bytes,
                               void* mem_base, size_t mem_bytes,
                               uint32_t block_bytes, size_t num_threads,
                               bool use_odirect)
    : mem_base_(static_cast<std::byte*>(mem_base)),
      mem_bytes_(mem_bytes),
      block_bytes_(block_bytes) {
    if (block_bytes_ == 0 || num_threads == 0) {
        throw std::invalid_argument("block_bytes/num_threads must be > 0");
    }
    if (use_odirect) {
        // 对齐契约验证, 照抄 KvTierEngine —— 两个引擎必须吃同样的约束,
        // 否则对照不公平。
        if (reinterpret_cast<uintptr_t>(mem_base_) % kAlign != 0) {
            throw std::invalid_argument("mem_base not 4K-aligned, O_DIRECT impossible");
        }
        if (block_bytes_ % kAlign != 0) {
            throw std::invalid_argument("block_bytes not multiple of 4K, O_DIRECT impossible");
        }
    }

    int flags = O_CREAT | O_RDWR;
    if (use_odirect) flags |= O_DIRECT;
    fd_ = ::open(path.c_str(), flags, 0644);
    if (fd_ < 0) {
        throw std::runtime_error(std::string("open failed: ") + std::strerror(errno));
    }
    // 预分配策略与 KvTierEngine 一字不差:fallocate 优先, 不支持则退化
    // ftruncate。unwritten extent 首写转换的成本两个引擎同样要付
    // (BENCH_ANALYSIS §10.1), 预写与否由 bench 协议统一控制。
    if (::posix_fallocate(fd_, 0, static_cast<off_t>(file_bytes)) != 0) {
        if (::ftruncate(fd_, static_cast<off_t>(file_bytes)) != 0) {
            int e = errno;
            ::close(fd_);
            throw std::runtime_error(std::string("preallocate failed: ") + std::strerror(e));
        }
    }

    workers_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

PoolTierEngine::~PoolTierEngine() {
    stop_.store(true, std::memory_order_release);
    work_cv_.notify_all();
    // worker 的退出条件是 "stop 且队列已空"(见 worker_loop), 所以 join
    // 天然把残留任务干完, 不会留下没报结果的 job 卡死 drain。
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    if (fd_ >= 0) ::close(fd_);
    // 不 unlink:文件删不删是策略层的事, 引擎只管字节(同 KvTierEngine)。
}

bool PoolTierEngine::submit(JobDesc&& job) {
    pending_jobs_.fetch_add(1, std::memory_order_relaxed);
    st_jobs_submitted_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(mu_);
        // 空 job(block 全被 Python 侧过滤光)直接完成, 语义同 uring 版。
        if (job.xfers.empty()) {
            results_.push_back(JobResult{job.job_id, true});
            st_jobs_completed_.fetch_add(1, std::memory_order_relaxed);
            pending_jobs_.fetch_sub(1, std::memory_order_release);
            drain_cv_.notify_all();
            return true;
        }
        uint64_t seq = next_seq_++;
        jobs_.emplace(seq, JobState{job.job_id,
                                    static_cast<uint32_t>(job.xfers.size())});
        for (const BlockXfer& x : job.xfers) {
            tasks_.push_back(BlockTask{seq, x, job.is_write});
        }
    }
    // 一个 job 通常带一批 block, 叫醒所有 worker 抢单
    work_cv_.notify_all();
    return true;
}

size_t PoolTierEngine::poll(std::vector<JobResult>& out, size_t max_n) {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    while (n < max_n && !results_.empty()) {
        out.push_back(results_.front());
        results_.pop_front();
        ++n;
    }
    return n;
}

void PoolTierEngine::drain() {
    std::unique_lock<std::mutex> lk(mu_);
    drain_cv_.wait(lk, [this] {
        return pending_jobs_.load(std::memory_order_acquire) == 0;
    });
}

EngineStats PoolTierEngine::stats() const {
    EngineStats s;
    s.jobs_submitted = st_jobs_submitted_.load(std::memory_order_relaxed);
    s.jobs_completed = st_jobs_completed_.load(std::memory_order_relaxed);
    s.jobs_failed    = st_jobs_failed_.load(std::memory_order_relaxed);
    s.ops_completed  = st_ops_completed_.load(std::memory_order_relaxed);
    s.bytes_written  = st_bytes_written_.load(std::memory_order_relaxed);
    s.bytes_read     = st_bytes_read_.load(std::memory_order_relaxed);
    s.submit_calls   = st_submit_calls_.load(std::memory_order_relaxed);
    s.sq_full_events = st_sq_full_events_.load(std::memory_order_relaxed);
    return s;
}

void PoolTierEngine::worker_loop() {
    while (true) {
        BlockTask t;
        {
            std::unique_lock<std::mutex> lk(mu_);
            work_cv_.wait(lk, [this] {
                return stop_.load(std::memory_order_acquire) || !tasks_.empty();
            });
            if (tasks_.empty()) break;   // 只可能是 stop 且没活了
            t = tasks_.front();
            tasks_.pop_front();
        }

        // 锁外做同步 IO:每 block 一次 syscall, 这就是与 uring 引擎的对照点。
        // 短读/短写判定与 uring 侧同标准 —— 一次调用没搬完整个 block 就算
        // 失败, 不做 resume 循环(O_DIRECT 下剩余长度可能掉出 4K 对齐;
        // uring 侧 res != block_bytes 也是一票否决, 见 kv_tier_engine.cpp ③)。
        std::byte* buf = mem_base_ + t.x.mem_offset;
        ssize_t res = t.is_write
            ? ::pwrite(fd_, buf, block_bytes_, static_cast<off_t>(t.x.disk_offset))
            : ::pread(fd_, buf, block_bytes_, static_cast<off_t>(t.x.disk_offset));
        st_submit_calls_.fetch_add(1, std::memory_order_relaxed);
        bool ok = (res == static_cast<ssize_t>(block_bytes_));
        if (ok) {
            st_ops_completed_.fetch_add(1, std::memory_order_relaxed);
            auto& ctr = t.is_write ? st_bytes_written_ : st_bytes_read_;
            ctr.fetch_add(block_bytes_, std::memory_order_relaxed);
        }

        {
            std::lock_guard<std::mutex> lk(mu_);
            JobState& js = jobs_.at(t.seq);
            if (!ok) js.failed = true;
            if (--js.remaining == 0) {
                bool job_ok = !js.failed;
                results_.push_back(JobResult{js.job_id, job_ok});
                st_jobs_completed_.fetch_add(1, std::memory_order_relaxed);
                if (!job_ok) st_jobs_failed_.fetch_add(1, std::memory_order_relaxed);
                jobs_.erase(t.seq);
                pending_jobs_.fetch_sub(1, std::memory_order_release);
                drain_cv_.notify_all();
            }
        }
    }
}

} // namespace p5
