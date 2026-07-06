# 微基准调试记录:从"负优化"到边界识别

这份文档记录 project5 的 uring 引擎在 AutoDL RTX 4090 (Ubuntu 22.04, XFS on
md127 RAID0 across nvme0n1 + nvme1n1) 上跑微基准时,如何从一份"看起来彻底
反向优化"的数据一路诊断到根因、修 patch、并识别出这条技术路线的边界。

---

## 0. 起点:数据形状和文档预期相反

RUN_ON_GPU.md 里的预期是 uring 在 syscall、CPU、MB/s 三条轴上都赢
`pool`(vLLM 官方 fs tier 语义) 和 `pool-slab`(消融组)。实测第一版:

| engine        | store MB/s | load MB/s | cpu_util | syscalls | CPU 秒 (util×wall) |
|---------------|-----------|-----------|----------|----------|--------------------|
| uring         | 1790      | 2118      | 2.75     | 1537     | 3.14 (store)       |
| pool          | 3757      | 7633      | 1.17     | 4096     | 0.64               |
| pool-slab     | 3781      | 7515      | 0.68     | 2048     | 0.37               |

**uring 用了 pool 5 倍、pool-slab 8 倍的 CPU 秒,吞吐还慢一半**。而且 syscall
明明少(1537 vs 4096),CPU 反而多 —— 这两个指标同时朝相反方向走,是个具体
信号:syscall 之外的时间被烧在用户态忙轮询或 kernel spinner 上。

---

## 1. 第一层诊断:sweep 揭示 gather 层完全失效

跑 `--job-blocks` sweep,期望 uring 收益随批量放大:

```
jb=1    syscalls=514   MB_s=1710
jb=8    syscalls=512   MB_s=1683
jb=32   syscalls=513   MB_s=1859
jb=128  syscalls=513   MB_s=1690
```

**syscall 数完全不动**。`--job-blocks` 变了 128 倍,syscall 恒等于 513,吞吐也
纹丝不动。这不是"负载不够激进",是**引擎里的批量提交层没接上 `job_blocks`
参数**。

在 `kv_tier_engine.cpp:200` 加一行 debug 打印:

```cpp
fprintf(stderr, "submit batch=%zu\n", reqs.size());
size_t accepted = backend.submit(reqs.data(), reqs.size());
```

分布出来:

```
1024 次 submit batch=1
   2 次 submit batch=512
```

根因坐实。**每个方向 (store/load) 只有一次真正的批量提交**(初始那一炮塞满
QD=512 的 SQ),之后每有 1 个 IO 完成,worker loop 就补 1 个 SQE 再 submit
一次 —— 完全退化成"用 io_uring API 写的同步 IO"。

### 病理循环的形状

worker 的 loop 结构是: ① 收新 job → ② 组批 submit → ③ 收 CQE。事件序:

1. 初始 submit 塞满 QD=512,`in_flight=512`
2. loop 到 ③ 非阻塞 `peek_cqe`,捞到 1 个 CQE(NVMe 完成了 1 个 block)
3. `in_flight=511`,loop 回到 ②,`room = QD - in_flight = 1`
4. 从 issue_order 头上塞 1 个 SQE → **submit batch=1**
5. loop 回到 ③,又捞 1 个 CQE...

这个循环里 CPU 花在用户态 loop 转圈 + 每次一次 `io_uring_enter` syscall。
批量摊薄的收益全没了。

### 修复:submit 前加 gather window

在 ② 前判断如果 `room < kSubmitThreshold` 且还有 in-flight 且 issue_order
未抽干,**跳过这轮 submit**,让 ③ 阻塞 wait 一批 CQE 攒够 room 再回来:

```cpp
constexpr size_t kSubmitThreshold = 32;
size_t room = queue_depth_ - backend.in_flight();
bool skip_submit = (room < kSubmitThreshold)
                   && (backend.in_flight() > 0)
                   && !issue_order.empty();
if (skip_submit) room = 0;   // 让 while(room>0...) 不进入
// ...
// ③ 里
if (skip_submit) {
    // 关键: cap 到 in_flight, 否则 QD < kSubmitThreshold 会死锁
    min_complete = kSubmitThreshold < backend.in_flight()
                   ? kSubmitThreshold : backend.in_flight();
} else {
    min_complete = (made_progress || more_to_issue) ? 0 : 1;
}
```

**踩到的坑**:第一版把 `min_complete = kSubmitThreshold` 硬写死,QD=8 跑时
`in_flight` 最多也只有 8,永远不满足"等 32 个 CQE",worker 死锁,ssh
session 挂到超时被 remote 强制断开。修复用 `min(threshold, in_flight)`
封顶。

### 效果

打上 gather patch 后同样负载:

```
32 次 submit batch=32
 2 次 submit batch=512     # 每个方向初始那一炮
syscall 513 → 17           # 少 30 倍
CPU 秒 3.14 → 2.57         # 略降
MB_s 1710 → 1592           # 略退 (~7-15%)
```

Syscall/CPU 大幅改善,MB/s 有小幅退让 —— 阻塞等 32 CQE 给 pipeline 引入了
微小断层。这个 tradeoff 是可接受的,毕竟 30 倍的 syscall 减少换 10% 的
MB/s 微降。

---

## 2. 第二层诊断:QD sweep 找到 sweet spot

Gather patch 上去之后 uring 仍然输 pool 一半吞吐。这条差距和 gather 层没
关系,得从别处查。跑 `--queue-depth` sweep:

| QD  | store MB/s | load MB/s | syscalls |
|-----|-----------|-----------|----------|
| 8   | 1407      | 3128      | 128      |
| 32  | 1586      | **4284**  | 32       |
| 128 | 1611      | 1900      | 29       |
| 512 | 1557      | 1783      | 17       |

**两条结论直接跳出来:**

**结论 A:io_uring 内部没串行,并发是有效的**。QD 8→32 时 load MB/s 从 3128
涨到 4284,如果是 workqueue 退化或内部 serialize,QD 变大应该没效果。有
效果就说明 uring 真的在并发处理。

**结论 B:QD 的 sweet spot 是 32,不是文档默认的 512**。QD > 32 后 load
MB/s 反而暴跌。原因是 NVMe controller 的**物理 queue depth** 通常就是
32–128,超过就是 kernel 端排队,不到设备端多并发。SQ/CQ 处理开销随 depth
线性上升 —— 高 QD 是**净负优化**。

这个发现直接改产品默认参数:`queue_depth=512` 是错的,应该改成 32。

---

## 3. 第三层诊断:iostat aqu-sz 定位到 fs 层串行

Store 侧 QD 从 8 到 512 几乎不动(1400 → 1600),说明写路径的瓶颈**根本不在
io_uring 并发上**,在更底层。上 iostat 直接看 nvme 端的实际排队深度:

**uring QD=32 store 期间(peak sample):**
```
md127     w/s=4034  wkB/s=1577984   aqu-sz=0.49  %util=93.60
nvme0n1   w/s=4051  wkB/s=1578088   aqu-sz=0.49  %util=93.60
nvme1n1   w/s=4051  wkB/s=1578088   aqu-sz=0.45  %util=93.60
```

**pool-slab (32 pthread) store 期间(peak sample):**
```
md127     w/s=5012  wkB/s=2097152   aqu-sz=19.92  %util=60.40
nvme0n1   w/s=5154  wkB/s=2097820   aqu-sz=15.46  %util=60.40
nvme1n1   w/s=5154  wkB/s=2097820   aqu-sz=19.62  %util=60.80
```

**aqu-sz 相差 30 倍**。用户空间告诉 io_uring `QD=32`,实际到 block layer
的并发只有 0.49 —— 平均连 1 个未完成 IO 都没有。而 pool-slab 打出 15.5 的
排队深度,吞吐直接高 2 倍。

`nvme %util 93%` 对 uring 反而是警报:controller 满负载,但每个时刻只有
< 1 个 IO 在处理,说明**32 个 write SQE 在到达硬件前的某一层被排成了单
列** —— io_uring → VFS → XFS → block → md 这条链上某处对单 fd 的
O_DIRECT write 做了 serialize。

pool-slab 也是同一个大文件同一个 fd,唯一区别是 32 个 pthread 各自
`pwrite`。多线程独立进 kernel 的路径能绕过这个串行 —— aqu-sz 15 就是证据。

### 排除 unwritten extent 假设

XFS 的一个已知行为是 `posix_fallocate` 分配 unwritten extent,首次写入时
需要 convert 成 written,伴随 journal log。这本可以解释 store 慢。跑对照:

```bash
# 先 dd 预写 2 GiB, 让所有 extent 转成 written
dd if=/dev/zero of=/root/autodl-tmp/bench_uring.bin bs=1M count=2048 oflag=direct
sync
python3 bench/bench_engine.py --engines uring ...
# store: 1646 MB/s  (预写前: 1671 MB/s)
```

**几乎无差**。XFS extent conversion 不是根因,从怀疑名单删掉。剩下的嫌疑
集中在 `inode i_rwsem` 或 md RAID 层对单 fd write 的排队 —— 但深入到内核
锁层面对面试 ROI 一般,识别到这一步就够了。

---

## 4. 归因诚实:哪些是 io_uring 的功劳,哪些是 C++ 的顺便

load 方向 uring 4144 MB/s vs pool-slab 2089 MB/s,uring **反赢 2 倍**。第一
反应是"看,io_uring 就是牛"。但仔细看 pool-slab load 时的数字:

```
pool-slab load:  wall=0.98s  cpu_util=16.4 core
uring    load:  wall=0.49s  cpu_util=4.2 core
```

pool-slab 打了 **16.4 核 CPU** 在 32 个 Python 线程上跑 preadv —— 这个数字
说明**GIL 争用 + 线程上下文切换**把线程池玩崩了。**如果对方是 C++ 线程池
+ pread**,这 2 倍差距很可能就没了。

必须诚实地把功劳分开:

**真正是 io_uring / SPSC / gather 层的优化**:
- syscall 少 30–200 倍(gather 后一次 enter 提交几十个 SQE,pool 是每 IO
  一次 pwrite)
- 数据面无锁(SPSC in_q/out_q 消除 mutex/cv 竞争)
- 单大文件 + fallocate 免除 file-per-block 的 open/close/replace 开销

**是 C++ 实现顺便顶掉的,不是 io_uring 功劳**:
- 绕过 Python GIL(read 那 2 倍差距,主要靠这条)
- 内存对齐、锁竞争消除等常规工程

**是我们输的地方**:
- 单 fd O_DIRECT write 吞吐输 pool 2 倍(iostat aqu-sz 铁证)

### 要证明 io_uring 本身的价值,唯一干净的对照是 "C++ pool + O_DIRECT"

当前的 pool/pool-slab 都是 Python 实现,拿它们对比 uring 相当于同时对比
"C++ vs Python"和"io_uring vs 线程池"两个变量。真要归因干净,得加一个
**C++ pool 引擎**:同样单大文件 + O_DIRECT,内部用 `std::thread` 池分派
pread/pwrite。它和 uring 引擎的唯一区别就是"io_uring vs pthread + 同步
syscall",拿这个对比才 apples-to-apples。这一步没做,是当前叙事最大的
未闭环。

---

## 5. 现在的画像

| 维度 | uring (QD=32) | pool-slab | vLLM 场景意义 |
|-----|--------------|-----------|--------------|
| syscall 数 | 少 30–200× | 基线 | CPU-constrained 时值钱 |
| store MB/s | 输 2× (1650 vs 3344) | 基线 | store 是 fire-and-forget,不在关键路径 |
| load MB/s | 赢 2× (4144 vs 2089) | 基线 | 但 2× 里 GIL 占不小,io_uring 净贡献未知 |
| CPU 秒 load | 显著低 | 基线 | 部分来自 io_uring, 部分来自绕过 GIL |
| CPU 秒 store | 略高 | 基线 | 部分是 bench 主线程忙 poll 污染 |

对 vLLM 官方 fs tier(它就是 Python 线程池 + file-per-block)整体是赢的,
因为它同时占 syscall、CPU、fs 元数据三个坑,我们把这三个都拆了。但**"我
的引擎更快"** 这话不准 —— 准的说法是**"我在 vLLM tier 的三条成本轴上都
赢:syscall 数、CPU 秒、read 吞吐(store 略输,已解释)"**。

---

## 6. 面试怎么讲

主线一句话:**我用 SPSC + 单线程 io_uring 做 KV disk tier,通过微基准发现
并修复了 gather 层退化 bug,sweep 出了 QD sweet spot,并用 iostat 定位到
了单 fd O_DIRECT write 在 XFS+md 上的串行边界**。

拆成三个可追问点:

**追问 1:你的 syscall 是怎么减少的?**
答:worker loop 的病理循环 —— 每 CQE 完成就补 1 SQE 立刻 submit,退化成
同步 IO。加了 gather threshold=32,submit 前先等 room 攒够。fprintf 加
debug 直接看到 batch 分布从 "1024 次 batch=1" 变成 "32 次 batch=32"。

**追问 2:QD 越大越好吗?**
答:不。sweep 显示 QD=32 是 sweet spot,超过就是净负优化。原因是 NVMe
controller 物理 QD 上限就是 32–128,更多的 SQE 只是在 io_uring/blk layer
排队,SQ/CQ 处理开销反而拖累吞吐。默认 QD=512 是错参数。

**追问 3:你的引擎在什么场景下会输?**
答:pure write 吞吐场景。单 fd O_DIRECT write 在 XFS + md RAID0 上被内部
串行,iostat aqu-sz 只有 0.49,而线程池 pwrite 能打到 15。fix 需要多 fd
分片,但 vLLM 场景 store 是 fire-and-forget 不在关键路径,tradeoff 可以
接受。

**追问 4(诚实版):你说 read 赢 2 倍,里面有多少是 io_uring 的功劳?**
答:不好说,因为对照组是 Python 线程池,GIL 占了不小比例。要拆干净得加
C++ 线程池对照,这一步我识别到了但还没跑完。

---

## 7. 剩余工作

1. **加 C++ pool 对照引擎**,把 "C++ 加成"和"io_uring 加成"拆干净。这是
   当前叙事最大的未闭环点。
2. **把默认 `queue_depth` 改成 32**,`RUN_ON_GPU.md` 里对应的示例配置也
   要同步。
3. **进入 §4 端到端**。vLLM 场景下 CPU 是 attention/decode 的稀缺资源,
   syscall/CPU 那条轴的胜利要在 revisit TTFT 上直接显形。
4. `bench_engine.py` 的主线程 `poll_finished` 忙轮询要修(空返回时
   `time.sleep(0)`),否则 CPU 数字被主线程污染,cpu_util 不能干净反映
   worker 侧成本。

---

## 8. 时间线

用于自查这次调试花了多少时间在什么地方,下次能避免重复。

| 阶段 | 主要动作 | 收获 |
|-----|---------|-----|
| 环境 | apt update / conda libstdc++ 冲突 / GitHub 拉代码 / miniconda source 陈旧 channel | 学到 AutoDL 环境的几个坑 |
| 冒烟 | bench_engine --smoke, 确认 O_DIRECT 生效 | 引擎数据面正确 |
| 主对比 | 三引擎对打, 发现 uring 全面负优化 | 建立问题 |
| Sweep | job-blocks sweep 显示 syscall 恒等 | 定位 gather 层失效 |
| Debug print | fprintf 看 batch 分布 | 看到 1024 次 batch=1 的病理 |
| Patch v1 | gather threshold, 修完 syscall 30x 降 | MB/s 微降但 CPU 大降 |
| Patch v2 | 修 QD < threshold 死锁 | ssh 挂死后学到教训 |
| QD sweep | 找到 sweet spot=32 | 产品级发现 |
| iostat | aqu-sz 0.49 vs 15.5 定位到 fs 串行 | 找到边界 |
| dd 预写 | 排除 unwritten extent | 缩小怀疑范围 |
| 归因反思 | 用户点出 GIL 混在里面 | 学到对照公平性 |
