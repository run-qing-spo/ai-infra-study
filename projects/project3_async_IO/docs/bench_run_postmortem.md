# bench 实跑复盘:从环境噪音到 slot 设计的测量偏差

本文记录在 Ubuntu 22.04 远端机上第一次跑 project3_async_IO bench 时实际遇到的
三个问题。前两个是工程噪音(API 版本、Makefile 路径冲突), 修完就过;第三个是
**bench 测量基础设施自己的 bug**, 跟被测的 IO 系统无关, 但会让 latency
统计输出系统性偏低的数值——这个才是值得记一笔的核心。

> 触发场景:第一次在新机器上跑 bench, 一路从"编译不过"推到"max=UINT64_MAX-49",
> 修完 max 之后又意识到 slot 设计里还有一个更隐蔽的跨周期问题没解决。

---

## 问题一:liburing 2.1 缺 `*_data64` API

`apt install liburing-dev` 装上之后, `make` 仍然报错:

```
error: 'io_uring_sqe_set_data64' was not declared in this scope;
       did you mean 'io_uring_sqe_set_data'?
```

Ubuntu 22.04 的 liburing 是 2.1, 而 `io_uring_sqe_set_data64` /
`io_uring_cqe_get_data64` 是 2.2 才加入的便利接口。两套 API 底层完全等价——
SQE 里 `user_data` 槽位本身就是 64-bit, 2.2 加 `_data64` 只是免除强转 `void*`
的语法糖。所以兼容老版本只要:

```cpp
io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(r.user_data));
out[reaped].user_data = reinterpret_cast<uint64_t>(io_uring_cqe_get_data(cqe));
```

这种 cast 是合理的, 因为指针和 `uint64_t` 在 x86_64 上同宽, 且容器底层就是
原样存的 64-bit 字段——`reinterpret_cast` 不引入语义破坏, 只是绕过类型系统的
形式障碍。

> 面试视角:这种"API 版本演进里其实只是包装/语法糖"的判断很值钱。面试中如果
> 被问"为什么要 cast", 答得出来"底层字段宽度相同, 仅类型形式不一致"比答
> "因为编译器要求"高一个层级。

---

## 问题二:Makefile target 路径跟 obj 路径相撞

原 Makefile:

```
TARGET   := $(BUILD)/bench       # 期望产出文件 build/bench
BENCH_O  := $(BUILD)/bench/bench.o  # 期望产出文件 build/bench/bench.o
```

GCC 编译 `bench.o` 时会 `mkdir -p build/bench/`, 此时 `build/bench` 已经是
**目录**。最终链接阶段 `ld -o build/bench` 直接报:

```
/usr/bin/ld: cannot open output file build/bench: Is a directory
```

修复是把所有 `.o` 放到独立的 `build/obj/<原路径>/`, 最终产物保留
`build/bench`。这种问题用 CMake / Bazel 之类不会碰到, 但手写 Makefile 时
是个低概率坑——记一下提醒未来对 BUILD 子目录命名做最小防御。

> 这个问题面试基本不会问, 但作为"环境噪音"是真实成本的一部分, 也是为什么
> 工程上多数大项目会上 CMake 的微观理由之一。

---

## 问题三:slot 设计与跨周期复用——主菜

### 第一层 bug:同 batch 内 submit_ts 被自己覆盖

第一次跑 sync 后端 default 配置, 输出:

```
latency ns: p50=356687 p90=633544 p99=699437 p99.9=721788 max=18446744073709551566
```

`max ≈ UINT64_MAX - 49`, 显然是 `uint64_t` 下溢。追到代码:

```cpp
while (clk::now() < t_end) {
    size_t k = backend->reap(cqe_buf.data(), cqe_buf.size(), 1);
    auto now = clk::now();    // 整 batch 共用的时刻
    for (size_t i = 0; i < k; ++i) {
        ...
        auto lat = now - submit_ts[id % qd];   // ① 读
        latencies_ns.push_back(lat);
        submit_one();                          // ② 写:更新 submit_ts[slot]
    }
}
```

`submit_one()` 内部会执行 `submit_ts[next_id % qd] = clk::now()`, 这个 `now()`
**晚于** batch 共用的那个 `now`。如果 sync 后端把 batch 里的请求乱序完成
(同一批 reap 返回的 id 集合不连续), 第 `i` 次循环里 ② 写入的 slot 可能正好
是后续第 `j>i` 次循环里 ① 要读的 slot。lat 被算成"晚 ts - 早 now" = 负几十 ns,
强转 uint64_t 就成了 UINT64_MAX 附近的值。

修复是把循环拆成两阶段:**先把整 batch 的 lat 全算完, 再统一 submit_one()**:

```cpp
size_t to_resubmit = 0;
for (size_t i = 0; i < k; ++i) {
    ... lat = now - submit_ts[id % qd];
    ++to_resubmit;
}
for (size_t i = 0; i < to_resubmit; ++i) submit_one();
```

这样 batch 内读写顺序固定:所有读都发生在所有写之前。验证跑 5 次 sync, max
全部落在 1.0M~11M ns 量级, 无 UINT64_MAX 异常。

### 第二层 bug:跨周期 slot 复用——两阶段救不了

修完第一层之后, slot 设计里还残留一个更隐蔽的问题, 这次跨的是 batch 边界。

**触发条件:** 任何 reap 不 drain 满 qd 的场景。具体推演:

- 初始 qd 个 in-flight, id=0..qd-1, 各占独立 slot, 没问题
- 某次 reap 返回 k < qd 个, 设 reaped_ids = {0, 2, 3}(sync 后端乱序完成),
  id=1 还在飞
- 两阶段写阶段:submit 3 个新 id = qd, qd+1, qd+2, slot = 0, 1, 2
- 注意 **slot 1 被 id=qd+1 写了新 ts, 但 id=1 还在飞**
- id=1 后续完成时读 `submit_ts[1]`, 拿到的是 id=qd+1 的提交时刻
- 后果:`lat = now - 一个比真实 submit 时刻晚得多的 ts`, **严重低估真实延迟**

注意这次不会下溢——id=qd+1 的 ts 仍然早于 reap 时刻——但 lat 是"早 ts 应该
是真值, 实际拿到了一个晚 ts, 差值小一截"。形态不是异常值, 而是**系统性偏差**,
比第一层 bug 更危险:出来的数据看着合理, 但本质是错的。

### 诊断方法

不需要静态分析或者反复阅读 sync_backend 实现, 加一个 mirror 数组就行:

```cpp
std::vector<uint64_t> slot_last_id(a.qd, 0);
// submit 时:
slot_last_id[slot] = next_id;
// reap 时:
if (slot_last_id[slot] != id) {
    ++collisions;
    uint64_t gap = slot_last_id[slot] - id;  // 应当是 qd 的整数倍
    ...
}
```

如果跨周期复用真的发生了, `gap` 必然是 qd 的整数倍——因为 slot 只能被
`id`, `id+qd`, `id+2qd`, ... 这条等差数列上的 id 覆盖。

### 第一轮实测:cached 模式

各配置跑 5s, page cache 命中状态:

| 配置          | collisions       | max_gap     | 完成数  | p50      |
|---------------|------------------|-------------|---------|----------|
| sync qd=32    | 0                | 0           | 235k    | 350 μs   |
| sync qd=8     | 87,497 (2.83%)   | 24 = 3·qd   | 3,088k  | **6.3 μs** |
| uring qd=32   | 0                | 0           | 3,588k  | 22.8 μs  |
| uring qd=128  | 0                | 0           | 3,552k  | 90.1 μs  |

三个初步观察(其中 1 和 2 在第二轮 --direct 测试中被部分推翻, 见"问题三续"):

**1. 这一轮里 collision 只在 sync 后端 qd=8 出现。** cached 状态下 IO 几乎
瞬时——sync 后端 reap 时所有 in-flight 都已完成可以一次拿光, uring 的 peek
同样能一次拿光 ready CQE, 两者实质上都是 "drain 满"模式, 所以 0 collision。
qd=8 出现的少量乱序是 sync 后端在小 batch 上偶尔的时序波动。**注意这个结论
依赖 cached 假设, 是脆弱的——一旦 IO 真实延迟拉长就会崩。**

**2. max_gap = 24 = 3·qd 给出 cached 模式下冲突严重程度的上界。** 意味着在
qd=8 时, 至少有某个 id **被推后 3 个完整周期才回来**——那个 slot 在它完成前
被覆写了 3 次。

**3. (此条已撤回, 详见"修复后回填")** 第一轮我们把 sync qd=8 p50=6.3μs
跟 sync qd=32 p50=350μs 的两个量级差归因于 "qd=8 被 2.83% collision 大量
拉低"。修复后 sync qd=8 p50 仍是 ~6 μs, **基本没动**——这是真值, 不是污染。
真正反常的是 sync qd=32 为什么这么慢, 那是 sync_backend 自身的扩展性问题,
跟 slot bug 无关。

### 修复方案权衡

三种走法, 按改动面排:

| 方案                              | 内存代价  | 每 op 开销 | 改动面 |
|-----------------------------------|-----------|------------|--------|
| `slot = id % (k·qd)` 环形数组扩容 | k 倍 ts   | 0          | 最小   |
| `unordered_map<id, ts>` 按 id 索引| 较高(哈希)| 哈希查/插  | 中等   |
| 强制 drain 模式(reap 满 qd 才处理)| 0         | 增加阻塞   | 小     |

cached 数据下看起来环形数组扩容最 pragmatic:管线本身 ≤ qd, 加倍存就够。
**但这个推荐被第二轮 --direct 测试推翻了——实测 max_gap 飙到 33·qd, 静态 k
怎么选都不稳, 详见下面"问题三续"。**

map 方案更严谨, 每 op 多一次哈希查+插+擦, 在 cached + uring 的 700 k ops/s
量级上 overhead 不可忽略, 但在 direct 模式真实 NVMe 路径上(~100 μs/op)百
纳秒级哈希查是噪声。

强制 drain 简单粗暴, 但**改变了被测系统的语义**——bench 不再测真实流水线场景,
变成测 batch 边界对齐的理想形态, 失去了原本的测量价值。

---

## 问题三续:--direct 把机制贡献和 bug 触发条件一起重排

第一轮的数据全是 page cache 命中状态。打开 --direct 让 IO 真的走 NVMe 之后,
**第一轮的两个核心结论都被推翻或大幅修正**, 同时浮出来一组关于 io_uring
机制贡献的洞察, 比 bug 本身更值钱。

### 设置坑:稀疏文件让 --direct 退化成 cached

第一次打 --direct 跑出来的数据非常可疑——uring direct qd=32 仍然 673 k ops/s,
跟 cached 几乎打平。检查 file:

```
Size: 4294967296    Blocks: 0    (du: 0)
```

ftruncate 创建的是**稀疏文件**, 逻辑大小 4 GiB, 实际分配 0 block。ext4 读
sparse hole 直接返回零, 不真走盘。--direct 标志只 bypass page cache, 没法让
sparse hole 变成真盘 IO。

修复:`dd if=/dev/urandom of=/tmp/p3_bench.dat bs=1M count=4096 conv=fdatasync`,
把 4 GiB 实数据写进去。**不能用 /dev/zero**——ext4 上某些路径会把 zero block
压缩成 unwritten extent, 读回来仍然不走盘;urandom 保证每 block 都有真随机
内容, 没有任何 short-circuit。

这个坑是测量类工作的通病:**测试夹具(file)的默认状态可能跟被测系统(NVMe)
的真实使用场景错位**。光开 --direct 不够, 必须保证 file 已经在盘上有真实数据。

### 真实 --direct 实测

| 配置                  | 吞吐         | p50         | collisions          |
|-----------------------|--------------|-------------|---------------------|
| sync  cached  qd=32   |  47 k ops/s  | 350 μs      | 0                   |
| **sync  direct qd=1** |  **7.6 k**   | **125 μs**  | 0(干净基线)        |
| sync  direct  qd=32   |  45 k        | 410 μs      | **46.34%**, gap≤384 |
| uring cached  qd=32   | 696 k        |  23 μs      | 0                   |
| **uring direct qd=32**| **45 k**     | **417 μs**  | **45.64%**, gap≤256 |
| uring direct  qd=128  |  51 k        | 1.55 ms     | 47.26%, gap≤1152    |
| uring direct  qd=256  |  51 k        | 3.97 ms     | 47.30%, gap≤8448    |

### 修正 1:bug 触发条件比第一轮总结的宽得多

第一轮的"collision 只在 sync 后端 qd=8 出现"错了。--direct 之后:
- sync qd=32 collision 从 0 → **46%**
- uring qd=32 collision 从 0 → **45%**
- uring qd=256 见到 max_gap = 8448 = **33·qd**

真正的触发条件是 **"reap 一次拿不光所有 in-flight 的实际 workload"**——
跟后端类型无关, 跟 qd 大小也只是间接相关, 直接相关的是
**[IO 真实延迟 / 用户态 batch 周期]** 的比值:

- cached:read 几乎瞬时, reap 调用之间所有 in-flight 都已经完成 → 每次 drain
  到底 → 0 collision (无论 sync 还是 uring)
- direct:NVMe 真实延迟 ~130 μs, 远大于用户态一次 reap+resubmit 的周期,
  reap 时只能拿到部分完成 → collision 全面爆发

这把"修复方案权衡"里推荐的 `k=2 环形数组` 直接打脸——max_gap 实测达到
33·qd, 静态 k 怎么选都不稳。**真要修这个 bug, 应该改成
`std::unordered_map<id, ts>` 按 id 直接索引**, 让 storage 跟着 ID 走而不是
跟着 slot 走。哈希查/插的百纳秒级 overhead 在 direct 模式的微秒级 IO 路径上
是测量噪声, 可以接受。

### 修正 2:机制贡献完全重排

io_uring 在 cached vs direct 两种工况下的优势来源是**两套独立机制**:

| 模式      | sync qd=32  | uring qd=32 | uring/sync | 主导机制                 |
|-----------|-------------|-------------|------------|--------------------------|
| cached    |  47 k ops/s | 696 k ops/s | **15×**    | syscall 摊销 (CPU 路径)|
| direct    |  45 k ops/s |  45 k ops/s | **1×**     | NVMe 设备饱和            |

bench.cpp 顶部注释里写得很清楚:`sync → N 个 worker 线程, N = qd`, 不是单
线程串行。所以 cached 模式下 sync 32 线程并发跑, 还是被打 ~15×, 差的全在
**每 op 都要走完整的 syscall 入/出 + 4 K memcpy**。uring 一次 enter 把 N 个
请求一并提交, 一次 reap peek 把所有 ready CQE 一并拽出, syscall 数被摊得
极薄, 拿到 ~15× 提升。

direct 模式下两条路径都被 **NVMe 本身的并发上限**卡住:
- sync 32 个线程都阻塞在 pread, 但 NVMe 队列只能容纳 ~6 个并发
- uring 单线程提交 32 SQE, kernel 把它们送进 NVMe 队列, 同样最多 ~6 并发
- 设备饱和点是物理上限, 跟提交方式无关

继续加 qd **只换 latency 不换 throughput**:qd=128 / 256 都顶在 51 k ops/s,
p50 从 1.5 ms 涨到 4 ms, 是 Little's law 在工作。

NVMe queue depth ≈ 6 是从 `sync qd=1 = 7.6 k ops/s` 反推的:单 in-flight 时
设备服务一个 op 要 1 / 7.6 k ≈ 131 μs, 接近 sync qd=1 的 p50=125 μs。uring
qd=32 达到 45 k = 5.9 × 7.6 k, 说明设备实际并发深度约 6。X15 SSD 标称能做
几十万 IOPS, 但在单线程 + ext4 + 4 K 随机这条路径下被限制在这个量级——要
往上突破得多线程裸 block device 或多 fd 并行, **这是另一个独立的工程话题**。

抽象出来:**io_uring 的两类收益是分开计费的**, 必须分别识别。

- syscall 摊销:仅在 CPU 路径主导(IO 短) 的工作负载里显现
- 设备级并发:仅在设备未饱和、且单线程 sync 已经压不满设备时显现

要同时拿到两块, 需要既"IO 不便宜"又"sync 单线程压不满设备"——典型如
**慢 SSD + 高 fanout 读** 或 **网络 IO + 多请求复用**。X15 NVMe + 单线程 ext4
+ 4 K 随机不是这种场景, 所以 direct 下 uring 的优势直接归零。

### 浮现的工作流信号:`sync direct qd=1` 是天然 ground truth

qd=1 时 submit_ts 数组只有一个 slot, 不可能跨周期复用, **测量必然干净**。
它给出真实 per-op latency: **p50 = 125 μs**, 跟消费级 NVMe 4 K 随机读延迟
公开数据一致。

有了这个锚点, 就能反向验证其他配置的数据是否合理。例如 sync direct qd=32
报 p50=410 μs:
- 32 个 worker 线程并发, NVMe 队列只能容纳 ~6 个同时跑
- 排在 6 之后的 op 要等队列里前面跑完才能进
- 一个 op 的真实生命周期 ≈ 排队等待 + NVMe 服务时间
- 平均下来 ≈ 32 / 6 × 125 μs ≈ 670 μs
- 报 p50=410 μs 比理论低 40%, 强烈提示**测量被 collision 拉低**

这种"用一个干净配置反向 sanity check 其他配置"的工作流, 比单看任何一组数字
都靠谱——它是这套 measurement bug 最容易被发现的形态信号, 也是后续要保留
qd=1 配置常跑的理由。

---

## 修复后回填:Little's Law 验证 + 一处误判修正

slot bug 最后选了 `std::unordered_map<uint64_t, clk::time_point>` 按 id 直接
索引、reap 时取完即删的方案(对应"修复方案权衡"表里中间那行)。改完之后重跑
一轮验证, 跟 Little's Law 给出的 p_avg 对账:

| 配置                | 修前 p50 | 修后 p50  | 修后吞吐 | Little 实测 p_avg = qd/tp | 误差   |
|---------------------|----------|-----------|----------|---------------------------|--------|
| sync  direct qd=32  | 410 μs   | **781 μs**| 40.9 k   | 32  / 40.9k = **782 μs**  | 0.13%  |
| uring direct qd=32  | 417 μs   | 776 μs    | 41.1 k   | 32  / 41.1k = 779 μs      | 0.4%   |
| uring direct qd=128 | 1.55 ms  | 2.69 ms   | 46.6 k   | 128 / 46.6k = 2.74 ms     | 1.8%   |
| uring direct qd=256 | 3.97 ms  | **5.41 ms**| 46.6 k  | 256 / 46.6k = **5.49 ms** | 1.5%   |

修后所有 direct 配置的 p50 都跟 Little's Law 给的 p_avg 误差在 **2% 以内**
(p50 < p_avg 是因为分位排序里中位略小于均值, 跟长尾对应)。两件事被证实:

1. unordered_map 方案彻底解决了 slot 复用对 latency 测量的污染
2. 之前 direct 模式下报的所有 latency, 都被系统性拉低了 ~1.7-1.9× (qd=32)
   到 ~1.25× (qd=256, 偏差被深 qd 摊得更薄)

### 顺手修一处误判:sync cached p50 偏差不是 slot bug

第一轮的"三个观察"#3 把 sync cached qd=8 报 p50=6.3 μs vs qd=32 报 350 μs 的
两个量级差归到"qd=8 被 collision 污染"。修复后 qd=8 仍是 p50≈6.0 μs, **没有
任何变化**, 推翻了那条归因。把这两组数字摆好:

| qd  | sync cached 吞吐 | sync cached p50 |
|-----|------------------|------------------|
| 8   | 655 k ops/s      | 6.0 μs           |
| 32  | 47 k ops/s       | 350 μs           |

6 μs 反映的是 **8 worker 线程并发跑 cached read 的真实单 op 延迟**——
syscall 入/出 + 4 KiB memcpy 加起来差不多就是这个量级。2.83% 的 collision
确实发生, 但被污染的样本数太少, 撼动不了 p50。

**真正反常的是 qd=32 这一边**:8 线程 → 32 线程, 吞吐**下降 14×, p50 上升
58×**, 完全反直觉。常识上 32 线程应该比 8 线程能压更多 cached read。这不是
bench 测量 bug, 是 sync_backend 自身的扩展性问题, 可能原因:

- 内部任务队列上的 mutex 争用随线程数线性恶化
- 32 worker 线程在物理核数有限时上下文切换抢占
- 唤醒 / cv broadcast 风暴

具体定位不在本文范围, 留个独立 issue 追:**sync_backend 在 qd ≥ 32 时
cached 性能崩溃**。这跟 slot bug 是两件事, 之前混在一起是误判。

### 还有一个修后才看清的形态信号

修后再看 sync direct qd=32 (40.9 k) 和 uring direct qd=32 (41.1 k) 几乎完全
重合, p50 也都 ~780 μs。这跟"两条路径都被 NVMe queue 卡死"的判断完全一致,
之前因为 slot bug 把两边 p50 都拉低、还拉低得不一样多, 反而模糊了这个对称性。
干净的测量让这个机制结论变得显眼——很多时候 bug 不只是把数字拉错, 还会**让
本来应该浮现的物理规律藏起来**。

---

## 可迁移的判断公式

这个 bug 的本质不是 IO 系统的 bug, 是**测量基础设施和被测系统共享了状态**
(`submit_ts` 数组), 而被测系统的状态机演化(slot 复用)污染了测量。

抽象成一条公式:

> 当 measurement ID 的生命周期 > measurement storage 的回收周期, 就一定有
> 测量数据被覆盖的风险。

隔离的办法只有两类:**扩大 storage 直到生命周期对齐**(环形数组扩容), 或
**用 ID 直接索引**, 让 storage 跟着 ID 走(hashmap)。--direct 数据(max_gap
达 33·qd)证明前者的"扩多少倍"很难静态选准, 实操上更稳的是后者。第三条
"修改被测系统让两者周期对齐"(强制 drain)是改变实验条件, 通常不算合法的
隔离手段。

### 第二条公式:机制贡献需要分开计费

io_uring 的两类收益(syscall 摊销 / 设备级并发)在不同工况下贡献完全不同——
cached 状态下 ~15×, direct 状态下 ~1×。**讨论 "X 比 Y 快多少" 时, 必须先
明确 IO 路径里哪一段是瓶颈, 否则给出的倍数没意义。**

工程上的对应判断:

- 测一个 IO 库的"性能", 至少要分别在 **CPU 路径主导** 和 **设备路径主导**
  两个工况测——cached vs direct 是最低成本的开关
- 看到某个工况下 uring vs sync 比值很小(比如 direct 的 1×), 不能 conclude
  "uring 没用", 只能 conclude "在这个瓶颈下两者打平"
- 真正能拉开 uring vs sync 差距的工况, 是 **IO 本身有真实延迟、且 sync
  单线程压不满设备** 的中间地带——比如机械盘、慢 SSD、网络 IO

### 面试场景里怎么用

两个判断公式映射到面试里, 形态都是"听到一个数字, 先问数字背后的瓶颈是什么":

**例 1:** 面试官问"你测出来 qd=32 的 p50 比单 op 真实延迟还低, 怎么解释?"
→ 答"先 Little's Law 对一下:p_avg = qd / throughput, 我看到的 p50 应该
接近这个值;如果显著偏低, 大概率是测量基础设施跟被测系统共享了状态", 然后
展开:
- 测量基础设施跟被测系统共享什么状态?
- 这个共享状态有没有被被测系统的状态机演化覆写的可能?
- 如果有, 怎么放大或抑制(改 qd / 改 IO 真实延迟)反向验证?

(这正是我们本项目实际经历的诊断路径:sync direct qd=32 报 p50=410 μs, Little's
Law 给 711 μs, 差 42% → 反查发现是 slot 复用覆盖 ts。)

**例 2:** 面试官问"你测出来 io_uring 比 read syscall 快 15×, 这数靠谱吗?"
→ 答"取决于 IO 路径里谁是瓶颈", 然后展开:
- 这 15× 是 cached 还是 direct 测出来的?
- 如果是 cached, 倍数来自 syscall 摊销, 换 direct 会塌
- 如果是 direct 还能拉开, 说明设备未饱和, 是真实并发收益

这套思路不限于 IO bench——profiler 的采样缓冲区 / metrics agent 的 ring buffer /
trace 系统的 span ID 复用, 都是公式 1 的变形;数据库 "B-tree vs LSM" / RPC
"gRPC vs HTTP/2" 这种比较, 都是公式 2 的变形(瓶颈不同, 倍数没意义)。

---

## 附:工作流程提示

### 信号显隐分级

- **第一层 bug(max=UINT64_MAX)**:形态明显, 看一眼就知道有事。这种 bug
  最好抓——出现就报。
- **第二层 bug(跨周期 slot 复用)**:没有异常形态, 出来的数字"合理但不
  可信"——但单纯靠"量级感觉不对"很容易被合理解释绕过(我们最初就把 sync
  qd=8 报 6 μs 错误归因为污染, 见"修复后回填")。真正稳定的抓手有两个:
  一是 **collision 率 / max_gap = qd 整数倍** 这种带数学结构的内建诊断指标,
  二是 **Little's Law 反推 p_avg = qd / throughput 与实测 p50 对账**——
  direct 模式下两者应该误差 < 5%, 偏差大就一定是测量被污染。
- **稀疏文件让 --direct 退化**:更难抓, 因为 --direct 标志开着、代码也对,
  只是文件本身没数据。这种 bug 需要**双重确认**——开了 --direct 之后
  throughput 和 latency 应该立刻往坏的方向变, 如果没变化, 一定是某层 cache
  / short-circuit 还在工作。

### 跨配置一致性检查是主力武器

这套 bench 里有三处"用一个配置反向 sanity check 另一个"的成功案例:

1. **Little's Law 对账**:p_avg = qd / throughput, direct 模式下 p50 跟它的
   误差应该 < 5%。修前 sync direct qd=32 p50=410 μs 比 711 μs 低 42% →
   推出测量被污染;修后 781 μs 跟 782 μs 误差 0.13% → 验证修复到位。这是
   全文最硬的工具, 因为它有量化的判定阈值。
2. **sync direct qd=1 是天然 ground truth**:qd=1 时 slot 没有复用空间, 任何
   实现方案下都是干净测量;它给出真实的 per-op 设备延迟(NVMe 4 K 随机读
   ~125 μs), 可以当所有更深 qd 配置的基准锚点。
3. **cached vs direct throughput 差异**:同一个后端开 --direct 后吞吐应该
   显著下降。稀疏文件那次发现差异接近零, 反向 catch 到 setup 错误。

注意一个反例:第一轮我们也曾试图用"sync qd=8 p50=6 μs 跟 uring 22 μs 不一致"
这种**纯量级感觉**做一致性检查, 得出了错误结论。**纯量级判断容易被合理解释
绕过, Little's Law 这种量化关系更可靠**——这是修复后回填补出来的教训。

**比 unit test 更管用**, 因为 measurement bug 通常通不过任何 assert——数据
都是合法范围内的, 只是物理上不合理。靠的是有量化阈值的物理关系(Little's Law、
ground truth 锚点), 而不是工程直觉。

### 诊断脚手架:结构性消除优于运行时检测

第一轮发现 bug 时我们加了 `[TEMP DIAG]` 块跟踪 collision, 当时一度想把它改
成 `--diag` 开关常驻保留。最后选择 unordered_map 修复方案后, **诊断块整个
被移除了**——这是更好的工程决策, 原因是:

- map by-id 方案**结构上消除了 collision 发生的可能**, slot 复用这条失败
  路径不存在了
- 留个永远报 0 的运行时计数器是死代码, 既增加维护成本, 又给未来读代码的人
  一种"这里还有动态行为要担心"的错觉
- 真要做健康度自检, 上面那条 Little's Law 对账更普适, 而且不依赖 bug 的
  具体形态

通用原则:**bug 应该首选 by-construction 消除, 不行才退到 by-observation 检测**。
诊断脚手架在 debug 阶段是脚手架, 不是产品。修完即拆。

(如果未来 bench 演化出 batched-submit-with-flush 之类的语义, slot 复用风险
可能以另一种形态回来, 那时再加诊断不迟。)
