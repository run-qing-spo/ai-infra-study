#include "tiered_cache.hpp"

#include <cassert>

namespace p4 {

TieredCache::TieredCache(BlockStore& l1_store, EvictionPolicy& l1_policy,
                         BlockStore& l2_store, EvictionPolicy& l2_policy)
    : l1_store_(l1_store), l1_policy_(l1_policy),
      l2_store_(l2_store), l2_policy_(l2_policy),
      spill_buf_(l1_store.block_size()) {
    assert(l1_store_.block_size() == l2_store_.block_size() &&
           "L1 和 L2 block_size 必须一致 —— tier 间搬移是定长拷贝");
}

bool TieredCache::get(BlockId id, std::byte* dst) {
    if (l1_store_.contains(id)) {
        l1_store_.read(id, dst);
        l1_policy_.on_access(id);
        return true;
    }
    if (l2_store_.contains(id)) {
        l2_store_.read(id, dst);
        l2_policy_.on_access(id);
        // 关键:不 promote 回 L1。热度只在 L2 policy 里刷,位置不动。
        return true;
    }
    return false;
}

void TieredCache::put(BlockId id, const std::byte* src) {
    // 覆盖分支 A:id 在 L1。走"L1 内覆盖",不触发 spill。
    if (l1_store_.contains(id)) {
        l1_store_.evict(id);
        l1_store_.write(id, src);
        l1_policy_.on_access(id);
        return;
    }

    // 覆盖分支 B:id 在 L2。KV cache 场景理论上一样 id 内容也一样,
    // 但契约上得处理 —— 从 L2 撤走,落到 insert 路径(相当于"覆盖式上浮")。
    if (l2_store_.contains(id)) {
        l2_store_.evict(id);
        l2_policy_.on_erase(id);
        // fall through
    }

    // Insert 路径:id 不在系统里,写到 L1。L1 满则 spill。
    if (l1_store_.full()) {
        // 1) 挑 L1 最冷 —— l1_policy 内部 pop 并从账本删掉,返回 id
        const BlockId spill_victim = l1_policy_.evict();
        // 2) 从 L1 store 读出 victim 字节到中转 buf(此时 L1 store 里还在)
        l1_store_.read(spill_victim, spill_buf_.data());
        // 3) L1 store 侧也删掉
        l1_store_.evict(spill_victim);
        // 4) 塞进 L2 —— L2 也满就先真扔一个 L2 底
        if (l2_store_.full()) {
            const BlockId real_victim = l2_policy_.evict();
            l2_store_.evict(real_victim);
            // real_victim 这次是真离开系统,不再进任何 policy 账本。
        }
        l2_store_.write(spill_victim, spill_buf_.data());
        l2_policy_.on_insert(spill_victim);
    }

    // 至此 L1 一定有空位
    l1_store_.write(id, src);
    l1_policy_.on_insert(id);
}

} // namespace p4
