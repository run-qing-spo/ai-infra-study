#pragma once

#include "io_backend.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace p3 {

// 同步基线:内部一个线程池,每个 worker 拿 IoRequest 后调 pread/pwrite。
// 这模拟"用 thread-per-pending-IO 撑高并发同步 IO"的传统玩法。
//
// num_workers = 同时可以阻塞在 pread/pwrite 上的线程数 = 实际并发深度。
// 跟 UringBackend 的 queue_depth 同义,benchmark 里二者会被设成同一个值做公平对比。
class SyncBackend : public IoBackend {
public:
    explicit SyncBackend(size_t num_workers);
    ~SyncBackend() override;

    size_t submit(const IoRequest* reqs, size_t n) override;
    size_t reap(IoCompletion* out, size_t max_n, size_t min_complete) override;
    size_t in_flight() const override;

private:
    void worker_loop();

    const size_t              num_workers_;
    std::vector<std::thread>  workers_;

    std::mutex                sq_mu_;
    std::condition_variable   sq_cv_;
    std::deque<IoRequest>     sq_;

    std::mutex                cq_mu_;
    std::condition_variable   cq_cv_;
    std::deque<IoCompletion>  cq_;

    std::atomic<size_t>       in_flight_{0};
    std::atomic<bool>         stop_{false};
};

} // namespace p3
