#pragma once

#include "block_store.hpp"

namespace p4 {

// 策略层接口:只管 "下一个该踢谁",不碰数据。
//
// 设计要点:策略层维护自己的"账本"(LRU 是双向链表 + map,LFU 是频率堆,etc),
// 账本里只有 BlockId,没有数据指针。这样换策略不影响 BlockStore,
// 也方便单测(可以脱离 BlockStore 直接喂 id 序列验证策略行为)。
class EvictionPolicy {
public:
    virtual ~EvictionPolicy() = default;

    // 通知:有一个新 block 被插入。请记账。
    // 前置:此 id 之前不在账本里(由 Cache 保证 —— miss 路径才会调)。
    virtual void on_insert(BlockId id) = 0;

    // 通知:此 block 刚被命中。请更新热度。
    // 前置:此 id 在账本里。
    virtual void on_access(BlockId id) = 0;

    // 选一个 victim,从账本里删掉它,返回它的 id。
    // 前置:账本非空(由 Cache 保证 —— full 才会调 evict)。
    // 返回后,Cache 会拿这个 id 去 BlockStore::evict(id)。
    virtual BlockId evict() = 0;

    // 从账本里删掉指定的 id(不是挑最冷的那个)。前置:此 id 在账本里。
    // 用途:tiered cache 中把 id 从 L1 policy 迁到 L2 policy 时,L1 侧要显式
    //      erase 掉;以及 L2 覆盖分支里需要"先删旧记账再走 insert 路径"。
    // 跟 evict() 的区别 —— evict 是"策略挑谁走",on_erase 是"外部指定谁走"。
    virtual void on_erase(BlockId id) = 0;

    // 账本内 block 数。debug / 不变量检查用。
    virtual size_t size() const = 0;
};

} // namespace p4
