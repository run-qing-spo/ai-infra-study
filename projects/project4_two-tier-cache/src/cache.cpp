#include "cache.hpp"

#include <cstring>

namespace p4 {

const std::byte* Cache::get(BlockId id) {
    std::byte* p = store_.get(id);
    if (!p) return nullptr;
    policy_.on_access(id);
    return p;
}

void Cache::put(BlockId id, const std::byte* src) {
    // 已存在 → 覆盖 + on_access。
    // (BlockStore::get 不更新热度,所以这里要显式调 policy_。)
    if (std::byte* existing = store_.get(id)) {
        std::memcpy(existing, src, store_.block_size());
        policy_.on_access(id);
        return;
    }

    // 不存在 → 满了先腾位置。
    if (store_.full()) {
        BlockId victim = policy_.evict();
        store_.evict(victim);
    }

    std::byte* dst = store_.alloc(id);
    std::memcpy(dst, src, store_.block_size());
    policy_.on_insert(id);
}

} // namespace p4
