#include "sync_backend.hpp"

#include <algorithm>
#include <cerrno>
#include <unistd.h>

namespace p3 {

SyncBackend::SyncBackend(size_t num_workers) : num_workers_(num_workers) {
    workers_.reserve(num_workers_);
    for (size_t i = 0; i < num_workers_; ++i) {
        workers_.emplace_back(&SyncBackend::worker_loop, this);
    }
}

SyncBackend::~SyncBackend() {
    {
        std::lock_guard<std::mutex> lk(sq_mu_);
        stop_.store(true, std::memory_order_release);
    }
    sq_cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

size_t SyncBackend::submit(const IoRequest* reqs, size_t n) {
    {
        std::lock_guard<std::mutex> lk(sq_mu_);
        for (size_t i = 0; i < n; ++i) sq_.push_back(reqs[i]);
        in_flight_.fetch_add(n, std::memory_order_relaxed);
    }
    // n 个新请求 → 最多唤醒 n 个 worker。notify_all 简单但可能 thundering herd;
    // 对当前规模(几十个 worker)无所谓,先不优化。
    sq_cv_.notify_all();
    return n;
}

size_t SyncBackend::reap(IoCompletion* out, size_t max_n, size_t min_complete) {
    std::unique_lock<std::mutex> lk(cq_mu_);
    if (min_complete > 0) {
        cq_cv_.wait(lk, [&] { return cq_.size() >= min_complete; });
    }
    size_t n = std::min(max_n, cq_.size());
    for (size_t i = 0; i < n; ++i) {
        out[i] = cq_.front();
        cq_.pop_front();
    }
    return n;
}

size_t SyncBackend::in_flight() const {
    return in_flight_.load(std::memory_order_relaxed);
}

void SyncBackend::worker_loop() {
    while (true) {
        IoRequest req;
        {
            std::unique_lock<std::mutex> lk(sq_mu_);
            sq_cv_.wait(lk, [&] {
                return !sq_.empty() || stop_.load(std::memory_order_acquire);
            });
            if (sq_.empty()) return;   // stop_ 触发且队列空了 → 退出
            req = sq_.front();
            sq_.pop_front();
        }

        ssize_t res;
        if (req.op == IoRequest::READ) {
            res = ::pread(req.fd, req.buf, req.size, static_cast<off_t>(req.offset));
        } else {
            res = ::pwrite(req.fd, req.buf, req.size, static_cast<off_t>(req.offset));
        }
        int32_t r = (res >= 0) ? static_cast<int32_t>(res) : -errno;

        {
            std::lock_guard<std::mutex> lk(cq_mu_);
            cq_.push_back(IoCompletion{req.user_data, r});
            in_flight_.fetch_sub(1, std::memory_order_relaxed);
        }
        cq_cv_.notify_one();
    }
}

} // namespace p3
