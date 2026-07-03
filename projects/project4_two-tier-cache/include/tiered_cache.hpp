#pragma once

#include "block_store.hpp"
#include "eviction_policy.hpp"

#include <cstddef>
#include <vector>

namespace p4 {

// 两级 tier 组合:L1(热) + L2(冷),Exclusive 语义。
//
// 设计原则:
//   - L1 和 L2 内容不重叠(Exclusive)。get 时哪一层有直接返,**不 promote**
//     L2 hit 的 id 回 L1(按前面对齐的方案)。为什么不 promote:一次 L2 hit
//     就把它拉回 L1,意味着"访问一次就重新热"—— 但真正的热是"多次访问"。
//     简单版做单向下沉,不做上浮。要做上浮加个访问计数阈值就行。
//   - put 只写 L1。L1 满 → 挑 L1 最冷的 spill 到 L2;L2 也满 → 挑 L2 最冷的真扔。
//   - 每层各自一个 EvictionPolicy 实例(类型可以相同,追踪的 id 集合不同)。
//     这就是当时对齐时说的"没必要用不同类型的 policy"—— 用同一类,不是同一实例。
//
// 为啥不继承 BlockStore:BlockStore::read/write 是"纯字节接口,不管热度";
// 而 tier 内部 read/write 一定要顺手更新自己的 policy 账本(否则 tier 内的
// LRU 顺序就废了)。所以 TieredCache 跟 Cache 类平级,各自对上层暴露 get/put。
// 想嵌套三级 tier(加个 object storage L3),把 L2 换成"另一个 TieredCache
// 装饰过的 store 层"就能扩,但那需要 TieredCache 也实现 BlockStore,先不做。
class TieredCache {
public:
    // 前置:l1.block_size() == l2.block_size()。
    TieredCache(BlockStore& l1_store, EvictionPolicy& l1_policy,
                BlockStore& l2_store, EvictionPolicy& l2_policy);

    // hit(不论在 L1 还是 L2):拷进 dst + 更新对应层 policy + 返 true。
    // miss:返 false,不动 dst。
    bool get(BlockId id, std::byte* dst);

    // put 只写 L1。已在 L1 → 就地覆盖;已在 L2 → 从 L2 撤走走 insert 路径
    // (等价一次"覆盖式上浮")。insert 时 L1 满则 spill 到 L2,L2 满则真扔 L2 底。
    void put(BlockId id, const std::byte* src);

    size_t size() const { return l1_store_.size() + l2_store_.size(); }
    size_t capacity() const { return l1_store_.capacity() + l2_store_.capacity(); }
    size_t l1_size() const { return l1_store_.size(); }
    size_t l2_size() const { return l2_store_.size(); }

private:
    BlockStore&     l1_store_;
    EvictionPolicy& l1_policy_;
    BlockStore&     l2_store_;
    EvictionPolicy& l2_policy_;

    // spill 中转 buffer:L1 → L2 搬移时必须先 read 出字节再 write 到另一层。
    // 一块就够,因为整个 put 是单线程串行流程;上多线程时这里得改成 per-thread 或 pool。
    std::vector<std::byte> spill_buf_;
};

} // namespace p4
