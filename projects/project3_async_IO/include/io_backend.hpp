#pragma once

#include <cstddef>
#include <cstdint>

namespace p3 {

// 一次 IO 请求。上层填好后通过 backend->submit() 交给后端。
struct IoRequest {
    enum Op : uint8_t { READ, WRITE };
    Op       op;
    int      fd;          // 已经 open 好的文件描述符
    uint64_t offset;      // 文件内字节偏移
    void*    buf;         // 数据落在 / 写出 这个 buffer
    uint32_t size;        // 传输字节数
    uint64_t user_data;   // 由上层定义的 tag,会原样回到 IoCompletion,通常当 request id 用
};

// 一次 IO 完成。
struct IoCompletion {
    uint64_t user_data;   // 跟 IoRequest.user_data 对应
    int32_t  res;         // >=0:实际传输字节数; <0:-errno
};

// 后端接口:同步实现和 io_uring 实现都实现它,benchmark 用同一份代码跑。
//
// submit / reap 是 io_uring 原生形态:
//   - submit(reqs, n)        把一批请求挂到队列里
//   - reap(out, max, min)    最多取 max 个完成事件;min>0 时阻塞到至少有 min 个
//
// 这样设计的原因:同步后端也可以用 thread pool 模拟"提交一批 + 异步等结果",
// 跟 io_uring 后端在接口语义上一致,公平对比。
class IoBackend {
public:
    virtual ~IoBackend() = default;

    // 返回实际被接收的请求数(<=n,队列满时可能小于 n)
    virtual size_t submit(const IoRequest* reqs, size_t n) = 0;

    // min_complete=0: 非阻塞 poll;>0: 阻塞直到至少这么多完成事件 ready
    virtual size_t reap(IoCompletion* out, size_t max_n, size_t min_complete) = 0;

    // 已提交但还没被 reap 的请求数(近似值,给 benchmark 驱动收尾用)
    virtual size_t in_flight() const = 0;
};

} // namespace p3
