#include "ssd_block_store.hpp"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>

namespace p4 {

namespace {

// pread/pwrite 可能被信号打断只完成一部分,标准做法是循环补齐。
// 本地 SSD 上 4KB 一次很难 short,但正确处理算基本功 —— 网络存储、SIGCONT
// 之类下这段循环会救命。返回实际处理的字节数(全部完成时 = n,失败时 -1)。
ssize_t pread_full(int fd, void* buf, size_t n, off_t off) {
    auto* p = static_cast<char*>(buf);
    size_t remain = n;
    off_t cur = off;
    while (remain > 0) {
        ssize_t r = ::pread(fd, p, remain, cur);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return static_cast<ssize_t>(n - remain);  // 意外 EOF
        remain -= static_cast<size_t>(r);
        p      += r;
        cur    += r;
    }
    return static_cast<ssize_t>(n);
}

ssize_t pwrite_full(int fd, const void* buf, size_t n, off_t off) {
    const auto* p = static_cast<const char*>(buf);
    size_t remain = n;
    off_t cur = off;
    while (remain > 0) {
        ssize_t w = ::pwrite(fd, p, remain, cur);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        remain -= static_cast<size_t>(w);
        p      += w;
        cur    += w;
    }
    return static_cast<ssize_t>(n);
}

} // namespace

SsdBlockStore::SsdBlockStore(size_t block_size, size_t capacity, const std::string& path)
    : block_size_(block_size), capacity_(capacity), path_(path) {
    fd_ = ::open(path_.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "open " + path_);
    }

    // ftruncate 把文件撑到最终大小。稀疏文件,首次写到某个 slot 时内核分配 extent。
    if (::ftruncate(fd_, static_cast<off_t>(block_size_ * capacity_)) < 0) {
        int e = errno;
        ::close(fd_);
        fd_ = -1;
        throw std::system_error(e, std::generic_category(), "ftruncate");
    }

    // 同 DRAM slab:倒序压栈,alloc 出来从 slot 0 起,debug 时好看
    free_list_.reserve(capacity_);
    for (size_t i = capacity_; i-- > 0; ) {
        free_list_.push_back(i);
    }
    index_.reserve(capacity_ * 2);
}

SsdBlockStore::~SsdBlockStore() {
    if (fd_ >= 0) ::close(fd_);
    // cache 语义:内容重启即失效,顺手把 backing 文件抹掉不留残留。
    // 崩溃退出走不到这里 —— 学习项目接受偶尔在 /tmp 留孤儿文件。
    if (!path_.empty()) ::unlink(path_.c_str());
}

bool SsdBlockStore::write(BlockId id, const std::byte* src) {
    assert(!full() && "Cache 层应在 full 时先 evict 再 write");
    assert(index_.find(id) == index_.end() && "重复 write 同一个 id");

    // 关键顺序:先 pwrite 成功,再 pop_back + emplace 更新 metadata。
    // 反过来就会在 IO 失败时丢 slot —— 既不在 free_list 也不在 index,泄漏。
    const size_t slot = free_list_.back();   // 只读不 pop
    ssize_t w = pwrite_full(fd_, src, block_size_, slot_offset(slot));
    if (w != static_cast<ssize_t>(block_size_)) return false;

    free_list_.pop_back();
    index_.emplace(id, slot);
    return true;
    // 不 fsync:cache 语义,内容丢了就冷启动。primary 存储才需要 fsync/fdatasync。
}

bool SsdBlockStore::read(BlockId id, std::byte* dst) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    ssize_t r = pread_full(fd_, dst, block_size_, slot_offset(it->second));
    return r == static_cast<ssize_t>(block_size_);
}

bool SsdBlockStore::contains(BlockId id) const {
    return index_.find(id) != index_.end();
}

void SsdBlockStore::evict(BlockId id) {
    auto it = index_.find(id);
    assert(it != index_.end() && "evict 一个不存在的 id");
    free_list_.push_back(it->second);
    index_.erase(it);
    // 磁盘扇区上的旧字节没抹,下次 pwrite 到这个 slot 时覆盖。逻辑不可达就够了。
}

} // namespace p4
