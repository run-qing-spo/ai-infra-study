#include "lru_policy.hpp"

#include <cassert>
#include <stdexcept>

namespace p4 {

void LruPolicy::on_insert(BlockId id) {
    assert(index_.find(id) == index_.end() && "重复 on_insert");
    // 头 = MRU,新插入算最热。
    lru_list_.push_front(id);
    index_.emplace(id, lru_list_.begin());
}

void LruPolicy::on_access(BlockId id) {
    auto it = index_.find(id);
    assert(it != index_.end() && "on_access 一个不在账本里的 id");
    // splice 是 O(1) 且 iterator 不失效 —— LRU 用 std::list 的核心理由。
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
    // splice 之后 it->second 仍然指向同一个 list 节点(只是位置换了),不用更新。
}

BlockId LruPolicy::evict() {
    if (lru_list_.empty()) {
        throw std::logic_error("LruPolicy::evict on empty policy");
    }
    BlockId victim = lru_list_.back();
    lru_list_.pop_back();
    index_.erase(victim);
    return victim;
}

void LruPolicy::on_erase(BlockId id) {
    // O(1) 定点删除:跟 on_access 一样靠 iterator hash 表拿到 list 节点。
    // list.erase 只让被删节点本身失效,其他 iterator 保持有效 —— splice/erase
    // 混用 std::list 才敢做这种"iterator 存进 map"的骚操作。
    auto it = index_.find(id);
    assert(it != index_.end() && "on_erase 一个不在账本里的 id");
    lru_list_.erase(it->second);
    index_.erase(it);
}

} // namespace p4
