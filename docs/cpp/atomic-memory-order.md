# C++ 原子操作内存序

::: tip 本项目实战
project2 (SPSC 队列) 里每一条 `head_.store / tail_.load` 都精确选了 `memory_order`,
不是 "保险起见全用 seq_cst". 本文按一条单线展开为什么这么选.
:::

---

## 1. 起点:`std::atomic` 给了什么,没给什么

`std::atomic<T>` 保证**单次** load/store 不撕裂 (没有"读到一半"的状态),
所以多个线程同时读写**同一个** atomic 变量, 不是 data race.

但它**不**自动管以下两件事:

- 同一线程里, 这条 atomic 操作和**周围其他内存访问**的相对顺序
- 另一个线程"什么时候"能看到这次写

编译器、CPU 都可能把 "写 buf_[h] + 写 head_" 重排成 "写 head_ + 写 buf_[h]".
单线程看不出区别 (as-if 规则), 但多线程会爆.

这就是为什么每次 atomic 操作都要带一个 `memory_order` 参数 ——
它不是控制这次写本身, 而是控制**这次写和周围内存访问的相对顺序**.

---

## 2. 重排能干出什么坏事:一个 SPSC 反例

生产者想做的事:

```cpp
buf_[h] = item;          // (1) 写数据
head_.store(h + 1);      // (2) 发布
```

消费者:

```cpp
size_t h = head_.load(); // (3) 看到发布
T val = buf_[t];         // (4) 读数据
```

如果 (1)(2) 重排成 (2)(1):
消费者在 (3) 看到 head 已推进, 在 (4) 读 buf_[t] —— 但生产者还没写完!
读到旧值或半写状态, 数据竞争.

如果 (3)(4) 重排成 (4)(3):
消费者先把 buf_[t] 读出来缓存, 再看 head_ —— 也是读到旧数据.

所以需要两条栅栏:
- 生产者侧: 拦住 (1) 跨到 (2) 之后
- 消费者侧: 拦住 (4) 跨到 (3) 之前

---

## 3. 六档 `memory_order` 各自拦什么

C++ 提供六档 (实际常用四档). **看表前先记两条结论**:

1. `acquire / release / acq_rel` 都只在**同一个** atomic 变量上配对生效, 提供
   "一对线程之间, 围绕这个变量的 happens-before".
2. `seq_cst` 在此之上**额外**提供一条:
   **所有被标记为 seq_cst 的操作** (跨不同 atomic 变量、跨所有线程) 存在一个
   **全局总顺序 S**, 每个线程看到的顺序都和 S 一致. 这是 acq_rel 给不了的,
   也是 seq_cst 唯一不可替代的能力.

   ⚠ 注意: 只有**标记为 seq_cst 的那次操作**才进 S. 同一个变量上的 relaxed 操作、
   另一个变量上的 release/acquire 操作, **都不参与 S 的总序**. 所以
   "把变量声明为 `atomic<T>` " 不等于 "它的所有访问都纳入全局总序" ——
   每次访问站不站进 S, 取决于那一次调用传的 `memory_order`.

| order | 拦什么 | 用在哪 |
|---|---|---|
| `relaxed` | 只保证原子性, 不拦任何重排 | 计数器、自己读自己写过的下标 |
| `consume` | acquire 的弱化版, 编译器实现普遍退化成 acquire, **别用** | — |
| `acquire` | 后续读写不能重排到这条 load **之前** | "我要读取由对方发布的数据" |
| `release` | 前面读写不能重排到这条 store **之后** | "我要发布我刚写完的数据" |
| `acq_rel` | 同时 acquire + release, 用于 RMW (如 `fetch_add`) | 自旋锁释放、引用计数 |
| `seq_cst` | acquire + release + **该次操作纳入全局总序 S** (S 是 seq_cst 独有概念, 其他档无此属性) | 多个独立 atomic 之间需要全局协调 (Dekker 锁、双重检查跨变量可见性) |

**关键直觉**: release 是 "向下游推", acquire 是 "向上游拉", 必须**配对**才形成
happens-before. 单边 release / 单边 acquire 等于没用.
seq_cst 多花的代价 (x86 上的 `mfence`、arm64 上的 `dmb ish`) 买的就是那条
"该操作纳入 S" —— 如果你的算法只在单个变量上做配对, 这条没用, 就别为它付费.

**混搭反例**: `x` 全用 seq_cst, `y` 用 relaxed. x 的所有访问彼此有总序, 但 y 的
访问根本不进 S, 也就和 x 之间没有任何顺序保证: 线程 A 写 x=1(seq_cst)、再写
y=1(relaxed), 线程 B 读 y=1(relaxed) 之后再读 x(seq_cst), 仍然可能读到 x=0.
要让两个变量之间有跨线程的顺序, **两边在两个变量上都得用 seq_cst** (或者退一步用
release-acquire, 但接受第 6 节那种 r1=r2=0 的合法行为).

---

## 4. release-acquire 配对的语义保证

一对配对的 release-acquire (写同一个 atomic 变量), 提供这条保证:

> 如果消费者的 `acquire load` 读到了生产者 `release store` 写入的那个值,
> 那么生产者在 release 之前做的**所有内存写**, 消费者在 acquire 之后**都能看见**.

即:
- 生产者 release 之前的写, 不能漏出到 release 之后
- 消费者 acquire 之后的读, 不能漏到 acquire 之前
- 两条加起来, 形成跨线程的 happens-before

这是 SPSC 队列正确性的全部依据.

---

## 5. 什么时候 `relaxed` 就够

只要满足**任意一个**, relaxed 就安全:

- 这个变量**只有自己写**, 自己读自己写过的: cache coherence 已经保了顺序,
  不需要 acquire/release 再加栅栏. (project2 里生产者读 `head_` 用 relaxed.)
- 不依赖**任何其他内存**的可见性: 比如纯计数器, 调用方只关心"加了多少",
  不会因此去读别的内存.

反过来, 只要这次原子读/写要"和其他内存访问的可见性绑在一起", 就必须 acquire/release.

---

## 6. 什么时候非 `seq_cst` 不可

release-acquire 只保证**一对**配对的 happens-before, 不保证**多个独立位置之间**
有全局一致顺序. 典型反例:

```cpp
// 线程 A
x.store(1, release);
r1 = y.load(acquire);

// 线程 B
y.store(1, release);
r2 = x.load(acquire);
```

release-acquire 下, `r1 == 0 && r2 == 0` 是合法的 (两边各自看到自己写的, 但
互相没看到对方). 如果业务上需要排除这种情况 (例: Dekker 锁), 必须用 seq_cst.

经验法则: 单生产者单消费者、生产者消费者解耦良好, release-acquire 足够;
多个 atomic 变量之间有复杂依赖、需要"所有线程对操作顺序达成一致"才上 seq_cst.

代价: seq_cst 在 x86 上 store 要 `mfence` 或 `lock`, 在 arm64 上是 `dmb ish`,
比 release-acquire 慢一档.

---

## 7. project2 实战回顾

`include/spsc_queue.hpp` 里每条 atomic 操作选哪档, 都能对到上面的规则.

**生产者 push**:

```cpp
const size_t h = head_.load(std::memory_order_relaxed);      // (a)
// ... 判满 ...
cached_tail_ = tail_.load(std::memory_order_acquire);        // (b)
buf_[h] = std::forward<U>(item);                              // (c)
head_.store(next, std::memory_order_release);                 // (d)
```

- (a) `relaxed`: head_ 只有生产者写, 自己读自己 → §5 第一条
- (b) `acquire`: 要读消费者发布的 tail_, 与消费者 (g) 配对 → §4
- (c) 普通写: 受 (d) 的 release 拦住, 不会漏到 store 之后
- (d) `release`: 发布数据, 与消费者 (e) 配对 → §4

**消费者 pop**:

```cpp
const size_t t = tail_.load(std::memory_order_relaxed);      // (f)
// ... 判空 ...
cached_head_ = head_.load(std::memory_order_acquire);        // (e)
out = std::move(buf_[t]);                                     //
tail_.store((t + 1) & mask_, std::memory_order_release);     // (g)
```

- (f) `relaxed`: tail_ 只有消费者写
- (e) `acquire`: 与生产者 (d) 配对, 看到 head 推进 ⇒ 看到 buf_[t] 新值
- (g) `release`: 与生产者 (b) 配对, 通知 "这格我读完了, 可以覆盖"

两对 release-acquire (d↔e、g↔b) 撑起整个队列的正确性, 没有任何一处用 seq_cst.

---

## 8. 速查表

| 场景 | 用什么 |
|---|---|
| 自己读自己写过的 atomic | `relaxed` |
| 纯计数器, 不绑别的内存 | `relaxed` |
| 发布数据给别的线程 (store) | `release` |
| 读别的线程发布的数据 (load) | `acquire` |
| RMW 同时承担发布 + 读取 | `acq_rel` (例: `fetch_add`) |
| 多 atomic 之间需要全局总序 (Dekker 锁等) | `seq_cst` |
| 不确定就先用 `seq_cst` (会慢, 但不会错) | `seq_cst` |

最后一条是兜底, 不是推荐. 写 SPSC / MPMC / 自旋锁这类热路径, 该精挑就精挑;
应用层偶尔用一下的标志位, seq_cst 写起来省脑子, 性能也无所谓.
