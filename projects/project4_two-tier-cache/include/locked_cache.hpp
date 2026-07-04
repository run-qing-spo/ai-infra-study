#pragma once

#include "block_store.hpp"
#include "eviction_policy.hpp"

#include <shared_mutex>

namespace p4 {

// Cache 的线程安全版本 —— 用 std::shared_mutex 保护 store + policy 的 metadata。
//
// 表面契约:get 用 shared_lock,put 用 unique_lock。理想里读并行、写独占。
//
// 但实操会立刻撞到一个坎:Cache::get 原本调 policy_.on_access(id),那步会写
// LRU 双向链表 + iterator hash 表 —— 是货真价实的写操作。如果 get 里保留
// on_access,shared_lock 就没意义了(多个 reader 会数据竞争),必须升级成
// unique_lock,退化成大锁。
//
// 出口就在"immutable 为什么有意义"这条线上:
//   KV cache 语义下,同 id 的内容由 token 序列决定,一旦被 write 就不会被别的
//   线程改写。这个"内容不变"的假设,把 store.read 变成真正的 pure read ——
//   只读 index_ 定位 slot,memcpy 出去,不动任何 metadata。
//   → 于是"get 里保留 on_access"这条不必要的写就可以砍掉,shared_lock 名副
//     其实,多个 reader 真并行。
//
// 代价:LRU 热度不再随 hit 刷新,淘汰序退化成"先来先走"(接近 FIFO)。
// 面试里被追这个:
//   - "生产上怎么办?" → CLOCK(每个 slot 一个 atomic bit 记访问,evict 时扫)、
//     2Q、或 batched LRU(线程本地 access log,后台线程 drain 到 policy)。
//   - "为啥不用 lock-free LRU?" → 双向链表的 splice 涉及三个指针互指,
//     lock-free 版本要么用 hazard pointer 要么用 epoch,复杂度爆炸,收益
//     还不一定打得过 CLOCK 这种"用简单近似换并发"的路子。
//
// put 端就朴素了:改 store + 改 policy,全走 unique_lock 独占,没啥可省的。
// 高竞争工作负载(小 cap + 高 miss rate)下瓶颈会移到这里,想缓解就 sharding:
// 按 id 哈希分片 → 每片一把 shared_mutex,写只锁自己那片。
class LockedCache {
public:
    LockedCache(BlockStore& store, EvictionPolicy& policy)
        : store_(store), policy_(policy) {}

    // shared_lock:只调 store_.read。参见类注释里对 on_access 的取舍。
    // hit 拿到字节,miss 返 false,dst 不动。
    bool get(BlockId id, std::byte* dst);

    // unique_lock:完整走覆盖 / 淘汰 / 插入路径,和 Cache::put 逻辑一致。
    void put(BlockId id, const std::byte* src);

    // 这两个也得上 shared_lock:size 依赖 store 内部的 index_.size(),
    // 和 evict/write 有 read-modify 冲突。capacity 是构造时定的常量,不用锁。
    size_t size() const;
    size_t capacity() const { return store_.capacity(); }

private:
    BlockStore&               store_;
    EvictionPolicy&           policy_;
    // mutable:让 size() 这种 logical-const 方法也能锁。shared_mutex 本身
    // 状态改变是"读多写少"的实现细节,不属于 Cache 的对外可见状态。
    mutable std::shared_mutex mu_;
};

} // namespace p4
