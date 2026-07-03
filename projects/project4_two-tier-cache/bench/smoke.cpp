// P4 骨架冒烟测试:验证 put/get/hit/miss/evict 闭环正确。
// 不是 benchmark,只是"接口拼起来能跑、LRU 顺序对"。
// 真正的 trace 回放 + 命中率对比留到后续。

#include "cache.hpp"
#include "dram_block_store.hpp"
#include "lru_policy.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace p4;

namespace {

// 用 block_id 自身当种子,把 block 填成可识别的内容。
// 这样命中后能验证"取回来的数据 == 当初写进去的数据"。
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
    constexpr size_t kBlockSize = 4096;   // 4KB,顺手满足后面 O_DIRECT 对齐
    constexpr size_t kCapacity  = 4;      // 故意小,容易触发 evict

    DramBlockStore store(kBlockSize, kCapacity);
    LruPolicy      policy;
    Cache          cache(store, policy);

    std::vector<std::byte> buf(kBlockSize);    // 写入用
    std::vector<std::byte> out(kBlockSize);    // 读出用(新接口:调用方持 dst)

    // 1) 填到满:0,1,2,3 全部 put,LRU 顺序应为 (头) 3,2,1,0 (尾)
    for (BlockId id = 0; id < 4; ++id) {
        fill_block(buf, id);
        cache.put(id, buf.data());
    }
    assert(cache.size() == 4);

    // 2) 全部 hit + 数据正确
    for (BlockId id = 0; id < 4; ++id) {
        assert(cache.get(id, out.data()) && "应该全部命中");
        assert(check_block(out.data(), kBlockSize, id));
    }
    // 经过这轮 get,LRU 顺序变成 (头) 3,2,1,0 (尾) → 顺序 access 后是 (头) 3,2,1,0
    //   ↑ 等等,access 顺序是 0,1,2,3,所以每次 access 都把它移到头部,
    //     最终 LRU 顺序是 (头) 3,2,1,0 (尾)。0 在尾 = 最旧。

    // 3) put 一个新 id(4),容量满 → 应淘汰 id=0
    fill_block(buf, 4);
    cache.put(4, buf.data());
    assert(cache.size() == 4);
    assert(!cache.get(0, out.data()) && "0 应被淘汰");
    assert(cache.get(4, out.data()) && "4 应在");

    // 4) 命中 1(把它顶到头),再 put 5 → 应淘汰 2(此时 2 在尾)
    //    上一步状态:经过 cache.get(0)=miss / cache.get(4)=hit,
    //               LRU 是 (头) 4,3,2,1 (尾)。注:get(0) miss 不动账本。
    //    然后这一步 cache.get(1) hit → (头) 1,4,3,2 (尾)。
    //    put(5):2 是尾,被淘汰。
    assert(cache.get(1, out.data()));
    assert(check_block(out.data(), kBlockSize, 1));

    fill_block(buf, 5);
    cache.put(5, buf.data());
    assert(!cache.get(2, out.data()) && "2 应被淘汰");
    assert(cache.get(1, out.data()));
    assert(cache.get(3, out.data()));
    assert(cache.get(4, out.data()));
    assert(cache.get(5, out.data()));

    // 5) 重复 put 同一 id 应走"覆盖"分支,不淘汰别人
    fill_block(buf, 5);   // 内容不变,但 put 行为要正确
    cache.put(5, buf.data());
    assert(cache.size() == 4);
    assert(cache.get(5, out.data()));
    assert(check_block(out.data(), kBlockSize, 5));

    std::puts("smoke: OK");
    return 0;
}
