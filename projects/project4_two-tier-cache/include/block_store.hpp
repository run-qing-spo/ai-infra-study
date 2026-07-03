#pragma once

#include <cstddef>
#include <cstdint>

namespace p4 {

// block 的逻辑 id。上层(Cache)只用 id 寻址,不关心物理槽位。
// 选 uint64_t 是因为 KV cache 在长序列下 block 数量可能很大,uint32_t 不太够。
using BlockId = uint64_t;

// 数据层接口:只管 "block_id ↔ 一块固定大小的字节序列"。
//
// 不管淘汰策略 —— 淘汰由上层(Cache)挑出 victim id,再调 evict(id) 让数据层
// 把对应槽位回收。数据层不知道 LRU/LFU 是什么。
//
// ─── 接口契约:read/write 拷贝语义,不返回裸指针 ────────────────────────
// P4 初期 alloc/get 返回 std::byte* 是 DRAM 场景的取巧:数据层 slab 的内存
// 可以直接解引用。上 SSD 后立刻卡住 —— 磁盘上的字节没有"能安全解引用"的
// 指针,你手上只有 (fd, offset) 元数据,得 syscall 把字节搬进 DRAM buffer
// 才能用。GPU HBM 同理(cudaMemcpy)。
//
// 所以接口统一收敛成:
//   - write(id, src): 调用方提供 src buffer,store 借读 block_size() 字节
//     做一次物理拷贝(DRAM: memcpy;SSD: pwrite;将来 GPU: cudaMemcpyH2D)。
//     src 的所有权始终在调用方,store 不持有它。
//   - read(id, dst):  调用方提供 dst buffer,store 把 block_size() 字节拷/
//     读进去(DRAM: memcpy;SSD: pread;将来 GPU: cudaMemcpyD2H)。
//
// 代价:DRAM 版本失去了"直接返指针零拷贝"的取巧,多一次 memcpy。
// 收益:所有介质接口对称,上层 Cache 一份代码适配任何后端;后面做 tiered
//       cache 时,DRAM↔SSD 之间的搬移就是 "read from src tier + write to
//       dst tier",不用分开两套。
//
// 为啥不用 shared_ptr 共享 src:跨介质拷贝是物理必然,SSD 扇区上的字节和
// DRAM heap 上的字节根本不是同一块内存,shared_ptr 帮不到这里。
// (同介质纯 DRAM cache 内部倒是可以用 shared_ptr 零拷贝,那是另一个故事。)
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

    // 把 src 的 block_size() 字节写入 id 对应的槽位。
    // 前置条件:!contains(id) && !full()  —— 覆盖/满的处理在 Cache 层做。
    // 返回:成功 true,后端 IO 失败 false(DRAM 实现永远 true;SSD 可能失败)。
    virtual bool write(BlockId id, const std::byte* src) = 0;

    // 把 id 对应的 block_size() 字节读进 dst。
    // 前置条件:contains(id)。
    // 返回:成功 true,不存在或 IO 失败 false。
    // 注意:这里不更新任何"热度"信息(那是策略层的事)。
    virtual bool read(BlockId id, std::byte* dst) = 0;

    virtual bool contains(BlockId id) const = 0;

    // 把 id 对应的槽位回收(数据视为失效)。
    // 前置条件:contains(id)。
    // 只动 metadata,物理字节会被下一次 write 到这个 slot 时覆盖。
    virtual void evict(BlockId id) = 0;
};

} // namespace p4
