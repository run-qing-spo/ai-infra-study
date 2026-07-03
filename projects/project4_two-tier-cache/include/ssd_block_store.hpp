#pragma once

#include "block_store.hpp"

#include <cstddef>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <vector>

namespace p4 {

// SSD 数据层:文件后端的 slab。
//
// 物理布局:一个预分配大小的 backing 文件,按 block_size 切成 capacity 个
// 固定"扇区槽位",slot 索引 [0, capacity) 对应文件偏移 slot * block_size。
// metadata 层(free_list_ + index_)跟 DramBlockStore 完全同构 —— 换 backing
// 不换账本。
//
// 为什么 SSD 也要预分配整个文件:
//   - 文件 offset 直接由 slot 算出,不用维护"下一个可写位置"这种可变状态;
//   - 逻辑上跟 slab 布局镜像,后面做 DRAM↔SSD spill 时两侧 slot 语义对齐;
//   - 生产上应该走 fallocate(Linux) / F_PREALLOCATE(macOS) 预留连续 extent
//     减少碎片,学习项目为了跨平台先用 ftruncate,造出的是稀疏文件,首次写
//     每个槽时按需分配 extent —— 功能等价,只是不保证连续性。
//
// 关于 IO 后端:
//   起步用 pread/pwrite(POSIX),page cache 兜底,同步阻塞。写一次几十微秒到
//   毫秒级,evict 触发的 spill 会阻塞在这里 —— 这是 P4 骨架接受的代价。P5
//   换 io_uring 后能异步提交 + 批量收割,那时候接口不变,把 pread/pwrite
//   换成 sqe 提交即可。O_DIRECT / F_NOCACHE 想加也是 P5 的事,前提是 buffer /
//   offset / size 全 4KB 对齐(我们已经具备)。
//
// 持久性:cache tier 语义,不 fsync。重启后 SSD 内容视为丢失(析构 unlink
// 掉),冷启动重跑就行。primary storage 场景才需要 fsync/fdatasync。
class SsdBlockStore : public BlockStore {
public:
    // path:backing 文件路径。构造时 O_CREAT|O_RDWR|O_TRUNC 打开并撑到目标大小。
    //       析构时 close + unlink(cache 语义:内容不留)。
    SsdBlockStore(size_t block_size, size_t capacity, const std::string& path);
    ~SsdBlockStore() override;

    SsdBlockStore(const SsdBlockStore&) = delete;
    SsdBlockStore& operator=(const SsdBlockStore&) = delete;

    size_t block_size() const override { return block_size_; }
    size_t capacity() const override { return capacity_; }
    size_t size() const override { return index_.size(); }

    bool write(BlockId id, const std::byte* src) override;
    bool read(BlockId id, std::byte* dst) override;
    bool contains(BlockId id) const override;
    void evict(BlockId id) override;

private:
    off_t slot_offset(size_t slot) const {
        return static_cast<off_t>(slot) * static_cast<off_t>(block_size_);
    }

    const size_t block_size_;
    const size_t capacity_;
    std::string  path_;
    int          fd_ = -1;

    std::vector<size_t>                 free_list_;   // 空闲 slot 索引栈
    std::unordered_map<BlockId, size_t> index_;       // id → slot
};

} // namespace p4
