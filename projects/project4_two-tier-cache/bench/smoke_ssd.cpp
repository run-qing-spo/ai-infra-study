// P4 SSD 后端冒烟测试:证明 SsdBlockStore 在 Cache 眼里跟 DramBlockStore 完全等价。
// 用例结构复刻 smoke.cpp,只换 store 类型 —— 如果 hit/miss/evict 语义一样成立,
// 说明 read/write 拷贝接口成功把介质差异锁在 store 内部。

#include "cache.hpp"
#include "lru_policy.hpp"
#include "ssd_block_store.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace p4;

namespace {

void fill_block(std::vector<std::byte>& buf, BlockId id) {
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<std::byte>((id * 31 + i) & 0xff);
    }
}

bool check_block(const std::byte* p, size_t n, BlockId id) {
    std::vector<std::byte> expect(n);
    fill_block(expect, id);
    return std::memcmp(p, expect.data(), n) == 0;
}

} // namespace

int main() {
    constexpr size_t kBlockSize = 4096;
    constexpr size_t kCapacity  = 4;
    const std::string kBackingPath = "/tmp/p4_ssd_smoke.dat";

    SsdBlockStore store(kBlockSize, kCapacity, kBackingPath);
    LruPolicy     policy;
    Cache         cache(store, policy);

    std::vector<std::byte> buf(kBlockSize);
    std::vector<std::byte> out(kBlockSize);

    // 1) 填满
    for (BlockId id = 0; id < 4; ++id) {
        fill_block(buf, id);
        cache.put(id, buf.data());
    }
    assert(cache.size() == 4);

    // 2) 全部 hit + 数据正确 —— 关键:数据是从磁盘 pread 回来的,能对上就说明
    //    pwrite/pread 路径完整、slot offset 计算正确、稀疏文件按需分配成功。
    for (BlockId id = 0; id < 4; ++id) {
        assert(cache.get(id, out.data()) && "SSD 后端应全部命中");
        assert(check_block(out.data(), kBlockSize, id));
    }

    // 3) 触发 evict(id=0 最旧)
    fill_block(buf, 4);
    cache.put(4, buf.data());
    assert(cache.size() == 4);
    assert(!cache.get(0, out.data()) && "0 应被淘汰");
    assert(cache.get(4, out.data()));
    assert(check_block(out.data(), kBlockSize, 4));

    // 4) 命中 1 顶到头,put 5 → 淘汰 2
    assert(cache.get(1, out.data()));
    assert(check_block(out.data(), kBlockSize, 1));
    fill_block(buf, 5);
    cache.put(5, buf.data());
    assert(!cache.get(2, out.data()) && "2 应被淘汰");
    assert(cache.get(1, out.data()));
    assert(cache.get(3, out.data()));
    assert(cache.get(4, out.data()));
    assert(cache.get(5, out.data()));

    // 5) 覆盖分支 —— 内容不变但 evict+write 两步应等价于旧接口的原地覆盖
    fill_block(buf, 5);
    cache.put(5, buf.data());
    assert(cache.size() == 4);
    assert(cache.get(5, out.data()));
    assert(check_block(out.data(), kBlockSize, 5));

    // 6) SSD 特有点:关键内容应能从磁盘完整往返 —— 已经被上面 get 验证覆盖。
    //    (析构时 store 会 unlink /tmp/p4_ssd_smoke.dat,避免 /tmp 残留)

    std::puts("smoke_ssd: OK");
    return 0;
}
