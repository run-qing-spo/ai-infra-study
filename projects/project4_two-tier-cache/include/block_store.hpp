#pragma once

#include <cstddef>
#include <cstdint>

namespace p4 {

// block 的逻辑 id。上层(Cache)只用 id 寻址,不关心物理槽位。
// 选 uint64_t 是因为 KV cache 在长序列下 block 数量可能很大,uint32_t 不太够。
using BlockId = uint64_t;

// 数据层接口:只管 "block_id ↔ 一块固定大小的内存"。
//
// 不管淘汰策略 —— 淘汰由上层(Cache)挑出 victim id,再调 evict(id) 让数据层
// 把对应槽位回收。数据层不知道 LRU/LFU 是什么。
//
// 为什么 alloc 返回可写裸指针:学习项目 P4 阶段先不做引用计数,调用方约定
// "拿到指针后立即用完,不许跨下一次 put 持有"。
// P4 末尾会重构成 RAII handle + pin/unpin,P5 接 GPU 时复用。
class BlockStore {
public:
    virtual ~BlockStore() = default;

    // 单个 block 的字节数。整个 store 内所有 block 同 size。
    virtual size_t block_size() const = 0;

    // 容量上限(以 block 数量计)。
    virtual size_t capacity() const = 0;

    // 当前占用的 block 数。
    virtual size_t size() const = 0;

    bool full() const { return size() >= capacity(); }

    // 为 id 分配一个槽位,返回可写指针(block_size() 字节)。
    // 前置条件:!full() && !contains(id)
    // 由 Cache 负责在 full 时先 evict 再 alloc。
    virtual std::byte* alloc(BlockId id) = 0;

    // 按 id 取数据指针;不存在返 nullptr。
    // 注意:这里不更新任何"热度"信息(那是策略层的事)。
    virtual std::byte* get(BlockId id) = 0;

    virtual bool contains(BlockId id) const = 0;

    // 把 id 对应的槽位回收(数据视为失效)。
    // 前置条件:contains(id)。
    virtual void evict(BlockId id) = 0;
};

} // namespace p4
