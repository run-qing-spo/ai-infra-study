# Snapshots — 优化每一步的代码留档

这个目录保留**起点 baseline** 的完整代码, 加上中间几轮的**关键差异 diff**.
完整的最终代码在 `include/spsc_queue.hpp`, 优化思路全过程见 `../optimization_journey.md`.

`01_baseline.hpp` 是一份完整可读的 .hpp; 之后每轮的改动只贴关键 diff 片段
(push/pop 函数体 + 私有成员), 这样能聚焦看 "每一步加了什么 / 减了什么",
避免重复贴 90% 一样的样板代码.

注意: snapshot 文件用 `namespace spsc`, 跟 `include/spsc_queue.hpp` 同名同类,
**不要在同一个翻译单元里同时 include**, 只用来阅读对照.

---

## v01_baseline — 起点

文件: [`01_baseline.hpp`](01_baseline.hpp)

特征:
- push/pop 的 release-acquire 配对已就位
- 没有 false sharing 防护 (`head_` 和 `tail_` 紧挨着, 同 cache line)
- wrap 用 `% cap_` (cap 任意)
- 没有本地缓存 (`tail_.load(acquire)` 每次 push 都跨核走)

bench 数据 (M ops/sec): cap=4 → 14.4 / cap=64 → 18.1 / cap=1024 → 19.5 / cap=65536 → 20.6

---

## v02 — 加 `alignas(kCacheLine)` 拆 cache line

**diff 摘要** (相对 v01):

新增常量:
```cpp
static constexpr size_t kCacheLine = 128; // Apple Silicon arm64; x86 是 64
```

私有成员从:
```cpp
std::atomic<size_t> head_{0};
std::atomic<size_t> tail_{0};
```

变成:
```cpp
alignas(kCacheLine) std::atomic<size_t> head_{0};
alignas(kCacheLine) std::atomic<size_t> tail_{0};
```

push/pop 函数体**完全不变**. 改动只在内存布局.

bench 数据: cap=64 → 25.0 (+38%), cap=1024 → 25.0 (+28%), cap=4 → 16.9 (+17%)

---

## v03 — 在 v02 基础上加 cached head/tail

**diff 摘要** (相对 v02):

私有成员加两个本地缓存, **关键: 各自紧贴写它的那一侧**:

```cpp
// 生产者侧 cache line
alignas(kCacheLine) std::atomic<size_t> head_{0};
size_t cached_tail_{0};   // 紧贴 head_, 只有生产者读写

// 消费者侧 cache line
alignas(kCacheLine) std::atomic<size_t> tail_{0};
size_t cached_head_{0};   // 紧贴 tail_, 只有消费者读写
```

push 函数体加 fast path:
```cpp
bool push(const T& item) {
    const size_t h = head_.load(std::memory_order_relaxed);
    const size_t next = (h + 1) % cap_;

    // Fast path: 缓存说不满, 跳过跨核 acquire load
    if (next == cached_tail_) {
        cached_tail_ = tail_.load(std::memory_order_acquire);
        if (next == cached_tail_) return false;
    }
    buf_[h] = item;
    head_.store(next, std::memory_order_release);
    return true;
}
```

pop 对称 (用 `cached_head_` 判空).

**踩坑提醒**: 中间曾尝试过 "在 v01 baseline 上直接加 cached" (跳过 v02),
全线慢 30-72% —— 因为 `cached_*` 落进同一条 cache line, 写它们让对方 cache
失效. 必须先 v02 拆 cache line, cached 才能正确放在 "写它的那一侧" 的线上.

bench 数据: cap=64 → 27.0 (+8% vs v02), cap=1024 → 31.2 (+25% vs v02), cap=4 持平

---

## v04 — 在 v03 基础上加位运算 wrap

**diff 摘要** (相对 v03):

构造函数加约束:
```cpp
explicit SpscQueue(size_t capacity)
    : buf_(capacity), cap_(capacity), mask_(capacity - 1) {
    assert(capacity >= 2 && (capacity & (capacity - 1)) == 0);
}
```

私有成员加 `size_t mask_;`.

热路径上所有 `% cap_` 替换成 `& mask_`:
- push: `(h + 1) % cap_` → `(h + 1) & mask_`
- pop:  `(t + 1) % cap_` → `(t + 1) & mask_`
- size_approx: `(h + cap_ - t) % cap_` → `(h - t) & mask_`

(size_approx 利用 size_t 下负数 wrap 的算术对 2 的幂模运算成立的性质.)

需要 `#include <cassert>`. **代价**: API 受限, 容量必须是 2 的幂.

bench 数据: cap=4 → 21.3 (+26% vs v03), cap=64 → 36.5 (+35% vs v03),
cap=1024 → 35.9 (+15% vs v03)

---

## v05 — 在 v04 基础上加 move 语义 (即 `include/spsc_queue.hpp`)

完整代码在 [`../../include/spsc_queue.hpp`](../../include/spsc_queue.hpp).

**diff 摘要** (相对 v04):

需要 `#include <utility>` (std::forward, std::move).

push 改成两个公共重载 + 一个 private 模板实现:
```cpp
public:
    bool push(const T& item) { return push_impl(item); }
    bool push(T&& item)      { return push_impl(std::move(item)); }
private:
    template <typename U>
    bool push_impl(U&& item) {
        // 原 push 函数体 + buf_[h] = std::forward<U>(item);
        ...
    }
```

pop 的写法只改一行:
```cpp
out = std::move(buf_[t]);  // 原: out = buf_[t];
```

trivial T (int 等) 上 move 自动退化为 copy, bench 数字基本不变.
大对象 T (`std::string(256)`) 上 copy 路径 vs move 路径加速 ~1.3-1.35x.
move-only 类型 (`std::unique_ptr`) 之前编译都过不去, 现在可用.

---

## 累计加速 (M ops/sec, int64)

| cap   | v01 baseline | v02 alignas | v03 + cached | v04 + bitmask | v05 (final) |
|-------|--------------|-------------|--------------|---------------|-------------|
| 4     | 14.4         | 16.9        | 17.2         | 21.3          | 21.3        |
| 64    | 18.1         | 25.0        | 27.0         | 36.5          | 36.5        |
| 1024  | 19.5         | 25.0        | 31.2         | 35.9          | 35.9        |
| 65536 | 20.6         | 不稳定      | 不稳定       | 21.8          | 21.8        |

cap=65536 的数据靠不住, 见 `../optimization_journey.md` 末尾的分布实验.

大对象路径 (v05 专属, `std::string(256)`): copy 2.7 M/s, move 3.6 M/s, **1.3-1.35x**.
