#include "cache.hpp"

namespace p4 {

bool Cache::get(BlockId id, std::byte* dst) {
    if (!store_.read(id, dst)) return false;
    // BlockStore::read 不更新热度(那是数据面的边界),这里显式通知策略。
    policy_.on_access(id);
    return true;
}

void Cache::put(BlockId id, const std::byte* src) {
    // 已存在 → 覆盖分支。
    // 新接口下 store.write 假设 !contains(id),所以覆盖要走"evict 旧的 → write 新的"
    // 两步。metadata 上是一次 map erase + 一次 map insert,可忽略。策略侧只需
    // on_access(不算新插入,这个 id 在账本里从没离开过)。
    if (store_.contains(id)) {
        store_.evict(id);
        store_.write(id, src);
        policy_.on_access(id);
        return;
    }

    // 不存在 → 满了先腾位置。
    if (store_.full()) {
        BlockId victim = policy_.evict();
        store_.evict(victim);
    }

    store_.write(id, src);
    policy_.on_insert(id);
}

} // namespace p4
