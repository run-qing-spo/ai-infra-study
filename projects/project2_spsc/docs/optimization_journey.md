# SPSC Queue 优化迭代记录

本文档复盘 `SpscQueue<T>` 的优化过程, 重点是**每一轮的核心机制 + 数据怎么动**,
不讲"最终代码怎么写". 最终实现读 `include/spsc_queue.hpp` 即可.

测试平台: macOS, Apple Silicon (arm64), `-O2`, gcc 13.
Bench: `make bench` (`test/bench.cpp`). 每个数据点 = 5 轮取最小耗时.

---

## 起点 (Baseline)

最小可用实现: 两个原子下标 `head_` / `tail_`, release-acquire 配对, 用 `% cap_`
做 wrap, 没有 false sharing 防护, 没有本地缓存.

| cap | ns/op | M ops/sec |
|---|---|---|
| 4 | 69.4 | 14.4 |
| 64 | 55.2 | 18.1 |
| 1024 | 51.4 | 19.5 |
| 65536 | 48.6 | 20.6 |

从极小 cap 到很大 cap 吞吐只涨 1.43x —— **瓶颈不是容量, 是两条 atomic 路径自身的
cache 弹跳代价**. 后面三轮就是为了压扁这个开销.

---

## 第 1 轮: false sharing — `alignas(128)`

**优化点: 消除"读自己变量被对方写误伤"的 cache miss**.

push 里 `h = head_.load(relaxed)` 这一步本应是本地 hit (P 自己刚写过 head_).
但 `head_` 和 `tail_` 同一条 cache line 时, C 每写 `tail_` 一次就把整条 line 在 P
失效, P 下次读自己的 `head_` 就 miss —— 跟 `head_` 本身的值变化无关. C 读自己 `tail_`
对称受 P 写 `head_` 误伤.

```cpp
alignas(kCacheLine) std::atomic<size_t> head_{0};
alignas(kCacheLine) std::atomic<size_t> tail_{0};
```

`kCacheLine = 128`: Apple Silicon L1 cache line 是 128 字节 (x86 是 64). 取 128
多浪费 padding, 但保证不会漏掉 false sharing.

| cap | baseline | alignas | 变化 |
|---|---|---|---|
| 4 | 69.4 | ~59 | -15% |
| 64 | 55.2 | ~40 | **-28%** |
| 1024 | 51.4 | ~40 | **-22%** |
| 65536 | 48.6 | ~50 | 抖动 ±20%, 不可信 (见末尾分布实验) |

cap=4 加速少, 是因为高争用下 acquire load 必然 miss, 本来就要付 cross-core 代价,
false sharing 的相对收益变小.

---

## 第 2 轮: 本地快照 `cached_tail_` / `cached_head_`

**优化点: 让大多数 push 不必跨核读真的 `tail_`, 改读 P 自己 line 上的本地快照**.

push 只判断"满没满", 这个判断不需要 `tail_` 最新值, 只需要保守快照: `cached_tail_`
落后于真值是 OK 的, 因为 `tail_` 只会向前推进, 真值只会"更不满". 只在 cached 看起来
满了才回去 acquire load 真 `tail_`. 对称地 `cached_head_` 给消费者用.

### 失败的中间步: cached 紧跟 tail_

第一次直接在 baseline 之上加 cached, 没拆 line, 结果**全线变慢 30-72%**:

| cap | baseline | cached-only | 变化 |
|---|---|---|---|
| 4 | 69.4 | 90.6 | +30% |
| 64 | 55.2 | 67.2 | +22% |
| 1024 | 51.4 | 66.4 | +29% |
| 65536 | 48.6 | 83.5 | +72% |

原因: `cached_tail_` 紧跟 `tail_`, 同一条 line. P 每次 refresh **写 `cached_tail_`**,
把 C 的 `tail_` 整条失效 —— **人为给 C 制造了一个原本不存在的 false sharing**.
多了一次写, 该省的 miss 一次没省, 净亏.

cap=65536 慢得最离谱: 该 cap fast path 命中率高, `cached_tail_` 被写的频率最接近
每次 push, ping-pong 最频繁.

**这一步的真正价值**: 让 "**alignas 是 cached 的地基**" 从直觉变成数据强证.
没有把 `head_` / `tail_` 拆到独立 cache line, cached_*_ 放哪都会撞到对方关心的变量,
false sharing 跑不掉.

### 正确版: cached 放在自己一侧的 line

```cpp
// 生产者侧 line: head_ + cached_tail_ 都只有 P 写
alignas(kCacheLine) std::atomic<size_t> head_{0};
size_t cached_tail_{0};

// 消费者侧 line: tail_ + cached_head_ 都只有 C 写
alignas(kCacheLine) std::atomic<size_t> tail_{0};
size_t cached_head_{0};
```

P 写 `cached_tail_` 不触及 C 任何 line, false sharing 消除.

| cap | alignas-only | + cached | 额外加速 |
|---|---|---|---|
| 4 | ~59 | ~58 | 持平 |
| 64 | ~40 | ~37 | -8% |
| 1024 | ~40 | ~32 | **-19%** |
| 65536 | ~50 | ~62 | 偏慢 (memory-bound 噪声) |

- cap=4 持平: 高争用下 fast path 几乎从不命中, cached 退化成死代码.
- cap=1024 是甜点: fast path 命中率高 + cache line 没 ping-pong + buf_ 仍在 L1.

---

## 第 3 轮: 位运算 wrap

**假设**: `(h+1) % cap_` 里的 `cap_` 是运行时 `size_t`, 不是编译期常量,
所以 `%` 编译成真的 `div` / `udiv` 指令 (arm64 上需要约 7 个周期).
如果限定 cap 是 2 的幂, 可以换成 `(h+1) & (cap_-1)`, 单指令.

**改动**:

```cpp
explicit SpscQueue(size_t capacity)
    : buf_(capacity), cap_(capacity), mask_(capacity - 1) {
    assert(capacity >= 2 && (capacity & (capacity - 1)) == 0
           && "capacity must be a power of two and >= 2");
}
// push: (h + 1) & mask_
// pop:  (t + 1) & mask_
// size_approx: (h - t) & mask_   // size_t 下负数 wrap 的算术对 2 的幂模运算也成立
```

**代价**: 容量参数受限. 所有现有测试 / bench 用的 cap 都是 2 的幂, 没破坏.

**数据** (vs alignas + cached):

| cap | + cached | + 位运算 wrap | 额外加速 |
|---|---|---|---|
| 4 | ~58 | ~47 | **-19%** |
| 64 | ~37 | ~29 | **-22%** |
| 1024 | ~32 | ~27 | **-16%** |
| 65536 | ~62 | ~47 | (回到 baseline 水平) |

**学到的**:

- 每个 cap 都吃到加速 —— 因为热路径上每次 push/pop 都要走一次 wrap, 这是确定能消的开销.
- cap=65536 之前持续偏慢, 加位运算后回到 baseline 水平 (~47 ns/op),
  说明 cap=65536 反复反常的 5 ns 里, 其中相当一部分是 `div` 指令 + memory subsystem 噪声叠加.
- 位运算 wrap 是"无脑加速": 没有 cache 层面副作用, 没有架构敏感性, 唯一代价是 API 受限.

---

## 第 4 轮: move 语义

到此 trivial T (int 类) 的吞吐基本到顶 (中等 cap 接近 baseline 的 2x).
T 是大对象时, 瓶颈不再是队列, 而是**每次 push/pop 都走两次 deep copy**.

**改动**: 加 `push(T&&)` 重载, pop 用 `std::move`. 实现走 private 模板共享:

```cpp
public:
    bool push(const T& item) { return push_impl(item); }
    bool push(T&& item)      { return push_impl(std::move(item)); }
private:
    template <typename U> bool push_impl(U&& item) {
        ...
        buf_[h] = std::forward<U>(item);  // 左值 → copy, 右值 → move
        ...
    }
```

```cpp
out = std::move(buf_[t]);  // pop 把槽位资源转给 out
```

**附带收益**: move-only 类型 (`std::unique_ptr<T>`) 现在能直接放进队列了 —— 之前
`buf_[h] = item` 走拷贝赋值, 而 unique_ptr 拷贝赋值是 `= delete`, 根本编不过.

**测试**: 新增 `test/test_spsc_move.cpp`, 4 条:
- `MoveOnlyTypeCompiles`: 用 `unique_ptr<int>` 推 / 拉, 验证编译通过 + 行为正确
- `RvaluePushEmptiesSource`: 右值 push 之后源 string 应被 move 掏空
- `LvaluePushPreservesSource`: 左值 push 之后源 string 必须保留 (防 T&& 重载吃左值)
- `PopMovesOutOfSlot`: pop 多轮, 验证 moved-from 槽位能被下一轮 push 覆盖

**数据** (大对象 bench, `std::string(256)`, N=1M):

| cap | copy ns/op | move ns/op | speedup |
|---|---|---|---|
| 64 | 375 | 278 | **1.35x** |
| 1024 | 370 | 284 | **1.31x** |

吞吐换算: copy 路径 ~2.7 M msgs/sec, move 路径 ~3.6 M msgs/sec.

**学到的**:

- 每次 push→pop 周期省 ~95 ns, 正对应 push 一次 256 B memcpy + pop 一次 256 B memcpy
  + 减少的 malloc/free 压力.
- string 路径整体比 int64 慢 10 倍 (28-32 → 280-375 ns/op). 这**不是队列变慢**,
  是 T 本身的成本上来了 (堆分配 + memcpy).
- cap=64 vs 1024 几乎一样 (差距 < 2%) —— 大对象路径下, 队列内部的所有优化都退到背景里,
  瓶颈完全是 T 本身. 这反过来说明前 3 轮在 trivial 类型上的优化非常充分.

---

## 总览

int64 路径累计加速 (M ops/sec):

| cap | baseline | + alignas | + cached | + 位运算 | 总加速 |
|---|---|---|---|---|---|
| 4 | 14.4 | 16.9 | 17.2 | **21.3** | **1.48x** |
| 64 | 18.1 | 25.0 | 27.0 | **36.5** | **2.02x** |
| 1024 | 19.5 | 25.0 | 31.2 | **35.9** | **1.84x** |
| 65536 | 20.6 | ~20 | ~16 | 21.8 | ~持平 (数据不可信, 见末尾) |

大对象路径 (`std::string`):

| cap | copy | move | 加速 |
|---|---|---|---|
| 64 | 2.67 M/s | 3.59 M/s | 1.35x |
| 1024 | 2.70 M/s | 3.53 M/s | 1.31x |

---

## 元教训 (跨轮的)

1. **不要凭直觉优化, 让 bench 决定**.
   "本地缓存对方下标"听起来很合理, 数据告诉你它单独做反而变慢 72%.

2. **每个优化要先单独验证再叠加**.
   如果第 2 轮一开始就是 "alignas + cached 一起上", 数据会很好看,
   但分不清究竟是哪一项贡献多少 —— 也就分不清"先做 cache line 拆分是地基"这个判断.
   一次一项 + bench 对账, 才能 isolate 出每一项的真实价值.

3. **失败的迭代不要直接埋掉**.
   第一次单做 cached 失败, 那次的反思 (cache line 是地基) 是第 2 轮成功的前提.
   把失败原因写进代码注释和文档, 下一个看代码的人 (包括将来的自己) 不会重复踩坑.

4. **测量本身有噪声, 要识别能信和不能信的数据**.
   cap=65536 那一档反复反常, 不是优化变差, 而是 buf_ 4 MB 已经超 L1,
   测的是 memory subsystem. 同样的优化在 L1 内的 cap 上数据干净, 才该认真读.

5. **trivial 类型上测不出 move 的收益**.
   int 上 move 自动退化为 copy, bench 数字一样, 但 move 重载本身没问题.
   要看 move 的价值, 必须专门用大对象 (string / vector / proto) 作为 T 跑 bench.

6. **优化的收益分布是不均匀的**.
   trivial T 上, alignas + cached + 位运算 是大头.
   大 T 上, move 是大头, 队列内部优化几乎看不出差别 (因为相对开销很小).
   决定"该不该做某项优化"得先想清楚目标用例的 T 是什么.

---

## 补充实验: cap=65536 的分布探查

前面几轮里 cap=65536 一档持续反复反常 (有时快有时慢, ±20% 抖动),
我一直含糊地归因为 "memory-bound 噪声主导, 不可信". 这条说法本身没数据支持.
最终态收口后, 单独对这一档跑 30 轮 (N=10M each, 最终态代码), 拿到完整分布:

```
cap=65536, N=10M, 30 rounds, ns/op:
  min    = 44.61
  p25    = 59.09
  median = 63.37
  p75    = 68.43
  max    = 79.68
  mean   = 63.95
  stddev = 7.34  (11.5% of mean)
  range  = 35.07 (78.6% of min)
```

读出来的事:

- **不是双峰也不是长尾**, 就是**单峰但宽分布** (IQR/median ≈ 14%).
  所以反常不是某种偶发"卡顿模式", 而是这个 cap 在这台机器上稳定地**宽分布**.
- **`min=44.61` 是 round 0 一个孤立低值** (round 1-29 全部 ≥ 54), 大概率是
  cache 第一次热起来时的边界状态, 不代表稳态.
- **median 63 ns/op 才是真实代表性数字**. 之前我们一直用 "best of 5" 取 47-50,
  抓到的其实是分布尾部的低值 (大约 P10-P25), **系统性低估了真实开销**.
- **这一档的优化前后差距大部分被宽分布吞掉**: 11.5% 的标准差 ≈ 7 ns,
  比前几轮在这一档报的 "加速 / 减速" 差距 (3-10 ns) 还大. 也就是说,
  cap=65536 这一档前面表里那些"快了 / 慢了"基本都在噪声里, 不要当真.

这条实验回答了几件事:
1. 之前对 cap=65536 的"memory-bound 噪声"说法**方向对**, 但不严格 ——
   更准的说法是 "**单峰宽分布, 小 best-of-N 会偏估 P10-P25**".
2. 让本节表里所有 cap=65536 列的数字打上"不可信"标签, 解释为什么用 median
   重测会发现优化前后差距很小.
3. 暗示一个 bench 工具改进方向 (后续如果再做): 对宽分布的档位用 median + IQR
   报告, 而不是 best-of-N. best-of-N 对 trivial 类型 + 窄分布 (cap=64/1024) OK,
   对宽分布会骗自己.

实验脚本: `scratchpad/cap65k_dist.cpp` (会话临时文件, 不进 repo). 复现就用同样
的 N=10M, 30 rounds, 跑最终态代码, 期望看到类似分布.
