#include "kv_tier_engine.hpp"

#ifndef __linux__
#  error "KvTierEngine requires Linux (io_uring + O_DIRECT)"
#endif

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <unordered_map>

#include "uring_backend.hpp"   // project3

namespace p5 {

namespace {
constexpr size_t   kAlign     = 4096;   // O_DIRECT 对齐要求(现代内核实际是逻辑扇区 512, 按 4K 收紧)
constexpr size_t   kRingCap   = 1024;   // in_q_/out_q_ 容量(2 的幂)。一个 job 一格,
                                        // 1023 个在途 job 远超 scheduler 实际压力
constexpr unsigned kReapBatch = 256;    // 一次 reap 最多收的 CQE 数
} // namespace

KvTierEngine::KvTierEngine(const std::string& path, uint64_t file_bytes,
                           void* mem_base, size_t mem_bytes,
                           uint32_t block_bytes, size_t queue_depth,
                           bool use_odirect)
    : mem_base_(static_cast<std::byte*>(mem_base)),
      mem_bytes_(mem_bytes),
      block_bytes_(block_bytes),
      queue_depth_(queue_depth),
      in_q_(kRingCap),
      out_q_(kRingCap) {
    if (block_bytes_ == 0 || queue_depth_ == 0) {
        throw std::invalid_argument("block_bytes/queue_depth must be > 0");
    }
    if (use_odirect) {
        // 对齐契约验证。vLLM 侧 stride 按 PAGESIZE round_up + mmap 页对齐,
        // 正常永远不会走进这两个 throw;真触发说明上游布局变了, 宁可 fail fast。
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
    // 预分配整个文件:offset 由 slot 直接算出(P4 的 slab 思路), 且 fallocate
    // 预留连续 extent, 避免稀疏文件首写时的 extent 分配抖动。
    if (::posix_fallocate(fd_, 0, static_cast<off_t>(file_bytes)) != 0) {
        // 有些文件系统(如 tmpfs 之外的少数)不支持, 退化 ftruncate(稀疏文件)
        if (::ftruncate(fd_, static_cast<off_t>(file_bytes)) != 0) {
            int e = errno;
            ::close(fd_);
            throw std::runtime_error(std::string("preallocate failed: ") + std::strerror(e));
        }
    }

    worker_ = std::thread([this] { worker_loop(); });
}

KvTierEngine::~KvTierEngine() {
    stop_.store(true, std::memory_order_release);
    wake_cv_.notify_one();
    if (worker_.joinable()) worker_.join();
    if (fd_ >= 0) ::close(fd_);
    // 不 unlink:文件删不删是策略层(Python manager)的事, 引擎只管字节。
}

bool KvTierEngine::submit(JobDesc&& job) {
    pending_jobs_.fetch_add(1, std::memory_order_relaxed);
    if (!in_q_.push(std::move(job))) {
        pending_jobs_.fetch_sub(1, std::memory_order_relaxed);
        return false;   // 环满:调用方把这个 job 报失败, 上层退化 recompute
    }
    st_jobs_submitted_.fetch_add(1, std::memory_order_relaxed);
    // 生产者先拿锁再 notify:配合 worker "持锁试 pop 失败才 wait" 的顺序,
    // 关掉 "push 发生在检查之后、wait 之前" 的丢唤醒窗口。
    { std::lock_guard<std::mutex> g(wake_mu_); }
    wake_cv_.notify_one();
    return true;
}

size_t KvTierEngine::poll(std::vector<JobResult>& out, size_t max_n) {
    size_t n = 0;
    JobResult r;
    while (n < max_n && out_q_.pop(r)) {
        out.push_back(r);
        ++n;
    }
    return n;
}

void KvTierEngine::drain() {
    std::unique_lock<std::mutex> lk(wake_mu_);
    drain_cv_.wait(lk, [this] {
        return pending_jobs_.load(std::memory_order_acquire) == 0;
    });
}

EngineStats KvTierEngine::stats() const {
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

void KvTierEngine::worker_loop() {
    p3::UringBackend backend(queue_depth_);

    // gather threshold 随 QD 联动:min(32, QD/2), 至少为 1(§8.3)。
    // 写死 32 的坑:threshold == QD 时 gather 退化成 lockstep ——
    // "提交 QD 个 → room=0 → 阻塞等全部完成 → 盘空 → 再提交下一批",
    // 提交和执行完全串行, 每批头尾盘都在干等。QD/2 保证阻塞等待期间
    // 任何时刻至少还有一半 IO 在盘里飞, 批量摊薄和流水线两头都保住。
    // (dm 机器 QD 32→256 吞吐平坦, 联动主要是护住低 QD 默认配置,
    // 不是治已观测到的病 —— 见 BENCH_ANALYSIS §8.3 后记。)
    const size_t submit_threshold =
        std::max<size_t>(1, std::min<size_t>(32, queue_depth_ / 2));

    // 活跃 job 表。unordered_map 节点地址稳定, JobState* 可以塞进 user_data
    // 关联表。key 用内部序号而不是 job_id:引擎不该假设上游 id 永不复用。
    struct JobState {
        JobDesc  desc;
        size_t   next_xfer = 0;     // 下一个还没下发的 xfer 下标
        uint32_t inflight  = 0;     // 已下发未完成的 block 数
        bool     failed    = false;
    };
    std::unordered_map<uint64_t, JobState> jobs;
    std::deque<uint64_t> issue_order;   // FIFO 下发:先来的 job 先占 ring 深度
    uint64_t next_seq = 0;

    std::vector<p3::IoRequest>   reqs;
    std::vector<p3::IoCompletion> comps(kReapBatch);

    auto finish_job = [&](uint64_t seq, JobState& js) {
        bool ok = !js.failed;
        // out_q_ 满时自旋等调度线程来收。容量 1023, 实际到不了这里;
        // 真到了, 说明上层根本没在 poll, 背压停在引擎侧是对的。
        while (!out_q_.push(JobResult{js.desc.job_id, ok})) {
            std::this_thread::yield();
        }
        st_jobs_completed_.fetch_add(1, std::memory_order_relaxed);
        if (!ok) st_jobs_failed_.fetch_add(1, std::memory_order_relaxed);
        jobs.erase(seq);
        pending_jobs_.fetch_sub(1, std::memory_order_release);
        { std::lock_guard<std::mutex> g(wake_mu_); }
        drain_cv_.notify_all();
    };

    while (true) {
        bool made_progress = false;

        // ① 收新 job(SPSC pop, 无锁)
        JobDesc jd;
        while (in_q_.pop(jd)) {
            uint64_t seq = next_seq++;
            auto [it, _] = jobs.emplace(seq, JobState{std::move(jd)});
            // 空 job(全部 block 已在盘上被 Python 侧过滤光)直接完成
            if (it->second.desc.xfers.empty()) {
                finish_job(seq, it->second);
            } else {
                issue_order.push_back(seq);
            }
            made_progress = true;
        }

        // ② 组批下发:把 ring 的空余深度一次性填满, 然后一个 submit
        //    (= 一次 io_uring_enter syscall) 送走 —— 对照 fs tier
        //    每 block 一次 pwrite syscall, 这里是 P3 学到的核心收益。
        size_t room = queue_depth_ - backend.in_flight();
        // gather window: room 太小 + 还有活在飞 → 跳过这轮 submit,
        //   让 ③ 阻塞收一批 CQE, 下轮 room 攒够才 submit。
        //   issue_order 空时不 gather, 尾巴必须立刻 submit 不能 stall。
        bool skip_submit = (room < submit_threshold)
                           && (backend.in_flight() > 0)
                           && !issue_order.empty();
        if (skip_submit) room = 0;
        reqs.clear();
        while (room > 0 && !issue_order.empty()) {
            uint64_t seq = issue_order.front();
            auto it = jobs.find(seq);
            JobState& js = it->second;
            while (room > 0 && js.next_xfer < js.desc.xfers.size()) {
                const BlockXfer& x = js.desc.xfers[js.next_xfer];
                p3::IoRequest r;
                r.op        = js.desc.is_write ? p3::IoRequest::WRITE : p3::IoRequest::READ;
                r.fd        = fd_;
                r.offset    = x.disk_offset;
                r.buf       = mem_base_ + x.mem_offset;
                r.size      = block_bytes_;
                r.user_data = seq;   // 同 job 的 block 共享 seq, 完成时查表就够
                reqs.push_back(r);
                ++js.next_xfer;
                ++js.inflight;
                --room;
            }
            if (js.next_xfer == js.desc.xfers.size()) issue_order.pop_front();
        }
        if (!reqs.empty()) {
            size_t accepted = backend.submit(reqs.data(), reqs.size());
            st_submit_calls_.fetch_add(1, std::memory_order_relaxed);
            if (accepted < reqs.size()) {
                // SQ 满被顶回(理论上 room 已经挡住了;防御性处理):
                // 把没被接受的 xfer 游标回滚, 下轮重发。被顶回的一定是尾部
                // 且属于 issue_order 前端的 job —— 逐个退回去。
                st_sq_full_events_.fetch_add(1, std::memory_order_relaxed);
                for (size_t i = reqs.size(); i > accepted; --i) {
                    uint64_t seq = reqs[i - 1].user_data;
                    JobState& js = jobs.at(seq);
                    --js.next_xfer;
                    --js.inflight;
                    if (js.next_xfer + 1 == js.desc.xfers.size()) {
                        issue_order.push_front(seq);   // 之前被 pop 掉了, 放回去
                    }
                }
            }
            made_progress = true;
        }

        // ③ 收割完成。没有新活可干时用阻塞 reap(min=1)在内核里睡,
        //    有活时非阻塞 peek —— worker 只在真没事时才让出 CPU。
        if (backend.in_flight() > 0) {
            bool more_to_issue = !issue_order.empty();
            size_t min_complete;
            if (skip_submit) {
                // 关键: cap 到 in_flight, 否则等不齐 threshold 个 CQE 时死锁
                // (联动后 threshold ≤ QD/2, 这个 cap 理论上不再触发,
                //  但当初 QD=8 撞死锁的教训留着, 防御不删)
                min_complete = submit_threshold < backend.in_flight()
                               ? submit_threshold : backend.in_flight();
            } else {
                min_complete = (made_progress || more_to_issue) ? 0 : 1;
            }
            size_t n = backend.reap(comps.data(), kReapBatch, min_complete);
            for (size_t i = 0; i < n; ++i) {
                uint64_t seq = comps[i].user_data;
                JobState& js = jobs.at(seq);
                --js.inflight;
                if (comps[i].res != static_cast<int32_t>(block_bytes_)) {
                    js.failed = true;   // 短读/短写/负 errno 一律算失败
                } else {
                    st_ops_completed_.fetch_add(1, std::memory_order_relaxed);
                    auto& ctr = js.desc.is_write ? st_bytes_written_ : st_bytes_read_;
                    ctr.fetch_add(block_bytes_, std::memory_order_relaxed);
                }
                if (js.inflight == 0 && js.next_xfer == js.desc.xfers.size()) {
                    finish_job(seq, js);
                }
            }
            if (n > 0) made_progress = true;
        }

        if (stop_.load(std::memory_order_acquire) && jobs.empty()) {
            // 收尾前把 in_q_ 里残留的 job 也报失败, 不让 drain 卡死
            JobDesc leftover;
            while (in_q_.pop(leftover)) {
                while (!out_q_.push(JobResult{leftover.job_id, false})) {
                    std::this_thread::yield();
                }
                st_jobs_failed_.fetch_add(1, std::memory_order_relaxed);
                pending_jobs_.fetch_sub(1, std::memory_order_release);
            }
            drain_cv_.notify_all();
            break;
        }

        // ④ 真空转(没新 job、没在途 IO)才睡。持锁下最后再试一次 pop 之外的
        //    检查由 50ms 超时兜底 —— 即使极端 race 丢了唤醒, 也只多睡 50ms。
        if (!made_progress && backend.in_flight() == 0 && issue_order.empty()) {
            std::unique_lock<std::mutex> lk(wake_mu_);
            wake_cv_.wait_for(lk, std::chrono::milliseconds(50));
        }
    }
}

} // namespace p5
