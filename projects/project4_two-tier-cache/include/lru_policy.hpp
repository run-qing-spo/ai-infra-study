#pragma once

#include "eviction_policy.hpp"

#include <list>
#include <unordered_map>

namespace p4 {

// 经典 LRU 账本:
//   - lru_list_ 头是 most-recently-used,尾是 least-recently-used;
//   - index_ 把 id 映射到 list 里的 iterator,这样 on_access 是 O(1) splice。
//
// 注意:这层只存 id,不存数据。和 DramBlockStore 完全解耦。
class LruPolicy : public EvictionPolicy {
public:
    void   on_insert(BlockId id) override;
    void   on_access(BlockId id) override;
    BlockId evict() override;
    void   on_erase(BlockId id) override;
    size_t size() const override { return index_.size(); }

private:
    std::list<BlockId>                                   lru_list_;
    std::unordered_map<BlockId, std::list<BlockId>::iterator> index_;
};

} // namespace p4
