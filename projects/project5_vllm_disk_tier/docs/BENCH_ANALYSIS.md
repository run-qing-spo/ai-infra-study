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

**【§5 后记】**:结论 B 的现象成立(默认 QD=32 仍是对的),但当时的解释
错了 —— 拖垮高 QD load 的不是 NVMe 物理队列深度(pool-slab 32 线程能打
7500 MB/s,设备远没到顶),而是 io-wq 的 worker 伸缩在大批量入队时坍缩:
QD=512 时全程只有 1 个 worker(iou_wrk 实测,见 §5)。结论 A 也只对了一
半:提交/完成路径确实没串行,但 punt 之后的执行路径在 io-wq 里被按 inode
串行 —— 串行恰恰在 io_uring 自己家里。

---

## 3. 第三层诊断:iostat aqu-sz 看到设备端串行

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
当时集中在 `inode i_rwsem` 或 md RAID 层对单 fd write 的排队。

**【§5 后记,销案】**:两个嫌疑人都不是。pool-slab 同 fd 多线程能打出
aqu-sz 15.5,这本身就证明 fs/md 层允许这个 inode 并发 write —— 串行只能
在 uring 独有的路径里。真凶是 io-wq 的 per-inode hash,分片实验(同
inode 多 ring 照样提速)把它钉死,见 §5。

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

**【§11 后记,已闭环】**cpp-pool 引擎(csrc/pool_tier_engine)在 dm 机
器上跑了两轮:GIL 税 ≈ 0(cpp-pool 和 Python 池 CPU 同档),uring 的
CPU 优势干净归因到提交模型本身。见 §11。

**【复测后记】**:后来带 iou_wrk 计数的复测里,"load 赢 2 倍"没有复现 ——
pool-slab load 两次稳定打出 ~7500 MB/s(1.3 核),uring 反输 1.8×;当初
2089 MB/s / 16.4 核那组疑似异常样本(pool-slab store 同配置也跑出过 3777
vs 2500 的方差)。GIL 归因的方法论保留,但"read 反赢"这条叙事作废,新画
像见 §6。

---

## 5. 第四层诊断:100% punt —— io_uring 退化成隐形线程池

§3 停在"fs 层某处串行"。但 pool-slab 的数据本身就藏着反驳:它写的是
**同一个大文件同一个 fd**,32 个 pthread 的 pwrite 能打出 aqu-sz 15.5 ——
fs/md 层明明允许这个 inode 并发 write。串行不可能发生在两条路径共用的
层,只能在 uring 独有的那段:io_uring 内部。

### 机制假设:punt + per-inode hash

io_uring 对每个 IO 先带 NOWAIT 标志尝试内联非阻塞下发;下面任何一层返回
-EAGAIN,就把请求 punt 给 io-wq 内核线程池走同步阻塞路径。而 io-wq 有条
规则:**regular file 的 write 按 inode hash,同一 inode 的 write 全部串到
同一个 worker 上顺序执行**(内核防止 buffered write 在 i_rwsem 上互踩的
设计);read 不 hash,可以摊到多个 worker 并行。

触发条件的怀疑落在内核版本上:这台机器是 Ubuntu 22.04 / 5.15 内核,而
**md 的 REQ_NOWAIT 支持 5.17 才进主线**。5.15 上带 NOWAIT 的 bio 打到
md127 直接被弹 -EAGAIN → 内联提交必败 → 100% punt。如果成立,单文件
write = 单 worker = aqu-sz ~0.5,全对上。

### 观测:iou_wrk 计数器

io-wq worker 不是别人的线程,是**本进程的线程**,`/proc/self/task/*/comm`
里叫 `iou-wrk-*`。给 bench 的 poll loop 加 10ms 限频采样取峰值(iou_wrk
列)。关键性质:**内联提交成功时这个数只能是 0** —— 非零就是 punt 的直接
目击。

| 配置 | store MB/s | store iou_wrk | load MB/s | load iou_wrk |
|--------|-----------|---------------|-----------|--------------|
| QD=512 | 1537 | 2 | 1788 | 1 |
| QD=32  | 1541 | 5 | 4091 | 5 |

三条信息:

1. **全程非零 → 100% punt 坐实**,io_uring 的"异步提交"在这台机器上
   名存实亡。
2. store 吞吐对 QD 和 worker 数完全不敏感 → 有效写并发就是 1。(计数含
   刚 spawn 和空闲待退的 worker;hash 只限制"同时在跑"的数量,所以 2、5
   不推翻单列。)
3. 意外收获:**QD=512 时 load 只有 1 个 worker、1788 MB/s** —— 高 QD 拖垮
   load 的真凶是 io-wq worker 伸缩在大批量入队时的坍缩,不是 §2 猜的
   NVMe 物理队列深度。

### 干预:分片实验

bench 加 `--shard-files N`(N 个引擎 = N 个 ring + N 个 worker 线程,各写
各的文件)和 `--shard-same-file` 对照组(N 个 ring 写**同一个文件**,同
inode)。QD=32:

| 配置 | store MB/s | load MB/s | iou_wrk |
|------|-----------|-----------|---------|
| 1 引擎(基线) | 1399 | 4248 | 5 |
| 4 引擎 × 4 文件 | 3206 | 5638 | 23 |
| 4 引擎 × 同一文件 | 3546 | 5484 | 25 |

两个结论:

- **分片能修**:store 2.3×,逼近 pool 的 3714(≈盘在这个负载下的能力)。
- **same-file 和 multi-file 一样快**:同一个 inode 被 4 个 ring 并发写照样
  翻倍 → 串行**不在 fs/inode 层**,只在单个 io-wq 内部的 hash 链上(每个
  ring 有自己的 io-wq,互不共享 hash 链)。§3 的嫌疑人正式销案。

加上 `uname -r` = 5.15.0-94 实测,证据链闭环:**md 无 REQ_NOWAIT(<5.17)
→ NOWAIT bio 被弹回 → 100% punt(iou_wrk 非零)→ io-wq per-inode hash 把
write 串成单列(same-file 分片实验)**。

### 残酷的部分:CPU 卖点在这个环境不成立

shard4 load 打了 **12.7 核**,pool-slab 只要 1.35 核。punt 意味着每个 IO
由一个内核线程同步阻塞执行 —— io_uring 在这台机器上就是一个**隐形线程
池**:worker 数不受你控制,write 还被 hash 串行。syscall 少(64 vs 2048)
的优势还在,但"少 syscall 省 CPU"的逻辑链断了:CPU 没有省,只是从用户态
线程转移到了内核 worker 线程,总量还更大。

io_uring 的价值前提是 **NOWAIT 内联提交成功**。满足它要么 (a) 不经过 md
的设备,要么 (b) ≥5.17 内核。AutoDL 容器共享宿主内核,(b) 不可行 ——
(a) 是下一步的翻身仗。

---

## 6. 现在的画像

复测 + 分片后的完整画像(QD=32,run-to-run 方差见下):

| 维度 | uring 单文件 | uring shard4 | pool-slab | 说明 |
|-----|-------------|--------------|-----------|------|
| syscall 数 | 64 | 64 | 2048 | 唯一稳赢的轴 |
| store MB/s | 1400–1540 | 3206–3546 | 2500–3777 | 单文件输 ~2.4×;分片追平 |
| load MB/s | 4090–4250 | 5480–5640 | ~7500 | 都输;"赢 2 倍"未复现 |
| CPU(load) | 4.4–4.8 核 | 12.7 核 | 1.3 核 | punt 让 CPU 轴整体输 |

对 vLLM 官方 fs tier(pool,file-per-block)仍有 syscall 和 fs 元数据两条
赢面,但在 md + 5.15 的环境里,"单线程 io_uring 省 CPU"这条核心卖点不成
立。准确的说法从"我的引擎更快"变成:**"我能说清楚它为什么慢、慢在哪一
层、什么环境下会翻盘 —— 且观测(iou_wrk)和干预(分片)两组实验互相咬
合"**。

另:pool-slab store 同配置跑出过 3777 和 2500,这台机器 run-to-run 方差
不小,单次数字都要打折,结论只建立在多次复现的形状上。

**【§10.6 后记,口径警告】**本表 uring 的 MB/s 和 CPU 差距倍数含 bench
自伤成分:uring 排引擎列表第一个,替全场付了缓冲区 first-touch 缺页税,
且主线程忙轮询恒计 +1 核。punt/io-wq 的定性结论不受影响(iou_wrk 证据
独立),但差距倍数被夸大,AutoDL 上欠一轮修复后复测。详见 §10.6。

---

## 7. 面试怎么讲

主线一句话:**我做了个 io_uring KV disk tier,微基准全面负优化;我用
iou-wrk 线程计数和分片实验把根因钉死到"5.15 内核的 md 不支持 REQ_NOWAIT
→ 100% punt 到 io-wq → write 被 per-inode hash 串成单列",由此识别出
io_uring 的适用边界**。

**追问 1:你的 syscall 是怎么减少的?**
答:worker loop 的病理循环 —— 每 CQE 完成就补 1 SQE 立刻 submit,退化成
同步 IO。加了 gather threshold=32,submit 前先等 room 攒够。fprintf 加
debug 直接看到 batch 分布从 "1024 次 batch=1" 变成 "32 次 batch=32"。

**追问 2:QD 越大越好吗?**
答:不,QD=32 是 sweet spot。但注意解释:不是 NVMe 物理队列深度(那是我
第一版的错误归因),是高 QD 大批量入队时 io-wq 只 spawn 出 1 个 worker,
并行度坍缩在内核线程池的伸缩逻辑里 —— iou_wrk 计数直接看到 QD=512 时
load 全程只有 1 个 worker。

**追问 3:你的引擎为什么输?**
答:环境把 io_uring 的前提抽掉了。它的收益依赖 NOWAIT 内联提交成功;md
在 5.17 之前不支持 REQ_NOWAIT,所有 IO 100% punt 到 io-wq,write 再被
per-inode hash 串行。证据链三环:iou_wrk 全程非零(punt 目击)、store 吞
吐对 QD/worker 数不敏感(单列)、同 inode 多 ring 分片照样提速(排除 fs
层)。

**追问 4:怎么修?**
答:≥5.17 内核(容器环境做不到)、绕开 md、或多文件/多 ring 分片(实测
store 2.3×,逼近盘上限)。但 vLLM 场景 store 是 fire-and-forget 不在关键
路径,是否值得为它加分片复杂度要看端到端数据。

**追问 5(诚实版):那 io_uring 在这个项目里还剩什么价值?**
答:在这台机器上数据面价值基本归零 —— punt 让它退化成隐形线程池,CPU 反
而更贵。剩下的是 syscall 轴和方法论:部署 io_uring 前要冒烟检查 iou-wrk
线程数,非零说明拿到的不是异步 IO。

**【§10 后记】**翻身实验已跑,一波三折:iou_wrk 如预期归零,但单 ring
吞吐起初没翻身 —— 直到把 bench 的两处自伤(主线程忙轮询、first-touch
缺页记在先跑引擎头上)修掉,单线程单 ring 打满盘,CPU 一半、syscall
1%,三条轴全赢。完整故事见 §10(尤其 §10.6 的归因修正)。C++ pool 对照
(拆 GIL 加成)仍欠着,但"io_uring 加成"本身已经干净显形。
(→ 后来也补上了:§11,GIL 税 ≈ 0。)

---

## 8. 剩余工作

1. **非 md 单盘复测** —— io_uring 的翻身仗。预测:iou_wrk=0、cpu_util 骤
   降、store 追平 pool。这个实验**不需要 GPU**,路径按成本排:
   - 当前机器先试系统盘(`lsblk` + `df -T /`,/ 是 overlayfs 的话看
     O_DIRECT 能否穿透,引擎 open 会 fail fast);
   - AutoDL 换宿主:镜像版本无关,内核跟宿主机走;用**无卡模式**
     (~0.1 元/时)开机 → `uname -r` + `lsblk` 筛 ≥5.17 且数据盘非 md
     的宿主,不合格就释放换地区/机型;CPU 型号与内核无关,只是越新
     机型装机越晚、内核可能越新的弱启发;
   - 或者几块钱的抢占式云主机(阿里/腾讯 spot、AWS i3/i4i),Ubuntu
     24.04(6.8 内核)+ 本地 NVMe,最干净的对照。

   **【§10 后记,已完成】**在自有实体机(5.15 + LVM/dm-linear)上跑了:
   iou_wrk=0 达成、io-wq 的 CPU 消失;store 起初没追平 pool,查出来是
   bench 自伤(first-touch 缺页,§10.6),修掉之后单 ring 打满盘 ——
   预测最终成立,且比预测更好(load 也赢了 CPU 轴)。见 §10。
2. **加 C++ pool 对照引擎**,把 "C++ 加成"和"io_uring 加成"拆干净(GIL
   归因仍未闭环)。

   **【§11 后记,已完成】**cpp-pool 引擎落地,两轮复跑:GIL 税 ≈ 0,
   uring 的 CPU/syscall 优势归提交模型。另:AutoDL(md 机)修复后复测
   **决定不做**(md+5.15 宿主难找;punt 定性结论有 iou_wrk 独立证据,
   只需不再引用 §0/§6 的旧差距倍数)。
3. **把默认 `queue_depth` 改成 32**,`RUN_ON_GPU.md` 里对应的示例配置也
   要同步;同时 gather threshold 要和 QD 联动 —— threshold=32 撞上 QD=32
   会退化成"提交 32 → 等完 32"的 lockstep,改成 min(32, QD/2) 之类再
   sweep 验证。

   **【§10 后记】**dm 机器上 QD 32→256 吞吐完全平坦,lockstep 在这台机器
   上未显形(瓶颈在提交线程 CPU,轮不到队形问题出场)。联动修改仍值得做,
   优先级降。

   **【已完成】**threshold 改为引擎内随 QD 联动 `max(1, min(32, QD/2))`
   (kv_tier_engine.cpp worker_loop 开头);默认 `queue_depth` 512→32 同
   步到 pybind / manager.py / bench `--queue-depth` / RUN_ON_GPU.md 四处。
   联动后 QD=32 时等 16 补 16,任何时刻盘里至少半数 IO 在飞。验证:dm 机
   QD=32(新默认)一轮,吞吐/CPU 应与 QD=64 一致(之前 QD=32 的旧数字带
   lockstep,不可比)。
4. 若要在 md 环境部署:**引擎内建多文件分片**(bench 层 N 引擎只是探针,
   正式做应该是单引擎多 fd,或多 ring)。
5. **进入 §4 端到端**。vLLM 场景下 CPU 是 attention/decode 的稀缺资源,
   syscall 那条轴的胜利要在 revisit TTFT 上直接显形 —— 但 CPU 轴的故事
   要按 §5 的结论重讲。

   **【§13 后记,已完成】**2026-07-10/11 在 §12 同宿主上四组全绿:
   revisit TTFT 650ms → 123ms(5.3×),uring 对官方 fs tier mean -11% /
   p99 -8%,且 CPU 记账位置的差异在 pidstat 里直接显形。见 §13。
6. `bench_engine.py` 的主线程 `poll_finished` 忙轮询要修(空返回时
   `time.sleep(0)`),否则 CPU 数字被主线程污染,cpu_util 不能干净反映
   worker 侧成本。

   **【§10 后记,已修】**空轮改成真睡 0.5ms(sleep(0) 只让 GIL 不省
   CPU);同时 make_region 加了整块 pre-touch(§10.3 发现 first-touch
   缺页被记进 worker 的提交路径)。修复后老数字的 cpu_util 口径作废。

---

## 9. 时间线

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
| 复测主对比 | 带 iou_wrk 列重跑三引擎 | "load 赢 2 倍"未复现;发现 run-to-run 方差 |
| iou_wrk 计数 | /proc/self/task 里数 iou-wrk 峰值 | punt 直接目击;QD512 load 仅 1 worker, 推翻 NVMe 归因 |
| 分片实验 | --shard-files 4 ± --shard-same-file | same-file 一样快 → 销案 fs 层, 真凶 io-wq hash |
| 内核版本 | uname -r = 5.15, md REQ_NOWAIT 5.17 才合入 | punt 触发条件钉死 |
| 翻身实验 | 自有机 5.15+dm-linear, 复用 iou_wrk 探针 | load iou_wrk=0, punt 消失; store 首写仍 punt → 钉死 fallocate unwritten extent 是 fs 层第二 punt 源 |
| QD sweep(二次) | dm 机器 QD 64–256 吞吐平坦 | lockstep 嫌疑排除; syscall 32→5 继续降 |
| 分片(二次) | shard4 ± same-file 提速且重合 | "无 punt 分片失效"预测证伪 → 第二串行点在单 worker 线程 |
| perf 归因 | 按 tid 拆线程, GUP/缺页链贯穿 io_submit_sqes | 内联提交的 pin 页成本归提交线程; 处方 registered buffers |
| 修复后复测 | pre-touch + 空轮真睡, 重跑三引擎 | 单 ring 打满盘、0.2 核; "稳态 pin 页"归因修正为 first-touch artifact + 引擎顺序不公平 |
| 分片(三次) | shard4 与单 ring 完全重合 | 反常消失, 因果链闭环; registered buffers 降级为 CPU 微优化 |
| C++ pool 对照 | csrc/pool_tier_engine + bench cpp-pool, 两轮复跑 | GIL 税≈0; uring 的 CPU/syscall 优势干净归因到提交模型(§4 最后一环闭合) |
| md 复测(白捡) | 筛 e2e 宿主撞上 md RAID1+企业盘, 修复后 bench 四引擎 | punt 仍在(iou_wrk 5/6)但三轴全赢; "iou_wrk 非零=不可用"判据被推翻, 警报≠判决 |

---

## 10. 翻身实验:非 md 单盘 (5.15 + dm-linear)

2026-07-09,自有实体机(无 GPU):Ubuntu 20.04 HWE 内核 **5.15.0-119**,
单块消费级 NVMe 238G,ext4 on **LVM(dm-linear)**。关键性质:dm-linear
自 5.9 起支持 REQ_NOWAIT 直通,md 要 5.17 —— 和 AutoDL 机器**同为
5.15**,唯一变量就是 md vs dm,正好把"真凶是 md 缺 NOWAIT、不是内核版本
本身"钉死。盘的物理上限:dd O_DIRECT 顺序写 2.2 GB/s,读 ~2.9–3.0 GB/s
(由下面 shard4 load 摸到)。绝对 MB/s 与 AutoDL 的 RAID0 没有可比性,
本节只看形状。

### 10.1 punt 如预期消失,但冒出第二个 punt 源

QD=32 三引擎主对比(1024 × 1MB):

| engine | store MB/s | load MB/s | cpu_util(load) | iou_wrk |
|--------|-----------|-----------|----------------|---------|
| uring | 1585 | 1740 | 1.97 | store **6** / load **0** |
| pool | 2197 | 2802 | 0.36 | - |
| pool-slab | 1974 | 2674 | 0.47 | - |

load iou_wrk=0:NOWAIT 内联提交成功,punt 目击消失。CPU 从 md 机器的
4.4–4.8 核降到 1.97 核 —— 消失的 ~2.5 核就是 io-wq worker 的成本;剩下
~2 核是 bench 自伤(主线程忙轮询 + worker 自旋,见 10.3/10.4)。

store iou_wrk=6 是 **fs 层的第二个 punt 源**:引擎 posix_fallocate 预分
配出的是 unwritten extent,首写要做 extent 转换,这一步 NOWAIT 路径做不
了 → -EAGAIN → punt。dd 预写文件后复测,store iou_wrk 6→0 —— §3 时代
dd 预写实验怀疑过的因素,这次拿到直接读数。注意 bench 在 load 后会
unlink 文件,预写必须每次跑之前重做。

### 10.2 预测证伪:没有 punt,分片照样提速

预写后(iou_wrk 全程 0):

| 配置 | store MB/s | load MB/s | 说明 |
|-----|-----------|-----------|------|
| 单 ring QD=32 | 1623 | 1750 | |
| 单 ring QD=64/128/256 | 1663–1688 | 1742–1765 | QD 不敏感; syscall 32→5 |
| shard4 多文件 QD=64 | 2279 | 2953 | 双向逼近盘上限 |
| shard4 **同文件** QD=64 | 2294 | 2968 | 与多文件重合 |

原预测"无 punt 环境分片失效"**被证伪**:分片仍提速 ~35–70%,load 直接
摸到盘的读上限。iou_wrk=0 说明这次提速与 io-wq 无关 —— 存在第二个串行
点。同文件与多文件重合 → 再次销案 fs/inode 层,串行点在 **per-engine
的单 worker 线程**。QD 32→256 吞吐纹丝不动,顺带排除 §8.3 担心的
lockstep 是 load 慢的原因。

### 10.3 perf 归因:内联世界里,提交 CPU 归你自己

perf record 整个 bench(blocks 加到 2048,wall ~1.2s/方向),按 tid 拆:

- 主线程 58%:`_PyEval_EvalFrameDefault` 忙轮询,外加退出时 munmap 拆
  2GB 页表占 15% —— 全是 bench 自伤;
- store/load worker 各 ~20%,热点调用链一条贯穿:
  `io_uring_enter → io_submit_sqes → ext4_dio_write_iter → iomap_dio →
  bio_iov_iter_get_pages → get_user_pages_fast → 缺页 → shmem 页分配 +
  clear_page_erms`。

这条链同时证明两件事:提交是**内联**的(全程不见 io-wq,与 iou_wrk=0
互相印证);pin 页的成本发生在 worker 线程自己身上。其中缺页+清零部分
(约占 worker 的 20–25%)是 bench artifact —— make_region 只摸了每个
1MB block 的第 1 页,其余 255 页留给 GUP 首触(已修,见 10.4);稳态
成本是每 IO get_user_pages_fast pin 256 页 + bio 构造,摊在长尾里。

两个世界的机制画像(与 §5 对称):

- **punt 世界(md + 5.15)**:NOWAIT 被拒 → io-wq 接管。写被 per-inode
  hash 串成单列(store 慢);读不做 hash,punt 反而**免费送了多 worker
  并行提交**(md 机器单文件 load 有 4.1–4.3 GB/s 的真正原因)—— 代价
  是 CPU 爆炸。
- **内联世界(dm/单盘)**:NOWAIT 成功 → io-wq 成本归零,但 bio 构造、
  pin 页、DMA 映射全部同步落在提交线程上,单 worker 单核把单 ring 卡在
  ~1700 MB/s。

同一个分片实验在两台机器上都提速,但机制完全不同 —— md 上是绕开 io-wq
hash,dm 上是给提交路径加核。处方也不同:punt 世界要绕开 md 或升内核;
内联世界的对症药是 **registered buffers**(`io_uring_register_buffers`
+ `READ_FIXED/WRITE_FIXED`,注册时 pin 一次,每 IO 免 GUP)—— KV tier
的内存池本来就是一整块长期存活的 slab,天生适合一次性注册。其次才是
多 ring 分片。

**【§10.6 后记,归因修正一半】**"稳态成本是每 IO pin 256 页"被修复后的
复测推翻了大半:pre-touch 之后单 ring 直接打满盘,只用 0.14–0.21 核。
串行点确实在单 worker 线程,但被串行的主体是 first-touch 缺页这个
artifact,不是稳态提交成本;registered buffers 的处方随之降级。见 §10.6。

### 10.4 bench 卫生修复(随本节提交)

1. `make_region` 分配后整块 pre-touch,把 first-touch 缺页赶出计时窗口
   (10.3 的 artifact);
2. `run_uring` 主线程 poll loop 空轮真睡 0.5ms(§8.6 旧账)。

修复后 cpu_util 口径变了,前文所有 cpu_util 数字不能和修复后的新数字
直接对表;修复后需在两台机器各复测一轮基线。dm 机器的复测见 §10.6,
AutoDL(md 机)的还欠着。

### 10.5 面试版一句话

"我在同内核版本(5.15)的 md 和 dm 机器上做了对照:md 上 iou_wrk 非零、
分片提速是因为绕开 io-wq hash;dm 上 iou_wrk 归零、分片**仍然**提速 ——
perf 显示这次串行在单 worker 线程的内联提交路径(每 IO pin 256 页)。
io_uring 不是免费的异步:punt 世界里你付 io-wq 的 CPU 税,内联世界里
提交 CPU 归你自己,registered buffers 是后者的对症药。"

**【§10.6 后记】**这版说法只对了一半("内联世界提交 CPU 归你自己"成
立,但当时量出来的单核瓶颈主要是 bench artifact),修正版见 §10.7。

### 10.6 修复后复测:真正的翻身,和第三次归因修正

§10.4 的两处修复落地后,同机器同配置复测(QD=64,1024 × 1MB,预写):

| engine | store MB/s | load MB/s | cpu_util (st/ld) | syscalls |
|--------|-----------|-----------|------------------|----------|
| uring 单 ring | **2288** | **3000** | **0.14 / 0.21** | 28 / 26 |
| pool | 2229 | 3015 | 0.40 / 0.35 | 2048 |
| pool-slab | 1850 | 2684 | 0.42 / 0.49 | 1024 |
| uring shard4 | 2288 | 3016 | 0.16 / 0.21 | 28 |

三个事实:

1. **单 ring 打满盘**:store 2288 ≈ dd 写上限 2.2 GB/s,load 3000 ≈ 读
   上限;MB/s 与 pool 打平(都是盘瓶颈),CPU 少 2–2.5×,syscall 少
   ~75×。这是 RUN_ON_GPU.md 最初预期的形状,第一次真实出现 —— 单线程
   0.2 个核喂满一块 NVMe,就是 io_uring 该有的样子。
2. **分片不再提速**(与单 ring 完全重合)。§10.2 的反常随 artifact 一起
   消失:当初分片"有效",只是把缺页账单分给 4 个线程去付。因果链至此
   闭环。
3. **§10.3 的归因修正一半**:串行点确实在单 worker 线程,但被串行的主体
   是 first-touch 缺页,不是稳态 pin 页。每页的缺页路径(进 fault
   handler、分配、清零 4K、建页表)是微秒级,GUP 一个已驻留的页只要百纳
   秒级,差两个数量级。perf 采样显示缺页只占 worker ~25%,但干预实验
   (pre-touch)给出的真实份额是"从 1700 到打满盘的全部差距" —— 采样
   比例会骗人(成本摊在 percent-limit 之下的长尾符号里),观测只配提出
   假设,份额要靠干预来定。

还有一个藏得更深的对照不公平:make_region 每个方向只建一次缓冲区,三个
引擎**共用**,而 uring 排在引擎列表第一个 —— 旧 bench 里 **uring 替所有
引擎付了全部缺页税,pool 拿到的是摸热的缓冲区**。对照组共享可变状态时,
运行顺序本身就是混杂变量。

连带修正两条:

- **registered buffers 降级**:它治"每 IO pin 页打满单核",现在单核只用
  0.2,没有吞吐病可治。仍可作为 CPU 微优化(驻留页 GUP 的百纳秒也省掉,
  对 vLLM 场景省 CPU 仍有意义),排到端到端之后。
- **md 机器的旧数字要打折**:§0/§6 里 uring 输的幅度含同样的 artifact
  (uring 先跑替全场缺页 + 忙轮询计入 CPU)。punt 的定性结论不受影响
  (iou_wrk 证据独立),但差距倍数被夸大,AutoDL 上欠一轮修复后复测。

至此,"uring 全面负优化"完整拆解成三层:**一层环境**(md 无 REQ_NOWAIT
→ 100% punt → io-wq 串行),**两层 bench 自伤**(主线程忙轮询污染 CPU
轴;first-touch 缺页 + 引擎顺序不公平污染 MB/s 轴)。三层全部剥掉之后,
io_uring 在它的适用环境里兑现了全部承诺:同吞吐、一半 CPU、1% 的
syscall。

### 10.7 一句话(修正版)

"我的 io_uring 引擎微基准输给线程池,我把'输'拆成了三层:一层环境 ——
5.15 的 md 不透传 REQ_NOWAIT,100% punt 进 io-wq 被 per-inode hash 串行,
iou_wrk 计数直接目击;两层 benchmark 自伤 —— 主线程忙轮询污染 CPU 数字,
匿名页 first-touch 缺页全记在先跑的引擎头上(perf 在 io_submit_sqes 下面
抓到 shmem_fault)。逐层剥掉后,单线程单 ring 用 0.2 个核打满 NVMe,CPU
是线程池的一半、syscall 是 1%。这个过程我错归因过三次:NVMe 队列深度、
fs 层串行、稳态 pin 页成本 —— 每次都是干预实验(分片/预写/pre-touch)把
观测假设修正过来的,perf 采样比例骗过我一次。"

---

## 11. C++ pool 对照:GIL 税 ≈ 0,uring 的赢面归提交模型

§4 留下的最后一环:pool/pool-slab 是 Python 实现,uring 赢的 CPU 轴里
可能混着"绕开 GIL"的语言加成。2026-07-09 补上 cpp-pool 对照引擎
(csrc/pool_tier_engine):和 KvTierEngine 共用同一套 JobDesc/JobResult/
stats、同样单大文件 + fallocate + O_DIRECT、bench 侧走同一条 submit/poll
代码路径,唯一差异是 worker 侧换成 32 个 `std::thread` 各自同步
pread/pwrite。对照关系:pool-slab vs cpp-pool 的差 = GIL/Python;
cpp-pool vs uring 的差 = 提交模型,这才是 apples-to-apples。

dm 机器(§10 同一台),QD=64 / 32 线程,1024 × 1MB,预写,跑两轮取区间:

| engine | store MB/s | load MB/s | cpu_util st/ld(两轮区间) | syscalls |
|--------|-----------|-----------|---------------------------|----------|
| uring 单 ring | 2285–2288 | 3004–3005 | 0.11–0.13 / 0.21 | 27–29 |
| cpp-pool | 2284–2289 | 2893–2929 | 0.26–0.44 / 0.46–0.67 | 1024(真实计数) |
| pool | 2224–2238 | 2984–3025 | 0.37–0.40 / 0.36–0.39 | 2048(估算) |
| pool-slab | 1703–1899 | 2673–2677 | 0.43 / 0.38–0.57 | 1024(估算) |

三条结论:

1. **GIL 税 ≈ 0**:cpp-pool 和两个 Python 池挤在同一档(0.3–0.5 核),
   把线程池从 Python 换成 C++ 根本不省 CPU。原因:1MB 块下每个任务就是
   一次立刻释放 GIL 的大 syscall,解释器侧工作量(一次函数调用 + 一个
   memoryview 切片)相对每 IO 几百微秒的内核路径可忽略。第一轮看到
   cpp-pool load CPU 比 pool 高还愣了一下 —— "去 GIL 必有收益"这个默认
   本身就是错的:线程池的 CPU 大头是内核侧每 IO 的提交+睡眠+唤醒+上下文
   切换,语言换了这些一分不少。
2. **uring 的 2–3× CPU、37× syscall 优势因此干净归因到提交模型**:
   uring 0.11–0.21 核 vs cpp-pool 0.26–0.67 核,同为 C++、同文件布局、
   同 O_DIRECT,唯一变量就是"单线程批量 io_uring vs 每线程同步 syscall"。
   §4 的担心("read 赢 2 倍可能全靠 GIL")正式解除。
3. **线程池之间分不出稳定高下,别过度解读**:pool-slab load CPU 两轮
   0.57 → 0.38,单轮数字噪音就有 50%;cpp-pool load 略高于 pool 的部分
   有已知的 bench 侧不对称(cpp-pool 走 uring 的 poll loop:0.5ms 轮询
   + 每 10ms 扫 /proc 数 iou-wrk,约 0.05 核;pool 的主线程在 executor
   里睡死),残余在方差内,按"小差距不追、份额靠干预定"的原则封存。
   另:pool-slab store 稳定偏低(1700–1900),嫌疑是 ftruncate 稀疏文件
   每轮重付 extent 分配 —— 未验证,不追。

MB/s 全员顶在盘上限附近(store ~2.2 GB/s、load ~2.9–3.0 GB/s),无区分
度 —— 盘瓶颈下 CPU 和 syscall 才是有信息量的轴,这正是 vLLM 场景想要的
形状:同样喂满盘,谁给 attention/decode 留的 CPU 多。

### 面试版一句话

"有人质疑我的 uring 引擎赢线程池是 C++ vs Python 的语言差,我就补了一个
同构的 C++ 线程池对照:结果 C++ 化根本不省 CPU —— 1MB 块的负载下每个任
务就是一次立刻放 GIL 的大 syscall,GIL 收不到税,线程池的 CPU 账单其实
是内核侧每 IO 的提交和唤醒路径。io_uring 的 2–3 倍 CPU、37 倍 syscall
优势因此干净归因到提交模型本身。顺带学到:'去 GIL 必有收益'是个想当然
的默认,对照实验做出来才知道税基根本不在那里。"

---

## 12. md 复测白捡:punt 还在,伤害没了

2026-07-09/10,给 §4 端到端筛 AutoDL 宿主时撞上另一台 md 机:5.15.0-97,
XFS on **md0 = RAID1 × 两块 7TB 企业级 NVMe**(老机器是 RAID0 × 两块
小盘)。按 §5 的判据这又是 punt 世界 —— 本来只想用 iou_wrk 确认一下就
释放换机,结果顺手跑的四引擎对比(修复后 bench + cpp-pool + QD=32 新
默认/threshold=16 联动首跑)把 §10.6 欠的"md 修复后复测"白捡了回来,
还推翻了一个判据:

| engine | store MB/s | load MB/s | cpu_util st/ld | syscalls | iou_wrk |
|--------|-----------|-----------|----------------|----------|---------|
| uring QD=32 | **3617** | 7270 | **0.38 / 0.65** | 63 | **5 / 6** |
| cpp-pool | 3422 | 7545 | 0.63 / 1.01 | 1024 | 0 |
| pool | 3534 | 6988 | 1.29 / 1.45 | 2048 | - |
| pool-slab | 3589 | 6973 | 0.93 / 1.26 | 1024 | - |

**punt 确认在发生(iou_wrk 5/6 全程非零),但 uring 三轴全赢**:store
全场最快、load 与 cpp-pool 持平、CPU 全场最低(池组的 1.6–2.2×)、
syscall 1/16。§0 的"CPU 5–8 倍、吞吐一半"在这台 md 机上完全没有重演。

两层含义:

1. **§10.6 欠的"旧数字要打折"拿到实测**:修掉两层 bench 自伤后,md 机
   上的 uring 不再是负优化。注意硬件不同(RAID1+企业盘 vs RAID0+小盘),
   老机器 store 串行卡 1.5 GB/s 有 iostat aqu-sz 铁证,是真伤 —— 所以
   两台机器的数据都对,错的是"md ⇒ 惨"这个推广。
2. **punt 是机制不是判决,iou_wrk 非零只是警报**。punt 的实际伤害是两
   笔账:(a) write 的 per-inode hash 串行 —— 伤害取决于盘的单 IO 延
   迟,企业盘 1MB 写 ~0.3ms 量级(掉电保护写缓存),串行单列也能打出
   3.5+ GB/s 顶到阵列上限(四引擎 store 全挤在 3.4–3.6,盘瓶颈);
   (b) io-wq 的 CPU 税 —— 5–6 个 kernel worker 远比 32 个用户线程便
   宜,punt 路径反而全场 CPU 最低。§5 末尾"CPU 卖点在 punt 环境不成
   立"要收窄成:在**那台** RAID0 机器上不成立;punt 本身的固定成本没
   那么大,组合拳(punt × 高延迟盘)才致命。

未钉死的尾巴(对决策不关键,记录备查):这台机器上 store 是否仍被
hash 串到单 worker 没有直接证据 —— 3617 高于 dd 单线程的 2.4 GB/s,
但 dd 计时含 fsync 不可比;要钉死得上 iostat aqu-sz 或 shard 对照。
另 QD=32 + threshold=16 联动的 syscall 从旧版的 ~32 变 63(batch 16),
换掉 lockstep,可接受。

**结论:这台机器直接可用于 §4 端到端**,C2 预期按正常剧本看(同吞吐、
更低 CPU、syscall 1/16),不必再抽卡换宿主。

### 面试版一句话

"我本来把 iou_wrk 非零当'不可部署'的判据,复测教育了我:punt 是机制,
不是判决。同样 100% punt,RAID0+小盘上 write 串行卡一半吞吐、io-wq 吃
数倍 CPU;RAID1+低延迟企业盘上串行单列照样打满阵列,5 个 kernel worker
比 32 个用户线程还省 CPU。部署判据从'看 punt 与否'改成'看 punt 之后
的对照数字'——警报和判决是两回事。"

---

## 13. 端到端闭环:三层缓存的 TTFT 直接显形

2026-07-10/11,§12 同宿主(4090 / 5.15.0-97 / XFS on md0 = RAID1 × 两块
7TB 企业级 NVMe),vLLM 0.24.0 + Qwen2.5-7B-Instruct bf16,
`--max-model-len 16384`。负载 = `bench/long_context_ttft.py`:16 会话
× 6000 词前缀 prime → 24 个一次性请求 churn(把前缀从 GPU/CPU 逐级挤
出去)→ 16 会话 revisit。CPU tier 故意只给 4 GiB:这个模型每 token 的
KV ≈ 56 KB(28 层 × K/V × 4 kv-head × 128 dim × bf16),一个 8k 前缀
≈ 440 MB,16 个会话 ≈ 7 GB,必然溢出到盘 —— 踩不到盘的配置量出来全是
安慰剂(§4.1 的设计初衷)。四组由 `bench/run_e2e_overnight.sh` 一次跑
完,全程 9 分钟;原始数据 `results_e2e_20260711_0058/`。

| 组 | 配置 | revisit mean | p50 | p99 | revisit/prime |
|----|------|-------------|-----|-----|---------------|
| A  | 无 offload | 648.6ms | 648.4 | 654.9 | 0.98 |
| B  | CPU 4 GiB | 649.7ms | 650.1 | 654.9 | 0.96 |
| C1 | CPU + 官方 fs tier | 139.3ms | 139.5 | 151.1 | 0.20 |
| C2 | CPU + **uring tier** | **123.3ms** | **121.4** | **139.4** | **0.18** |

前夜的独立复跑(0710,C2 因接口坑手动补跑):C1 147.0ms/0.21、
C2 128.0ms/0.19 —— 两次运行方向与幅度一致,不是噪音。

三个读数:

1. **B≈A 不是失败,是容量论证**:4 GiB 装不下 7 GB 的 prime 工作集,
   churn 再灌 11 GB 把残存的也冲光 —— "多一层内存"在容量不够时等于
   零,这正是磁盘层的入场理由,也是 RUN_ON_GPU §4.1"容量故意压小"
   要买的东西。
2. **磁盘层的价值**:C1 把 revisit 从全量 prefill 的 650ms 打到
   139ms(4.7×)——盘上读 440 MB 回 CPU 再上 GPU,比重算 8k token 的
   prefill 便宜得多。
3. **uring 对 fs 的赢面**:mean -11%,p99 -8%(151→139ms),且 C2 的
   revisit 分布明显更窄(p50 121 / p99 139 vs 140/151)。

### CPU 账:fs 把 IO 记在引擎进程头上,uring 把它挪出去

负载全程 pidstat(5s 粒度)+ iou-wrk 每秒计数:

|            | EngineCore %CPU | 其中 %system | io-wq 线程 |
|------------|-----------------|--------------|-----------|
| C1 (fs)    | 110–121%        | 12–20%       | 全程 0    |
| C2 (uring) | 97–101%         | 2–3.5%       | 稳定 8    |

C1 的 fs tier 在引擎进程内用线程做同步文件 IO,系统调用和数据拷贝全部
记在 EngineCore 账上 —— 比无盘基线(~100%,busy loop)多付约 15 个点,
且几乎全是内核态。C2 的 EngineCore 和无盘基线几乎无差:提交线程只花
2–3 个点,搬运挪给了 8 个 io-wq kernel worker。诚实账:io-wq 是内核线
程,pidstat 不记它们,单看这张表不能下"总 CPU 更低"的结论 —— 但 §12
同盘微基准里 punt 路径的总 CPU(cpu_util 含 worker)全场最低,两笔证据
互补。延迟敏感的 scheduler 线程不被 IO 内核态打扰,大概率也是 C2 p99
更紧的原因。

盘的余量:iostat 里 revisit 阶段读峰 ~270 MB/s、`%util` ≤ 10% ——
这场赢在路径效率,不在带宽;工作集和并发还有一个数量级的加压空间,
CPU 记账的差距会随之放大(fs 的 15 个点是按 IO 量走的,uring 的 2–3
个点近乎常数)。

### 同盘微基准复测 + prewrite 探针:§12 结论稳定,归因再收一层

四引擎复测(blocks=2048 × 1 MiB,QD=32,`micro.json`)和 §12
(blocks=1024)形状完全一致:uring store 3819 MB/s / cpu 0.41 /
syscalls 127,cpp-pool 3799 / 0.66 / 2048,pool 3676 / 1.34 / 4096,
pool-slab 3777 / 0.91 / 2048;load 侧 uring 7401 / 0.60 对 cpp-pool
7341 / 1.03。吞吐全挤在阵列上限,uring 的赢面全在 CPU 和 syscall 轴。
job-blocks sweep(1→128)吞吐与 syscalls 全平 —— gather window 已把
提交批量托管,job 粒度不再影响提交模型(§8.3 联动修改的预期行为)。

prewrite 探针(同文件连写两遍,pass1 全首写 unwritten extent,pass2
全覆盖写 written):wall 0.283s vs 0.278s,iou_wrk 峰值 7 vs 5 ——
punt 不随 extent 状态变化。归因分层:**块设备层(md < 5.17 无
REQ_NOWAIT)先于文件系统层把 NOWAIT 拦死**,fs 层的 unwritten 假设在
md 宿主上根本不可检验(与 §3 当年 dd 预写实验同结论,这次是引擎原生
路径的复证)。"初始化预写消 punt"仍是裸盘 + ext4 宿主上值得做的实验,
归入 §8 的裸盘复测项。

### 面试版一句话

"端到端里我的 uring tier 对官方 fs tier 的赢面是两笔:TTFT p99 -8%
是表,CPU 记账位置是里 —— fs tier 把同步 IO 的 ~15 个点 CPU 记在 vLLM
调度进程头上且全是内核态,我的引擎把搬运挪给 8 个 io-wq worker,推理
进程和无盘基线一样干净。盘只用了 10%,这份差距会随负载放大。三层缓存
的容量论证也齐了:CPU 层装不下工作集时 revisit 等于全量重算(B≈A),
磁盘层一进场就是 5×(650→123ms)。"
