# 同步原语深入：从 sync_backend 的 mu/cv 一路追到 futex

本文从一个具体问题出发：`SyncBackend` 里为什么用 `std::mutex + std::condition_variable`
而不是 atomic / lock-free queue？顺着这个口子把 mutex、cv、futex 的内部机制都讲透。
是给未来的自己当 reference,不是教程。

---

## 起点：为什么 sync_backend 用 mu+cv 而不是无锁队列

看 `sync_backend.hpp`：
```cpp
std::mutex                sq_mu_;
std::condition_variable   sq_cv_;
std::deque<IoRequest>     sq_;
```

一个直觉的疑问是：上一个项目刚写过 SPSC 无锁队列, 这里为什么"退化"成了 mu+cv？
分三层回答：

**1. deque 本身不是线程安全的**——多 worker 并发 push/pop 必须串行化, 要么换无锁队列要么上锁。

**2. worker 需要"睡着等活"而不是"轮询等活"**。无锁队列只解决了"互斥访问"那一层, 没解决
"队列空了 worker 该干嘛"。三个选项：

- 自旋检查 → 空闲烧 CPU, 跟"用线程池撑高并发同步 IO"的目标冲突
- `sleep_for(几 ms)` → 引入固定唤醒延迟
- 真正的"睡到被叫醒" → 这就是 cv 干的事

**3. 这里不是性能热点**。worker 拿到请求后做的是 `pread/pwrite` (微秒到毫秒级),
mutex 的几十纳秒在这个尺度上是噪音。SyncBackend 是 io_uring 的对照基线, **应该用最朴素
直接的写法**, 不然 benchmark 比的就不是 IO 模型而是各自队列实现的精巧度。

> 上个项目里队列**本身就是 benchmark 对象**, 纳秒级开销决定吞吐, 所以值得上无锁。
> 这里队列是 IO 路径入口, 真正的开销在 syscall, 用 mu+cv 是合适的选择。

---

## mutex 的本质

mutex = mutual exclusion。两个核心动作：`lock()` / `unlock()`。规则一条：
**同一时刻最多一个线程持有它**。

"持有"的物理含义：线程 A `lock()` 成功后, B 也来 `lock()` 会被**内核摘下 runqueue**, state
从 `TASK_RUNNING` 改成 `TASK_INTERRUPTIBLE`, 不占 CPU 时间片。A `unlock()` 时内核把 B
挂回 runqueue, 调度器后续选中 B。

代价模型：
- **无竞争**：用户态原子 CAS 就完事, 几纳秒, 不进内核
- **有竞争**：一次进出内核, 几百纳秒到 1 微秒

C++ 上用 RAII 包装：
```cpp
{
    std::lock_guard<std::mutex> lk(sq_mu_);
    sq_.push_back(req);
}   // 出作用域自动 unlock, 异常也安全
```

mutex 解决的：**独占访问一段数据/代码**。
mutex 没解决的：**"等到某个条件成立再继续"**。这是 cv 的活。

---

## condition_variable 的本质——它不是"状态变量"

最容易踩的认知陷阱：把 cv 当成一个带 true/false 的标志。**它没有状态**。

cv 就是"**候车厅 + 广播喇叭**"：
- 候车厅里站着一队睡着的线程 (初始为空)
- `wait()` = 进候车厅蹲下睡
- `notify_one()` = 喇叭叫一个号
- `notify_all()` = 喇叭叫所有人

cv 不知道也不在乎你在等什么"条件", 条件依附在外部数据上 (predicate lambda 里读的那个),
cv 只负责让线程睡和醒。

> 问 "cv 默认初始化为 false 吗" 这个问题本身不适用——cv 没有 true/false。
> 默认构造的语义就是"候车厅空的, 喇叭关的"。

cv 不保护任何数据。保护数据的是 mu。两者是搭档关系：
- **mu**: 给"独占访问"
- **cv**: 给"睡到被叫醒"

---

## 为什么 mu 和 cv 必须配对——丢失唤醒

`cv.wait(lk)` 强制要求传一把 lock 住的 unique_lock, 不是设计师风格, 是协议必需。
看下面这个反例就懂了。

无 mu 版本：
```cpp
// 等待方
if (cq_.size() < min) {       // 检查
    cq_cv_.wait();             // 睡
}

// 通知方
cq_.push_back(...);            // 改数据
cq_cv_.notify_one();           // 叫醒
```

致命时序：

| 时间 | 等待方 | 通知方 |
|---|---|---|
| t1 | 检查 `cq_.size() < min` → 真 | |
| t2 | | push_back, cq_ 满足条件了 |
| t3 | | notify_one——但 cv 等待队列**还是空的** |
| t4 | 进入 wait(), 挂到等待队列 | |
| t5 | 永远睡下去 | |

t3 那一下 notify 落空——**cv 不缓存信号**, 没人在队列里就直接丢弃。这就是经典的
**lost wakeup**。

正确版本 (用 mu 圈住 "检查 + 准备睡")：
```cpp
// 等待方
unique_lock lk(cq_mu_);
cq_cv_.wait(lk, [&]{ return cq_.size() >= min; });

// 通知方
{ lock_guard lk(cq_mu_); cq_.push_back(...); }
cq_cv_.notify_one();
```

关键约束：**通知方改 cq_ 必须先拿 cq_mu_**。所以等待方持有 cq_mu_ 期间, 通知方动不了
cq_, 不会出现"看完条件 → 条件改了 → 我才睡"的窗口。

---

## wait() 内部到底做了什么——三步 + futex compare-then-sleep

错误的心智模型："`wait()` 持有锁一直到睡着, 然后释放锁"——这会死锁 (通知方拿不到 mu)。

正确的顺序是 **"先报到, 再放锁, 再睡"**：

1. 持有 mu 时, 把自己**登记到 cv 内部状态**里 (递增 generation 计数器 / 加入等待队列)
2. 释放 mu
3. 调系统调用真正睡下去

第 1 步是关键：登记发生在持锁期间, **通知方此时动不了**, 整个世界对 cv 的状态是一致的。

但第 2 步和第 3 步之间还有缝隙——锁已经释放, 但还没真睡进内核。此时通知方可能 push +
notify, notify 想从等待队列捞人但等待方还没"真"睡进去。这一刻 notify 会丢吗？

不会, 靠 **futex 的 compare-then-sleep**。简化模型：

```
// 第 1 步在持锁时:
int generation = cv->counter;   // 快照
lock.unlock();                  // 第 2 步
futex(&cv->counter, WAIT, generation);   // 第 3 步: 原子地检查 + 睡
lock.lock();                    // 醒来后重拿
```

`futex(addr, WAIT, expected)` 的语义是 "**原子地**检查 `*addr == expected`, 是就睡,
不是就立刻返回"。如果在第 2 步和第 3 步之间通知方调了 notify, **notify 内部第一件事
就是 `counter++`**, 然后才去叫醒。等待方第 3 步进内核时 futex 一看 `counter ≠ expected`,
立刻返回, 根本不睡。

> 所以"持有 mu 直到睡着"是错的, 实际是"持有 mu 直到登记完, 剩下交给 futex 的原子语义"。

---

## notify_one vs notify_all

`notify_one()`: 从等待队列**只挑一个**唤醒, 其它继续睡。
`notify_all()`: 把等待队列**全部**唤醒, 它们抢锁、按顺序处理。

**生产者-消费者场景默认用 notify_one**：一个请求只能被一个 worker 干, 多叫的都是浪费。

**notify_all 适用于"条件一旦成立对所有等待者都成立"的场景**。SyncBackend 析构里：
```cpp
sq_cv_.notify_all();   // stop_ = true 对所有 worker 都成立, 必须全叫醒
```
如果只 notify_one, 剩下 N-1 个 worker 永远睡, join 永远不返回, 死锁。

**反模式 (thundering herd)**：一个物品 + notify_all → 所有 worker 醒, 一个抢到、其他
回去再睡。付了 N 次"唤醒+切换+抢锁+挂回"的开销。

> SyncBackend 的 submit 里其实也用了 notify_all (推 n 个时叫醒 n 个), 是规模上的妥协
> ("几十个 worker 无所谓"), 大规模时应该改成 notify_one × n。

---

## predicate 循环——醒来不能相信"条件成立"

正确写法永远是 `wait(lk, predicate)` 带 lambda 的重载, 它内部是个循环：

```cpp
while (!pred()) {
    // 释放锁 + 睡 + 重拿锁
}
```

为什么必须循环？两种"醒来发现条件不成立"的情况：

1. **虚假唤醒 (spurious wakeup)**——内核可能在没人 notify 时也叫醒一个等待者。POSIX
   规范明确允许的实现自由度, 不是 bug。
2. **被插队**——worker X 被叫醒, 正要抢 mu。另一个刚做完事的 worker Y 先拿到了 mu,
   把 X 该处理的请求取走。X 拿到 mu 时条件已经不成立了。

带 predicate 的 `wait` 重载帮你把循环写好。**永远不要相信"被叫醒 = 条件成立"**, cv
只保证"如果条件真成立时 notify 了, 等待者最终会醒来", 不保证"醒来时条件一定成立"。

---

## futex 的设计哲学

cv 的实现绕不开 futex, 它本身就值得专门讲。

### 它解决什么

2002 年前, pthread mutex 的每次 lock/unlock 都是系统调用。**但绝大多数时候锁是没人抢的**——
这种情况根本不需要内核, 一条 `lock cmpxchg` 几纳秒就够。**只有真要把线程睡过去才需要
内核帮忙**。futex 就是为了这个场景生的。

### 核心拆分

传统内核锁是"状态 + 等待队列"都在内核。futex 反过来：
- **状态**放在用户态：就是一个普通的 `int` (futex word), 用户程序自己用 CAS 改
- **等待队列**留在内核：但**用这个 int 的内存地址做 key**

内核侧维护一张大哈希表 `地址 → 在这里睡着的线程列表`, **它不在乎这个 int 是什么含义**,
只知道"有人想睡在地址 X 上"。

### API 只有两个

```
futex(addr, FUTEX_WAIT, expected)   // 如果 *addr 还是 expected, 就睡到这个地址上
futex(addr, FUTEX_WAKE, n)          // 叫醒睡在这个地址上的 n 个人
```

`expected` 参数就是 compare-then-sleep——防止"我看完决定睡, 准备进内核, 中间状态被改"
这种丢失唤醒。

### mutex 怎么用 futex 实现

futex word 当锁状态：0=free, 1=持有但无人等, 2=持有且有人等。

加锁：
```
if CAS(word, 0 → 1): return        # 没人持有, 用户态完事, 不进内核
# 有人持有
CAS(word, 1 → 2)                    # 升级为"有人等"
futex(&word, WAIT, expected=2)      # 进内核睡
```

解锁：
```
if word == 1: CAS(word, 1 → 0); return    # 没人在等, 用户态完事
word = 0
futex(&word, WAKE, n=1)                    # 进内核叫一个
```

**热路径完全在用户态, 只有真有竞争时才付 1 微秒的内核往返**。"Fast Userspace muTEX"
名字的由来。

### 最美的部分——内核不知道这个 int 是什么意思

futex 不为 mutex 专门做, 它只提供"睡在某个地址上 / 叫醒某个地址上的人"两个原语。
这个 int 的语义完全是用户态决定的：

| 用法 | 语义 |
|---|---|
| 锁状态 | 实现 mutex |
| generation 计数器 | 实现 condition_variable |
| 资源计数 | 实现 semaphore |
| 32 位标志 | 实现 `std::atomic<T>::wait()` (C++20) |
| read/write 状态 | 实现 rwlock |

所有这些上层原语**共用一个内核机制**。RISC 的思路：**给一个最小、最通用的原语, 让上层
自己组合复杂语义**。

### 串起来

futex 的趣味在于它解决了一个看似矛盾的需求：**同步原语的"快"(不进内核) 和"会睡"(必须
进内核) 天然冲突**。解法是把这两件事拆开——状态变化不涉及睡的时候, 用户态自己搞定；
涉及睡或醒的时候, 再用一个最小化内核入口解决。而 compare-then-sleep 那个小聪明, 正好
把"状态在用户态、睡眠在内核态"这个拆分带来的竞争窗口给堵上了。

> 上个项目的 lock-free queue 是"完全不进内核"的极端, futex 是"95% 不进内核, 剩下 5%
> 优雅地进"的中庸——大多数生产代码 (std::mutex、cv、atomic::wait) 选的是后者。

---

## 心智模型总结

| 抽象层 | 干什么 | 不干什么 |
|---|---|---|
| mutex | 独占访问数据 | 等待条件 |
| cv | 让线程睡到被叫醒 | 保护数据、保存状态 |
| mu + cv 配对 | "看条件 → 决定睡 → 真睡" 这一段防丢失唤醒 | (无, 这才是完整抽象) |
| futex | "睡在地址上 / 叫醒地址上的人" 的最小内核原语 | 知道你在等什么 |

**记住一句话**：cv 没有状态, mu 保护数据 + 保护协议, futex 是底下那个聪明的原子睡眠原语。
