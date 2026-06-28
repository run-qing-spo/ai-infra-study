#pragma once

#include "io_backend.hpp"

// 仅 Linux:liburing 头里 include <linux/io_uring.h>,Mac 编译不过
#ifdef __linux__
#include <liburing.h>
#endif

namespace p3 {

// io_uring 后端。queue_depth 是 SQ/CQ 环的大小(也是 in-flight IO 的上限)。
// 单线程使用:多线程共享同一个 ring 需要额外同步(io_uring 本身不是 MT-safe),
// 当前实现不支持 → benchmark driver 也是单线程 submit/reap。
class UringBackend : public IoBackend {
public:
    explicit UringBackend(size_t queue_depth);
    ~UringBackend() override;

    UringBackend(const UringBackend&) = delete;
    UringBackend& operator=(const UringBackend&) = delete;

    size_t submit(const IoRequest* reqs, size_t n) override;
    size_t reap(IoCompletion* out, size_t max_n, size_t min_complete) override;
    size_t in_flight() const override;

private:
#ifdef __linux__
    io_uring ring_{};
#endif
    size_t in_flight_{0};
    size_t queue_depth_;
};

} // namespace p3
