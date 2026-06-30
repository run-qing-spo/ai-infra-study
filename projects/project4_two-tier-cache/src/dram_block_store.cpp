#include "dram_block_store.hpp"

#include <cassert>
#include <cstdlib>
#include <new>

namespace p4 {

DramBlockStore::DramBlockStore(size_t block_size, size_t capacity)
    : block_size_(block_size), capacity_(capacity) {
    // 一次性把整块 slab 申请下来。
    // 用 std::aligned_alloc 拿到 cache line(64B)对齐 —— 后面接 SSD 时
    // O_DIRECT 要求 buffer 至少 512B/4KB 对齐,所以这里直接对齐到 4KB,
    // 顺手把"P3 同步 / io_uring 后端能直接喂 slab 指针"这条路打通。
    constexpr size_t kAlign = 4096;
    const size_t bytes = block_size_ * capacity_;
    // aligned_alloc 要求 bytes 是 alignment 的整数倍 —— block_size 是 4KB 倍数时
    // 自动满足;不是的话向上 round 一下避免 UB。
    const size_t rounded = (bytes + kAlign - 1) / kAlign * kAlign;
    slab_ = static_cast<std::byte*>(std::aligned_alloc(kAlign, rounded));
    if (!slab_) throw std::bad_alloc();

    // free list 倒序压栈,这样 alloc 出来是从 slot 0 开始,debug 时好看。
    free_list_.reserve(capacity_);
    for (size_t i = capacity_; i-- > 0; ) {
        free_list_.push_back(i);
    }
    index_.reserve(capacity_ * 2);  // 装填率 < 0.5,减少 rehash
}

DramBlockStore::~DramBlockStore() {
    std::free(slab_);
}

std::byte* DramBlockStore::alloc(BlockId id) {
    assert(!full() && "Cache 层应在 full 时先 evict 再 alloc");
    assert(index_.find(id) == index_.end() && "重复 alloc 同一个 id");

    const size_t slot = free_list_.back();
    free_list_.pop_back();
    index_.emplace(id, slot);
    return slot_ptr(slot);
}

std::byte* DramBlockStore::get(BlockId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return nullptr;
    return slot_ptr(it->second);
}

bool DramBlockStore::contains(BlockId id) const {
    return index_.find(id) != index_.end();
}

void DramBlockStore::evict(BlockId id) {
    auto it = index_.find(id);
    assert(it != index_.end() && "evict 一个不存在的 id");
    free_list_.push_back(it->second);
    index_.erase(it);
}

} // namespace p4
