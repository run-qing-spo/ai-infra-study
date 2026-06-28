#include "uring_backend.hpp"

#ifndef __linux__
#  error "UringBackend requires Linux + liburing"
#endif

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace p3 {

UringBackend::UringBackend(size_t queue_depth) : queue_depth_(queue_depth) {
    // io_uring_queue_init: 内核分配一对 ring(SQ + CQ)的物理页,
    // 然后 mmap 进我们的进程地址空间。返回后 ring_ 里的指针指向那段
    // 用户/内核共享的内存。0 = 默认 flags(没开 SQPOLL / IOPOLL)。
    int ret = io_uring_queue_init(static_cast<unsigned>(queue_depth), &ring_, 0);
    if (ret < 0) {
        throw std::runtime_error(
            std::string("io_uring_queue_init failed: ") + std::strerror(-ret));
    }
}

UringBackend::~UringBackend() {
    io_uring_queue_exit(&ring_);
}

size_t UringBackend::submit(const IoRequest* reqs, size_t n) {
    size_t prepared = 0;
    for (size_t i = 0; i < n; ++i) {
        // io_uring_get_sqe: 拿一个 SQ entry 槽。SQ 满了返回 nullptr。
        // 注意此时还没走 syscall,只是在共享内存里占了个位置。
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) break;

        const IoRequest& r = reqs[i];
        if (r.op == IoRequest::READ) {
            io_uring_prep_read(sqe, r.fd, r.buf, r.size, r.offset);
        } else {
            io_uring_prep_write(sqe, r.fd, r.buf, r.size, r.offset);
        }
        // user_data 会原样跟着 CQE 返回,用来对应"这是哪个请求完成了"
        io_uring_sqe_set_data64(sqe, r.user_data);
        ++prepared;
    }

    // io_uring_submit: 一次 syscall(io_uring_enter)告诉内核"SQ 里有 prepared 个新请求,
    // 去处理吧"。返回内核实际消费的 SQE 数(正常等于 prepared)。
    // 关键点:N 个请求只触发 1 次 syscall,这是 io_uring 的核心收益之一。
    int submitted = io_uring_submit(&ring_);
    if (submitted < 0) {
        return 0;
    }
    in_flight_ += static_cast<size_t>(submitted);
    return static_cast<size_t>(submitted);
}

size_t UringBackend::reap(IoCompletion* out, size_t max_n, size_t min_complete) {
    if (min_complete > 0) {
        // io_uring_wait_cqe_nr: 阻塞直到 CQ 里至少有 min_complete 个完成事件。
        // 如果已经够了,直接返回不进 syscall。
        io_uring_cqe* cqe = nullptr;
        int ret = io_uring_wait_cqe_nr(&ring_, &cqe, static_cast<unsigned>(min_complete));
        if (ret < 0) return 0;
    }

    // 把当前 CQ 里 ready 的事件全部拽出来,最多 max_n 个。
    // io_uring_peek_cqe 不进 syscall(只读共享内存),所以这个循环很便宜。
    size_t reaped = 0;
    while (reaped < max_n) {
        io_uring_cqe* cqe = nullptr;
        int ret = io_uring_peek_cqe(&ring_, &cqe);
        if (ret == -EAGAIN || !cqe) break;
        if (ret < 0) break;

        out[reaped].user_data = io_uring_cqe_get_data64(cqe);
        out[reaped].res       = cqe->res;
        // 必须调用,告诉内核这个 CQE 槽我消费完了,可以复用
        io_uring_cqe_seen(&ring_, cqe);
        ++reaped;
    }
    in_flight_ -= reaped;
    return reaped;
}

size_t UringBackend::in_flight() const { return in_flight_; }

} // namespace p3
