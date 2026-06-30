#pragma once

#include "block_store.hpp"

#include <cstddef>
#include <unordered_map>
#include <vector>

namespace p4 {

// DRAM 数据层:slab 分配器。
//
// 物理布局:一整块连续内存 slab_,按 block_size 切成 capacity 个固定槽位。
// 槽位索引(slot)是 [0, capacity) 的小整数,空闲的丢进 free_list_。
// id → slot 的映射在 index_ 里。
//
// 为什么不用 unordered_map<id, vector<byte>>:
//   - 每个 block 单独 new 一块 → 内存碎片 + 缓存不友好;
//   - slab 的核心好处是 "同构、固定大小、连续",这正是 P5 接 GPU 时
//     做 pinned memory 或大段 cudaMemcpy 的前提。
//   - 这一层就把 layout 定下来,后面 SSD 层、HBM 层都按同 shape 镜像。
class DramBlockStore : public BlockStore {
public:
    DramBlockStore(size_t block_size, size_t capacity);
    ~DramBlockStore() override;

    DramBlockStore(const DramBlockStore&) = delete;
    DramBlockStore& operator=(const DramBlockStore&) = delete;

    size_t block_size() const override { return block_size_; }
    size_t capacity() const override { return capacity_; }
    size_t size() const override { return index_.size(); }

    std::byte* alloc(BlockId id) override;
    std::byte* get(BlockId id) override;
    bool       contains(BlockId id) const override;
    void       evict(BlockId id) override;

private:
    std::byte* slot_ptr(size_t slot) {
        return slab_ + slot * block_size_;
    }

    const size_t block_size_;
    const size_t capacity_;

    std::byte*               slab_ = nullptr;       // 一整块连续内存
    std::vector<size_t>      free_list_;            // 空闲槽位索引栈
    std::unordered_map<BlockId, size_t> index_;     // id → slot
};

} // namespace p4
