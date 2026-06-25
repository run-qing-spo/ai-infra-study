# LRU 项目测试设计

本文档配合 `testing_notes.md` 读 —— 那篇讲**为什么这么测**(锁保护资源、什么算合法状态),这篇讲**具体测了哪些 case、每个 case 在防什么**。读完这篇能回答:

1. 看到一个 test name,知道它在抓什么 bug
2. 看到 bench 输出某一行,知道这个数字是怎么测出来的、含义是什么
3. 想加新变体(比如 lru_lfu)时,知道至少要照搬哪些 case

---

## 一、四层测试金字塔

代码里用 `Layer N` 注释标了层级,逐层加压力:

| 层 | 文件 | 目标 | 何时跑 |
|----|------|------|------|
| Layer 1 单线程功能 | `test/test.cpp` 前半 | API 行为符合 LRU 语义(覆盖、evict、erase) | `make test` |
| Layer 2 单线程不变量 | `test/test.cpp` 后半 | 内部资源结构自洽(audit) | `make test` |
| Layer 3 并发不变量 | `test/test_mutex.cpp` / `test/test_sharded.cpp` | 多线程压力下不变量仍然成立 | `make test` + `make test-tsan` |
| Layer 4 性能基准 | `test/bench.cpp` | 量化"锁的代价" + "分片的收益与副作用" | `make bench` |

**测试时 Layer 1-3 全走,Layer 4 是性能数据,不参与 pass/fail**。

---

## 二、Bench 工作负载定义(读这一节才能看懂 bench 输出)

### 三种 workload —— 操作占比骰子

`bench.cpp` 顶部定义:

```cpp
{"read-heavy",  95,  5,  0}   // get 95% / push 5% / erase 0%
{"balanced",    50, 30, 20}   // get 50% / push 30% / erase 20%
{"write-heavy", 10, 70, 20}   // get 10% / push 70% / erase 20%
```

每次操作前 `rng() % 100` 掷骰子,落进哪个区间就跑哪种操作。**这是访问模式占比,不是命中率**。命中率取决于 key 分布 + 缓存容量 + 替换策略,跟 workload 是两件事。

### 容量与 key 空间

```cpp
constexpr int capacity = 1024;
constexpr int key_space = 2048;   // = 2 × capacity
```

key 在 `[0, 2048)` 均匀随机抽。**为什么是 2x 容量**:稳态命中率约 50%,既不是"刚 push 就 hit"的退化情况,也不是几乎全 miss 的极端情况 —— 留出空间让 LRU 替换策略真正干活。这个数字直接决定了 bench 里每一行的 hit rate 都在 ~50%,看到偏离 50% 就是工作负载在和容量打架。

### 计时基础设施

- 吞吐 = 总操作数 / wall clock 时间,跑 `1500ms` 取均值
- 延迟用 `steady_clock::now()` 直接打时间戳,有 20-40ns 调用开销 —— **绝对值不准,只能横向比**
- 每次操作前 `pick_op` 和 RNG 步进都在临界区**外**,不污染锁时间测量

---

## 三、Layer 1-2:单线程功能 + 不变量(`test/test.cpp`)

19 个 `LruCacheBaseTest` + 3 个 `LruBaseRobustness`,每条都是"造一个状态 → 触发一次操作 → 断言结果 + audit"。

### 按抓的 bug 分组

| 组 | 代表 test | 在防什么 bug |
|----|----------|------------|
| **基本 LRU 语义** | `PushAndGet`, `EvictFullPages`, `PushSameKey` | get/push 行为是否符合"最近用过的留在头部" |
| **erase 路径** | `EraseHead/Tail/Middle`, `useAfterErase` | erase 不同位置后,链表 + map + free list 三处都得对得上 |
| **边界容量** | `CapacityOneEraseThenPush`, `ConstructorThrowsOnNonPositiveCapacity` | 容量为 1 的极小情况、非法容量(0/负数)的早期失败 |
| **覆盖路径** | `PushSameKeyAtCapacityUpdatesValueWithoutEviction` | 满载下 push 已存在的 key 应该只更新 value、不触发 evict |
| **get 不副作用** | `GetMissDoesNotChangeOrder` | 未命中的 get 不应改变 LRU 顺序 —— 容易写漏 |
| **重复折腾后还干净** | `FreeListIntegrityAfterManyCycles`, `AuditHoldsThroughHeavyChurn` | free list 单向链表反复 push/evict 是否会撕裂 |
| **跨操作的不变量** | `AuditHoldsAcrossOperations`, `KeyValueConsistencyAfterMixedOps` | 不是单次操作的对错,是一长串混合操作走完后,资源是否仍处合法状态 |
| **异常安全** | `LruBaseRobustness.PushThrowOn*`, `ErasedSharedPtrStaysAlive` | K 的拷贝构造抛异常时,内部状态要么完全成功要么完全回滚;erase 后 caller 拿着的 shared_ptr 仍然安全 |

### audit() 是核心
单线程也跑 audit,**因为单线程的 bug 留在状态里**,下次操作才暴露,而 audit 把它当场抓到。audit 检查的资源关系见 `testing_notes.md` 第二节"形态 A"。

---

## 四、Layer 3:并发不变量

### 共同模板

不管是 `lru_mutex` 还是 `lru_sharded`,并发测试都是同一个套路:

1. 起 `2 × hardware_concurrency` 个线程
2. 每个线程跑数万次 `get` / `push` / `erase` 混合操作(按权重 5:3:2)
3. 全部 join 后,做两层检查:
   - **结构层**:`audit()` 返回空串 —— 内部链表 / map / free list 自洽
   - **内容层**:遍历所有可能的 key,凡是 `get` 拿得到的,`*sp` 必须等于 key 本身(因为约定 `v = k`)。如果某个 key 拿出来的 value 是别的 key 的 value,说明发生了**撕裂写**(线程 A 写 key/value 的中间状态被线程 B 看见)
4. 整体用 `std::async + wait_for(30s)` 兜底,防死锁让测试无限挂死

### `lru_mutex` 的三个 case

| Case | 在防什么 |
|------|--------|
| `ConcurrentMix` | 经典:join 后验证容量上界 + value 合法范围 + cache 仍可用 |
| `KeyValueConsistencyAfterConcurrentStorm` | 比 ConcurrentMix 严:加上"每个 key 的 value 没被串到别的 key 上"——抓撕裂写 |
| `ValuesSnapshotInternallyConsistent` | 写线程持续 push/erase 时,values() 拿到的快照必须自洽。**为未来谁拆锁优化做回归保护** —— 现在 values() 是整临界区,将来如果细化锁,这条会先报警 |

### `lru_sharded` 的五个 case

| Case | 在防什么 |
|------|--------|
| `BasicFunctional` | 单线程 64 key 灌进 16 shard,验证基本功能 + audit + 单 shard 内 LRU 语义还成立 |
| `EraseAndReinsert` | erase 后再插入同 key 能不能正确路由(防 splitmix 实现 bug,比如忘了 mix) |
| `ConcurrentStorm` | 复刻 mutex 版的 storm,但 `key_space=256, cap=64, shard=16`,**确保流量真的跨 shard**(关键:如果 key 范围过小,可能退化成单 shard 测试,白测) |
| `ValuesSnapshotInternallyConsistent` | 跨 shard 聚合的 values() 是不是仍然每个元素都合法。上界改为 `per_shard × shard_count`(分片版的"近似总容量") |
| `RoutingDoesNotBlackholeKeys` | 灌 256 key,给 16x 头空间(避开 balls-in-bins 方差),验证全部 key 都能 get 回来 —— 抓"路由把 key 算到不存在的 shard"或者"模 0"这类粗 bug |

### `RoutingDoesNotBlackholeKeys` 的容量为什么是 16x

256 个球扔 16 个桶,**就算 hash 完美均匀**,某些桶的负载也会超过均值 ~7-8 个(标准的 balls-in-bins 集中度上界)。如果 `cap = keys`,即 per_shard = 16,某些 shard 必然 evict,测试会假阳性失败。最初版本就踩了这个坑,改成 `cap = 16 × keys` 留出余量。**这是"hash 均匀"不等于"分桶大小均匀"的一个直接教训**。

### 为什么 storm 测试 key_space 一定要大

如果 `key_space < shard_count`,大量请求会塌到同一个 shard,等于退化成单 shard 测试。`test_sharded.cpp` 里 `key_space=256, shard=16`,**至少 16x 关系**,保证每 shard 都在被压。

---

## 五、Layer 4:性能基准(`test/bench.cpp`)

四个 section,每个回答一个不同的问题。

### Section 1 — 单线程 base vs mutex(锁的"裸成本")

| workload | base (M/s) | mutex (M/s) | mutex cost |
|----------|-----------|-------------|------------|
| read-heavy | 40.45 | 35.55 | 12.1% |

回答的问题:**在没有任何竞争的情况下,加一把锁本身要花多少?** 这是 `std::mutex::lock/unlock` 的无竞争开销(快路径,通常是一次 atomic CAS)。约 10% 是合理水位,如果某天这个数字飙升,说明 lock 实现退化或者临界区里多了不该有的工作。

### Section 2 — `lru_mutex` 多线程 scaling(锁的"竞争成本")

| threads | total (M/s) | speedup |
|---------|-------------|---------|
| 1 | 32.99 | 1.00x |
| 2 | 12.05 | 0.37x |
| 4 | 8.54 | 0.26x |
| 8 | 6.60 | 0.20x |

回答的问题:**为什么加线程吞吐反而暴跌?** 因为所有线程都在排队同一把锁,加线程只是让队列更长,加上每次锁释放都引发跨核 cache invalidation,总吞吐 < 单线程。**这是分片要解决的痛点**,bench 1 和 2 是分片登场的动机说明。

### Section 3 — `lru_sharded` 分片数 scan(分片的"收益与副作用")

固定 8 线程,扫 shards ∈ {1, 2, 4, 8, 16, 32, 64, 128},三种 workload。回答的问题有四个:

**Q1. 分片真的能横向扩展吗?**
看 read-heavy:shards=1 → 6.84,shards=128 → 18.69,2.7x 提升。**能**。

**Q2. 为什么 shards=2 反而比 shards=1 慢?**
8 线程争 2 把锁的代价 > 8 线程争 1 把锁。
- shards=1:8 线程进入同一个等待队列,锁释放后下一个立刻接上(convoy),cache line 在固定位置,**反而局部性好**
- shards=2:两把锁的 cache line 在不同位置,两边都被 8 核反复抢,**cache-line ping-pong**;加上两个独立 hashmap/pool 把数据切散,locality 更差;再算上 splitmix + mod + unique_ptr deref 的固定开销 —— 总账亏
- shards=4 才反超

教训:**分片数远小于线程数时是负优化**,工程上要么不分要么直接给到 4-8x 线程数以上。

**Q3. read-heavy 和 write-heavy 的改善比例为什么差不多?**
之前预期"write 改善大",错了。原因:**LRU 的 get 也要排他锁**(因为要 moveToHead),临界区长度和 push 差不多,锁竞争视角下读写等价。这正是 overview 里"为什么 get 不能用 shared_mutex 简单优化"那一条 —— bench 把它量化了。

**Q4. 命中率会不会随分片增加退化?**
看 balanced:49.9% → 43.6%,降 6 个百分点。原因:每 shard 容量 = total/N,N 越大每 shard 越小;热点 key 簇聚在某 shard 时,那个 shard 在拼命 evict,而其他 shard 还闲着 —— **全局容量被切碎成 N 份近似容量**。read-heavy 退化弱是因为 95% 都是 get,写少 → evict 少 → 切碎的代价表现不出来。

### Section 4 — 单线程延迟分位数

回答的问题:**每个操作的延迟分布是什么样的?** 给个直觉锚点,跟后续优化(无锁版、tryLock 等)做对照。p999 和 p50 的差距能反映"长尾事件"出现的频率 —— 例如 push 偶尔触发 evict + 链表重排,会拉长尾。

---

## 六、配套约束(Makefile 里写死的事)

- 测试编译带 `-DLRU_TEST_HOOKS`,这是 `audit()` 的开关。**绕开 Makefile 用 `g++ test.cpp ...` 单独编译会让 audit 失效**,详见 `.claude/skills/lru-verify/`
- bench 编译**不带** `LRU_TEST_HOOKS`。当前 hook 只门控 `audit()` 方法本身,bench 不调用它,所以现在两种构建的热路径指令几乎一致。**这条规则是防御性的**:未来若有人把 audit 类钩子塞进热路径(例如每次 push 后自动 sanity check),bench 自动免疫,数据不会被悄悄污染。顺带让 bench 二进制干净、跟测试构建解耦
- TSan 走单独二进制 `build/test_tsan_bin`,需要重新插桩,不能复用 `test_bin`

---

## 七、加新变体时的最低测试清单(给未来的 lru_lfu / lru_lockfree 用)

照搬以下 case,只改容器类型 / 命名空间:

- Layer 1-2:所有 `LruCacheBaseTest` 都套一遍(API 行为应该等价)
- Layer 3:`ConcurrentStorm` + `ValuesSnapshotInternallyConsistent`
- 自己加一条:任何"声称改善锁竞争"的变体,都要在 bench 里加上和现有 baseline 的对照
- 任何引入新的内部约束(比如 LFU 的 freq counter)都要新写一条 audit 不变量
