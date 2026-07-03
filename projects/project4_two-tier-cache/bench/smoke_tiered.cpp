// P4 tiered 冒烟测试:验证 DRAM(L1) + SSD(L2) 两级组合的 Exclusive spill 语义。
//
// L1 cap=2, L2 cap=2, 总 cap=4。故意设成"必然触发所有分支":
//   - L1 未满正常插入
//   - L1 满 → spill 到 L2(被 spill 的 id 应从 L1 消失,能在 L2 读回原字节)
//   - L2 满 → spill 到 L2 时先真扔 L2 底
//   - L2 hit 不 promote(get 之后 id 应仍在 L2 不在 L1)
//   - 覆盖分支 A(id 在 L1)/ B(id 在 L2)

#include "dram_block_store.hpp"
#include "lru_policy.hpp"
#include "ssd_block_store.hpp"
#include "tiered_cache.hpp"

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

// 便捷断言:tier 分布 + 数据从对应 tier 读回都正确
void expect_hit(TieredCache& tc, BlockId id, std::vector<std::byte>& out, size_t bs) {
    assert(tc.get(id, out.data()));
    assert(check_block(out.data(), bs, id));
}

void expect_miss(TieredCache& tc, BlockId id, std::vector<std::byte>& out) {
    assert(!tc.get(id, out.data()));
}

} // namespace

int main() {
    constexpr size_t kBlockSize = 4096;
    constexpr size_t kL1Cap = 2;
    constexpr size_t kL2Cap = 2;

    DramBlockStore l1(kBlockSize, kL1Cap);
    LruPolicy      l1_policy;
    SsdBlockStore  l2(kBlockSize, kL2Cap, "/tmp/p4_tiered_smoke.dat");
    LruPolicy      l2_policy;
    TieredCache    tc(l1, l1_policy, l2, l2_policy);

    std::vector<std::byte> buf(kBlockSize);
    std::vector<std::byte> out(kBlockSize);

    // ── 1) 填 L1 ────────────────────────────────────────────
    // put(0), put(1) 后:L1={1,0}(MRU→LRU), L2={}
    for (BlockId id = 0; id < 2; ++id) {
        fill_block(buf, id);
        tc.put(id, buf.data());
    }
    assert(tc.l1_size() == 2 && tc.l2_size() == 0);

    // ── 2) 第一次 spill:put(2) 挤出 L1 最冷(id=0)到 L2 ─────
    fill_block(buf, 2);
    tc.put(2, buf.data());
    assert(tc.l1_size() == 2 && tc.l2_size() == 1);
    // 0 现在应该在 L2,读回来的字节还是当初 put 进去的
    assert(l1.contains(2) && l1.contains(1) && !l1.contains(0));
    assert(l2.contains(0));
    expect_hit(tc, 0, out, kBlockSize);   // ← spill 后数据往返完整
    // 注意:上一句 get(0) 触发 L2 policy on_access,不会 promote 到 L1
    assert(!l1.contains(0) && l2.contains(0));

    // ── 3) 继续填,验证 L2 也满 ─────────────────────────────
    // 上一步 get(0) 让 L2 内 0 变 MRU;此时 L2={0}, L2 lru: 0
    // put(3):L1 满 → spill l1_policy 底(此时 L1 lru: 2>1;get 不改 L1
    //        因为 get(0) 是 L2 hit),spill 1 到 L2. L2={1,0}
    fill_block(buf, 3);
    tc.put(3, buf.data());
    assert(tc.l1_size() == 2 && tc.l2_size() == 2);
    assert(l1.contains(3) && l1.contains(2));
    assert(l2.contains(0) && l2.contains(1));

    // ── 4) L2 也满,put(4) 触发 L2 真扔 ──────────────────────
    // L1 spill 底(2)到 L2;L2 满 → 挑 L2 LRU 真扔。
    // L2 lru 当前:1(MRU,刚 on_insert), 0(LRU,更早 on_insert 之后被 on_access
    //  过一次拉到 MRU,再被 1 的 on_insert 抢过 MRU)。
    //  → L2 lru: 1(MRU), 0(LRU) 之后 1 被 on_insert 变 MRU. wait 顺序反了.
    //  实际:L2 policy 事件流是 on_insert(0)→on_access(0)→on_insert(1),
    //  所以 L2 lru:1(MRU), 0(LRU)。
    //  put(4) spill 2 → L2 满 → 真扔 0. L2={2,1}。
    fill_block(buf, 4);
    tc.put(4, buf.data());
    assert(tc.l1_size() == 2 && tc.l2_size() == 2);
    assert(l1.contains(4) && l1.contains(3));
    assert(l2.contains(1) && l2.contains(2));
    expect_miss(tc, 0, out);   // 0 真离开系统
    // spill 到 L2 的 2 的字节应该完整
    expect_hit(tc, 2, out, kBlockSize);

    // ── 5) L2 hit 不 promote ────────────────────────────────
    // 上一句 get(2) 是 L2 hit;确认 2 依然在 L2 而不是 L1
    assert(l2.contains(2) && !l1.contains(2));

    // ── 6) L1 hit 数据正确 ──────────────────────────────────
    expect_hit(tc, 4, out, kBlockSize);
    expect_hit(tc, 3, out, kBlockSize);
    assert(l1.contains(4) && l1.contains(3));

    // ── 7) 覆盖分支 A:put 一个已经在 L1 的 id ──────────────
    // 不改变 tier 分布,不 spill,只刷 L1 policy 热度。
    fill_block(buf, 4);
    tc.put(4, buf.data());
    assert(tc.l1_size() == 2 && tc.l2_size() == 2);
    assert(l1.contains(4));
    expect_hit(tc, 4, out, kBlockSize);

    // ── 8) 覆盖分支 B:put 一个已经在 L2 的 id ──────────────
    // 期望语义:L2 撤走 → 走 insert 路径 → 写到 L1(触发一次 spill 因为 L1 满)。
    // 当前状态:L1={4,3}(4 刚 on_access), L2={2,1}(2 之前 on_access 是 MRU)
    // put(1):
    //   1. 1 不在 L1,在 L2 → l2.evict(1), l2_policy.on_erase(1). L2={2}
    //   2. Insert 路径. L1 满 → l1_policy.evict() 挑 LRU.
    //      L1 policy 事件:on_insert(2→L2 spill 走)→ on_insert(3) → on_insert(2 覆盖?..)
    //      实际 L1 policy 里现在有 4 和 3, MRU=4(因为刚 on_access 4), LRU=3.
    //      → l1_policy.evict()=3. spill 3 到 L2(L2 未满,size=1). L2={3,2}
    //   3. l1.write(1) + on_insert(1). L1={1,4}
    fill_block(buf, 1);
    tc.put(1, buf.data());
    assert(tc.l1_size() == 2 && tc.l2_size() == 2);
    assert(l1.contains(1) && l1.contains(4));
    assert(l2.contains(2) && l2.contains(3));
    assert(!l1.contains(3) && !l2.contains(1));
    // 数据完整性:所有还在的 id 都能读回原字节
    expect_hit(tc, 1, out, kBlockSize);
    expect_hit(tc, 4, out, kBlockSize);
    expect_hit(tc, 3, out, kBlockSize);   // 刚 spill 到 L2 的 3
    expect_hit(tc, 2, out, kBlockSize);
    expect_miss(tc, 0, out);
    expect_miss(tc, 5, out);   // 从未 put 过

    std::puts("smoke_tiered: OK");
    return 0;
}
