#pragma once

#include "block_store.hpp"
#include "eviction_policy.hpp"

namespace p4 {

// 粘合层:把 BlockStore(数据)和 EvictionPolicy(账本)拼起来对外。
//
// 不持有 store / policy 所有权,用引用传入。这样测试时可以轻松换实现,
// 也避免 Cache 模板化(后面真要做线程安全时,把锁加在这里就行)。
class Cache {
public:
    Cache(BlockStore& store, EvictionPolicy& policy)
        : store_(store), policy_(policy) {}

    // hit: 返回数据指针(只读视角),并通知策略 on_access。
    // miss: 返 nullptr。
    // 注意生命周期:返回的指针在下一次 put 之前有效;put 可能触发 evict
    // 把别的 block 踢掉 —— 但本次返回的 id 自身不会被立刻踢
    // (LRU 刚 on_access 过它是最热的;其它策略需自己保证语义)。
    const std::byte* get(BlockId id);

    // 插入 / 覆盖:把 src 的 block_size() 字节拷进 store。
    // 已存在 → 覆盖数据 + on_access(算一次"写命中")。
    //   ↑ 这里有个微妙点:KV cache 场景里同 id 的内容不应该变(block 内容是
    //     由 token 序列决定的,id 一样内容就一样)。所以"已存在"应该几乎不发生;
    //     真发生了直接覆盖是安全的。先按这个简单语义来,后面如果上 RadixTree
    //     再重新审视。
    // 不存在 → 满了先 evict 一个,再 alloc + 拷贝 + on_insert。
    void put(BlockId id, const std::byte* src);

    size_t size() const { return store_.size(); }
    size_t capacity() const { return store_.capacity(); }

private:
    BlockStore&      store_;
    EvictionPolicy&  policy_;
};

} // namespace p4
