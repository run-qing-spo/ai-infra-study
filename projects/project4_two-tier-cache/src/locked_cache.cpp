#include "locked_cache.hpp"

#include <mutex>

namespace p4 {

bool LockedCache::get(BlockId id, std::byte* dst) {
    // shared_lock:同一时刻可以有 N 个 reader 一起持;有 writer 排队时全部等。
    // 只做 store_.read —— 那步内部是 unordered_map find + memcpy,读了 index_
    // 和 slab_ 的字节,没写任何 metadata。前提是 write 完成后同 id 内容不变
    // (KV cache 假设),这一点从上层保证。
    //
    // 注意这里刻意不调 policy_.on_access(id):hpp 里已经把为什么砍掉讲清楚了。
    // 副作用是 LRU 热度不再随 hit 刷新;bench 里能直接看到这个副作用如何影响
    // 命中率(LockedCache vs Cache 在同 trace 下会略有差)。
    std::shared_lock<std::shared_mutex> lk(mu_);
    return store_.read(id, dst);
}

void LockedCache::put(BlockId id, const std::byte* src) {
    // unique_lock:排他,阻塞所有 reader 和其他 writer。
    // 内部逻辑照抄 Cache::put —— 并发正确性不是靠算法变了,是靠外层这把锁。
    std::unique_lock<std::shared_mutex> lk(mu_);

    if (store_.contains(id)) {
        store_.evict(id);
        store_.write(id, src);
        policy_.on_access(id);
        return;
    }

    if (store_.full()) {
        BlockId victim = policy_.evict();
        store_.evict(victim);
    }

    store_.write(id, src);
    policy_.on_insert(id);
}

size_t LockedCache::size() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return store_.size();
}

} // namespace p4
