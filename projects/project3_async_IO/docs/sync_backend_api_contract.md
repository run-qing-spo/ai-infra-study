# SyncBackend 的 API 契约与所有权设计

`include/sync_backend.hpp` / `src/sync_backend.cpp` 的对外接口非常薄, 但每一个参数和返回值
都藏着异步 IO 模型的核心约定。本文记录这些约定, 以及它们背后的设计权衡。

读懂本文后, 应该能回答：
- 为什么 `submit` 的返回值在这个 backend 里看起来"没用"？
- 为什么 buf 的生命周期是调用方的责任, 而不是 backend 用 shared_ptr 接管？
- 为什么析构函数不防御并发使用？

---

## 接口速览

```cpp
class SyncBackend : public IoBackend {
public:
    explicit SyncBackend(size_t num_workers);
    ~SyncBackend() override;

    size_t submit(const IoRequest* reqs, size_t n) override;
    size_t reap(IoCompletion* out, size_t max_n, size_t min_complete) override;
    size_t in_flight() const override;
};
```

四个对外动作：构造、submit、reap、in_flight、析构。语义在下面拆解。

---

## 参数和返回值的"潜规则"

### submit 的返回值在 SyncBackend 里是死值

```cpp
size_t submit(const IoRequest* reqs, size_t n) override;   // 永远 return n;
```

`sq_` 是 `std::deque`, 无上限, 怎么 push 都行。所以这里返回值没信息——永远等于传入的 n。

那为什么签名要返回一个 size_t？**为 io_uring backend 留的契约**。io_uring 的 SQ 是固定
长度的 ring buffer, 可能只能塞下前 k 个, 剩下让调用方下一轮 retry。两个 backend 共用一个
interface 才方便公平对比。

**所以正确的使用姿势是**：

```cpp
size_t submitted = 0;
while (submitted < N) {
    submitted += be.submit(reqs + submitted, N - submitted);
    // SyncBackend 一次就够; io_uring 可能要循环多次
}
```

不要因为"sync 一次就够"就写成 `be.submit(reqs, N);`——换 backend 时会踩坑。

### reap 的两个数字是不同的旋钮

```cpp
size_t reap(IoCompletion* out, size_t max_n, size_t min_complete);
```

- `max_n` = "我的输出数组能装多少" (上限)
- `min_complete` = "至少要等到这么多完成才返回" (下限)

**`min_complete` 同时是阻塞开关**：
- `== 0` → 非阻塞, 当前 cq_ 有多少捞多少, 没有就 return 0
- `> 0` → 走 cv.wait, 睡到至少这么多完成事件攒齐才返回

这两个模式是异步 IO 的灵魂——你可以"submit 一批, 干别的, 时不时 poll"也可以"submit 一批,
没事干就睡着等"。

**边角**：如果 `min_complete > max_n`, 行为是"睡到 cq_ 有 min_complete 个, 然后返回 max_n
个, 剩下下次再取"。逻辑上不致命但用法别扭。学习项目里值得加一行 `assert(min_complete <= max_n)`。

### in_flight 的语义比名字暗示的窄

```cpp
// worker_loop 里:
{
    std::lock_guard<std::mutex> lk(cq_mu_);
    cq_.push_back(IoCompletion{req.user_data, r});
    in_flight_.fetch_sub(1, std::memory_order_relaxed);   // ← 这里减
}
```

`reap` 完全不碰 in_flight。所以它的**精确含义**是：

> "已 submit、但 worker 还没把 pread/pwrite 跑完的请求数"

完成了但躺在 cq_ 里没被 reap 走的**不算 in_flight**。所以 `in_flight() == 0` **不等于
"没事干了"**——cq_ 里可能还堆着一堆等收割的完成事件。

判断"全部收割完"的正确做法：

```cpp
// 错误
while (be.in_flight() != 0) { /* spin */ }

// 正确
size_t total = 0;
while (total < expected) {
    total += be.reap(out + total, expected - total, /*min_complete=*/1);
}
```

---

## 调用顺序——典型生命周期

```cpp
// 1. 构造:N 个 worker 立刻起来, 全部阻塞在 sq_cv_.wait
SyncBackend be(num_workers);

// 2. 准备一批请求 (注意 buf 必须在 reap 之前一直有效——见下节)
std::vector<char> buf(N * BLOCK);
std::vector<IoRequest> reqs(N);
for (...) reqs[i] = { fd, buf.data() + i*BLOCK, BLOCK, offset, READ, user_data };

// 3. submit:推完即返回, worker 立刻开干
size_t submitted = 0;
while (submitted < N) submitted += be.submit(reqs.data() + submitted, N - submitted);

// 4. reap:阻塞收齐
std::vector<IoCompletion> done(N);
size_t reaped = 0;
while (reaped < N) {
    reaped += be.reap(done.data() + reaped, N - reaped, N - reaped);
}

// 5. 析构:此刻必须没有其他线程在 submit/reap
}   // ~SyncBackend(): stop_ + notify_all + join workers
```

**几个关键的"顺序"约定**：
- submit 和 reap **可以错开、可以交织、可以在不同线程**。两者锁不同的 mu, 无冲突。
- **真正享受批量性**的姿势：submit 一大批 → 干别的或直接 reap 一大批。**不要 submit
  一个就 reap 一个**, 那退化成同步 IO, N 个 worker 等于浪费。
- **析构必须 quiescent**——见后面"析构契约"小节。
- 构造和析构之间, **N 个 worker 一直在跑** (哪怕完全没 submit, 也睡着等着), 占内存和
  thread ID。所以 benchmark 里 reuse 一个 backend, 不要随便构造销毁。

---

## Buffer 所有权——异步 IO 的核心约束

`IoRequest` 包含一个**裸指针** `buf`：
```cpp
struct IoRequest {
    int      fd;
    void*    buf;       // ← 指针
    size_t   size;
    uint64_t offset;
    Op       op;
    uint64_t user_data;
};
```

`submit()` 里 `sq_.push_back(reqs[i])` 复制的是这个结构体的**字节**——**包括那个指针字段**。
副本里的 `.buf` 和你传进来那份**指向同一块内存**。

所以：

```cpp
std::vector<char> buf(4096);
IoRequest req = { fd, buf.data(), 4096, 0, READ, 0 };
backend.submit(&req, 1);
// req 这个变量可以销毁,无所谓——它的副本在 sq_ 里
buf.clear();   // ← 灾难。sq_ 里 IoRequest 的 .buf 现在悬空
// worker_loop 醒来:pread(fd, dangling_ptr, 4096, 0) → UAF
```

**类比**：你给了一张写着"123 号"的明信片, 我复印了一份。复印件还是同一个地址。但那栋
房子是你家的——你把房子拆了, 我手里的复印件还指着那个地址, 但地址那边已经啥都没有了。

**所以约束是**：

> buffer 必须从 `submit` 那一刻活到对应 completion 被 `reap` 回来那一刻。

代码里**没有任何强制**, 也没注释提醒。这是异步 IO 的本质约束。

### 没 reap 算泄漏吗？

要分情况：

| 情况 | 后果 |
|---|---|
| submit 了, 没 reap, buf 还活, 最后正常析构 backend | 没事。sq_ 被 worker 排空, cq_ 随对象析构, 调用方自己的 RAII 管着 buf |
| submit 了, 没 reap, **buf 被释放了** | **UAF**——worker 可能正在 pread/pwrite |
| 长期 submit 不 reap | cq_ deque 越涨越大, 内存压力。逻辑层面应用模式有问题 |

**严格来说 backend 不会泄漏 buf**——它根本就不 own buf, 谈不上"漏"。所谓"内存麻烦"
都是调用方使用模式的责任。

### 为什么不用 shared_ptr 接管 buf

```cpp
struct IoRequest {
    std::shared_ptr<char[]> buf;   // 假设这么改
};
```

代价：
1. 每次 submit/reap 都**原子 ref-count + 复制 shared_ptr** (每请求两次原子操作)
2. **强制堆分配** (control block)
3. **benchmark 数字失真**——io_uring 的 SQE 是 64 字节裸结构体, 根本不可能用 shared_ptr,
   两个 backend 的对比就不公平了
4. 偏离 Linux 异步 IO 接口的真实语义 (io_uring/aio/IOCP 全部把 buffer 责任甩给调用方,
   因为内核 DMA 没法替你管理用户态内存)

**结论**：保留裸指针 + 文档化约束。代价由调用方付, 但 backend 保持轻和真实。

---

## 析构契约——为什么不防御并发

析构函数现在长这样：
```cpp
~SyncBackend() {
    { std::lock_guard<std::mutex> lk(sq_mu_); stop_.store(true, std::memory_order_release); }
    sq_cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
}
```

**它不防御**：如果析构发生时还有外部线程在 submit/reap, 行为是 UAF, 没有任何代码兜底。

### 为什么不防御？四个层面：

**Q1：拿什么检测"有人正在用我"？** 要么每个 public 方法进出加原子计数器, 要么整个对象用
shared_ptr 管。

**Q2：原子计数器要钱**——每次 submit/reap 多一次原子操作, 高频路径上不可接受。

**Q3：检测到了又能怎么办？**
- 抛异常 → 析构函数抛 = `std::terminate`, 比 UAF 还狠
- spin 等 → 死锁风险 (那个线程可能正等着 backend 给它结果)
- abort → 不优雅

**Q4：真要"任何时候析构都安全", 得改成 shared_ptr 语义**——这不是"加点防御", 是换所有
权模型。

C++ 主流的态度：**类承诺"submit/reap/in_flight 互相之间线程安全", 但不承诺"它们和析构
之间线程安全"**。析构契约写在文档里, 由调用方负责确保 quiescent。

> std::vector 也不防御你一边 push_back 一边在另一个线程析构这个 vector。所有标准库容器
> 都是这种态度。这是 C++ "**正确使用的代码不应该为错误使用付运行时成本**" 的体现。

### 调用方怎么保证 quiescent——四种姿势

#### Pattern A：单线程使用 (最常见)

```cpp
int main() {
    SyncBackend be(8);
    be.submit(reqs, N);
    be.reap(out, N, N);
}   // 主线程析构, 唯一使用者就是自己, 零 race
```

backend 内部的 N 个 worker 是它自己的事, 析构里 join 它们就够了。**多数 bench 代码、
多数实际用法都是这种**。

#### Pattern B：多线程使用, 靠 join 同步

```cpp
SyncBackend be(8);
std::thread producer([&]{ for (...) be.submit(...); });
std::thread consumer([&]{ for (...) be.reap(...); });
producer.join();    // ← happens-before 边
consumer.join();    // ← happens-before 边
// 这里保证 producer/consumer 已经退出, 不会再碰 be
// be 析构安全
```

`std::thread::join()` 的 happens-before 是 C++ 标准给的保证——这是多线程代码最常用的
析构-同步姿势。

#### Pattern C：循环型应用 + 显式 stop 信号

```cpp
std::atomic<bool> app_stop{false};
SyncBackend be(8);
std::thread worker([&]{
    while (!app_stop.load(std::memory_order_acquire)) {
        be.submit(...); be.reap(..., 1);
    }
});
// ...
app_stop.store(true, std::memory_order_release);
worker.join();
// be 析构安全
```

**绝对不要**用 `be.in_flight() == 0` 来判断"可以析构了"——in_flight 只反映 backend
内部状态, 完全不能反映"调用方线程还会不会再调 submit"。**这是应用层信息, 必须用应用层
同步原语**。

#### Pattern D：万不得已的 shared_ptr<SyncBackend>

只有在生命周期真乱到没法用 join 表达时 (异步回调可能在任何时刻触发) 才上：

```cpp
auto be = std::make_shared<SyncBackend>(8);
register_callback([be]{ be->submit(...); });   // 拷贝 shared_ptr
// 最后一个回调结束、释放最后一个 shared_ptr 时, backend 析构
```

代价：所有传递都带原子 ref-count, 强制堆分配, API 传染。**不要默认用这个**, 它解决的是
"所有权不清晰"的问题, 而 SyncBackend 的所有权通常很清晰。

---

## 析构的"优雅排空"语义

worker_loop 的 predicate 是 `!sq_.empty() || stop_.load()`, 注意是 **或**：

```cpp
sq_cv_.wait(lk, [&] {
    return !sq_.empty() || stop_.load(std::memory_order_acquire);
});
if (sq_.empty()) return;   // stop_ 触发且队列空了 → 退出
```

stop_ 设了之后, 如果 sq_ 里还有积压请求, worker 走 `!sq_.empty()` 那一支, **继续把
请求做完**, 推进 cq_, 再回 wait。直到 sq_ 真正空了, 下一次 wait 返回时才退出。

也就是说, 析构语义是 **"优雅排空"**——还没做的 IO 会做完, 结果推进 cq_, 但 cq_ 没人
读、随对象一起销毁。这是有意识的设计取舍：

> "我承认你提交过的请求, 但你既然不来 reap, 结果就丢了。"

如果想要"立即丢弃 sq_ 里的请求", predicate 改成只看 stop_, worker 退出前清空 sq_ 即可。

---

## 为什么析构 sq_cv_ 用 notify_all 但 cq_cv_ 什么都不做

**sq_cv_ notify_all 的必要性**：N 个 worker 都睡在 wait 里, 必须**全部叫醒**才能 join。
notify_one 只叫一个, 剩下 N-1 个继续睡——再也不会有 submit 来 notify 第二次了, join
死锁。

**cq_cv_ 不需要任何动作**：
1. cq_cv_ 的等待者是**外部调用方** (调 reap 的人), 不是 backend 内部线程。
2. 析构契约要求 quiescent, 所以**不应该有人此时阻塞在 cq_cv_ 上**——如果有, 那是
   调用方的 bug, 不是 backend 该兜底的。
3. 就算兜底 (来一发 cq_cv_.notify_all), 被叫醒的 reap 线程拿到锁后会访问 cq_, 但 cq_
   马上要随 backend 析构。叫醒它只是让 crash 来得更快, 没解决问题。

**根本原则**：destructor 的职责是清理自己创建的资源 (那 N 个 worker), 不是兜底外部
调用者的状态。RAII 的规矩是谁 new 谁 delete, 谁 spawn 谁 join。

---

## 容易踩的坑总结

| 坑 | 错误代码 | 后果 | 正确做法 |
|---|---|---|---|
| submit 完释放 buf | `submit(); buf.clear();` | UAF | 等到 reap 回来再释放 |
| 把 reap 当同步用 | `for (i) { submit(&r[i],1); reap(&o[i],1,1); }` | 吞吐塌成 1/N | 批量 submit 后批量 reap |
| 用 in_flight 判断完成 | `while (be.in_flight()) {}` | cq_ 里还有数据没收 | 用 reap 的返回值累加 |
| 不检查 submit 返回值 | `be.submit(reqs, N);` | 换 io_uring 后会丢请求 | while 循环 retry |
| 多线程 submit/reap 后裸析构 | `~Backend()` 直接调 | UAF | 先 join 用户线程 |
| 性能优化时给 buf 加 shared_ptr | `shared_ptr<char[]> buf` | 失去和 io_uring 对比的公平性 | 保留裸指针 + 调用方负责 |
