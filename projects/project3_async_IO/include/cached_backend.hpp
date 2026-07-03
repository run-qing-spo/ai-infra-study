#pragma once

#include "io_backend.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

namespace p3 {

// 用户态 LRU cache 装饰器,可以套在任何 IoBackend 上。
//
// 架构:三个正交维度
//   backend:  sync / uring         (底层 IO 接口)
//   direct:   O_DIRECT on/off      (open 的一个 flag)
//   cache:    这个装饰器 on/off    (叠在 backend 之上)
//
// 早期版本(CachedUringBackend)把这层做成跟 sync/uring 平级的 backend 选项,
// 把装饰器和底层实现混进了一层枚举 → 层级错了。现在改成:构造时接管
// 任意一个 inner IoBackend,submit 时先查用户态 LRU,miss 才转发底层。
//
// 动机(面试考点):KV offload 场景为什么走 O_DIRECT + 自建 cache 而不是
// 依赖 kernel page cache?
//   1) kernel page cache 按 4KB 物理页做 LRU,管不了"这段 KV 属于哪个
//      request 的 prefix、能不能命中"这种语义级判断;
//   2) 高并发下 kernel LRU 可能在你不注意时把热数据驱逐掉(memory pressure);
//   3) O_DIRECT 让 kernel 完全退出缓存决策 → 用户态自己拿全部控制权。
// 本装饰器只搞"用户态 LRU"这一块;O_DIRECT 由上层通过 open flag 打开。
//
// 关键假设(简化):
//   - 所有 IO 大小 == block_size(构造时传入);
//   - offset 按 block_size 对齐;
//   - slot_count >= max_in_flight(保证 miss 一定能借到 slot,不需要排队);
//   - 不匹配上述约束的请求 → 透传 inner,不进 cache。
//
// 写策略:write-invalidate。写请求透传 inner,同时把该 offset 从
// cache 里剔掉(下次 read 该 offset 会 miss 重读)。学习项目里 write-back
// 收益有限、脏页管理复杂,先不做。
class CachedBackend : public IoBackend {
public:
    // inner 所有权由 CachedBackend 接管。max_in_flight 是外层最大并发请求数
    // (通常 = bench 的 qd),用来断言 slot_count 够用。
    CachedBackend(std::unique_ptr<IoBackend> inner,
                  size_t max_in_flight,
                  size_t slot_count,
                  uint32_t block_size);
    ~CachedBackend() override;

    CachedBackend(const CachedBackend&) = delete;
    CachedBackend& operator=(const CachedBackend&) = delete;

    size_t submit(const IoRequest* reqs, size_t n) override;
    size_t reap(IoCompletion* out, size_t max_n, size_t min_complete) override;
    size_t in_flight() const override;

    // 调试用统计,bench 收尾时打印命中率。不进接口,单独暴露。
    struct Stats {
        uint64_t hits{0};
        uint64_t misses{0};
        uint64_t writes_invalidated{0};
        uint64_t passthrough{0};   // 大小/对齐不匹配、走 inner 不进 cache
    };
    Stats stats() const { return stats_; }

private:
    // -1 表示不在链表里(未初始化 / pinned in-flight)
    static constexpr int32_t kNil = -1;

    struct Slot {
        void*    buf{nullptr};    // block_size 大小,4KB 对齐
        uint64_t offset{0};       // 当前 slot 缓存的文件 offset
        bool     valid{false};    // slot 是否有有效数据
        bool     pinned{false};   // 是否被 in-flight miss 借出(此时不在 LRU 上)
        int32_t  lru_prev{kNil};
        int32_t  lru_next{kNil};
    };

    // 待返回给上层的 completion(cache hit 或 miss 转成的最终完成)
    // hit 时 submit() 里立即塞;miss 时 inner 完成后再塞。
    std::deque<IoCompletion> pending_completions_;

    // inner 在飞的 miss 请求:internal_id -> 借出的 slot + user 上下文
    struct InflightMiss {
        size_t   slot_idx;
        uint64_t user_data;
        void*    user_buf;
        uint32_t user_size;
        bool     is_read;         // 目前只有 read 走这里;留一位备用
    };
    std::unordered_map<uint64_t, InflightMiss> misses_;

    // LRU 头(最近使用)和尾(最先被驱逐)。只挂 valid && !pinned 的 slot。
    int32_t lru_head_{kNil};
    int32_t lru_tail_{kNil};

    // offset -> slot_idx。valid slot 一定在这个表里;pinned slot 也在
    // (方便 write invalidate 时找到它)。
    std::unordered_map<uint64_t, int32_t> offset_index_;

    // 空闲 slot(还没被用过的 slot,slot 生命周期开始时全在这里)
    std::vector<int32_t> free_slots_;

    // slot 存储
    std::vector<Slot> slots_;
    uint32_t          block_size_;
    size_t            slot_count_;

    // 底层真 IO(any backend:sync / uring / uring+O_DIRECT / ...)
    std::unique_ptr<IoBackend> inner_;

    // 递增分配 internal_id,永不复用(避免 stale completion 撞车)
    uint64_t next_internal_id_{1};

    // 提交到 inner 但还没 reap 回来的数量(用于 in_flight())
    size_t inner_pending_{0};

    Stats stats_{};

    // LRU 双链表操作:O(1) 插头、摘除、touch
    void lru_push_front(int32_t idx);
    void lru_remove(int32_t idx);
    void lru_touch(int32_t idx);

    // 拿一个 slot 装新数据:优先 free_slots_,不够就驱逐 LRU 尾
    int32_t acquire_slot_for(uint64_t offset);
};

} // namespace p3
