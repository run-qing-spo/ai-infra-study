---
title: AI 存储 · 项目总览
description: KV Cache 分层卸载 + 异步 Checkpoint 两个系统项目的设计、路线图与技术覆盖
---

# AI 存储 · 项目总览

围绕大模型**推理与训练中的存储 / IO**，做两个动手的系统项目——一个偏**读 / 服务侧**（KV Cache 分层卸载），一个偏**写 / 训练侧**（崩溃一致的异步 Checkpoint）。**深度优先**：把两个问题做透，用 trace 驱动的实验和可复现的数据说话。

::: tip 关于本页
本页是两个项目的**设计与路线总览**；细粒度任务和进度追踪见仓库的 **Issues / Milestones**。覆盖矩阵中的标记：**[建]** = 动手实现 · **[读]** = 掌握概念能讲清 · **[已]** = 已在现有笔记覆盖。
:::

---

## 1. 技术覆盖矩阵

两个项目 + 配套背景阅读触及的存储 / IO 技术面：核心主题由项目深度覆盖，广度主题用背景阅读补齐，硬件 / 网络已在现有推理笔记中。

| 主题 | 关键子项 | 覆盖来源 | 标记 |
|---|---|---|---|
| Linux IO 栈 | page cache、buffered/direct IO、mmap、readahead | 项目一 Stage 0 + 项目二 | [建] |
| 异步 IO | io_uring、AIO、cudaMemcpyAsync | 项目二（薄）+ 背景阅读 | [建]/[读] |
| 持久化 / 一致性 | fsync/fdatasync、原子 rename、崩溃一致性、WAL | 项目二（核心） | [建] |
| 文件系统 | ext4/XFS、inode、journaling、小文件问题 | 背景阅读（项目二轻触"选哪个 FS"） | [读] |
| 块层 | IO scheduler、NVMe 队列深度、IOPS vs 带宽 | 项目一 Stage 0 实测 + 背景阅读 | [建]/[读] |
| 存储引擎 | LSM、B+Tree、写放大、compaction、bloom filter | 项目一借用 LSM 思想 + 背景阅读 | [建]/[读] |
| 缓存 | LRU/LFU/ARC、多级缓存、淘汰 / 预取 / 准入 | 项目一（核心） | [建] |
| 分布式存储 | 副本 / 一致性、纠删码、对象存储(S3)、分布式 FS | 背景阅读（项目一 LMCache→S3、项目二写对象存储 轻触） | [读] |
| Checkpoint | async / sharded / incremental、DCP、断点续训、恢复 | 项目二（核心） | [建] |
| DataLoader | prefetch、sharding、小文件打包、GPU 空等 | 背景阅读（可选 demo） | [读] |
| KV Cache | PagedAttention、prefix cache、offload、量化、disagg | 项目一（核心）+ 笔记 | [建]/[已] |
| GPU IO | GDS/cuFile、GPUDirect RDMA、pinned memory、bounce buffer | 项目一 GDS（stretch）+ 笔记 | [建]/[已] |
| 内存层级 | HBM / DRAM / CXL / SSD、NUMA | 笔记 + 项目一模拟器分层 | [已]/[建] |
| 网络存储传输 | RDMA、RoCE / IB、NCCL 边界 | [推理 IO 优化笔记](/infra/inference-io-tech-complete) Layer 4–5 | [已] |

> 关联现有笔记：[推理基础：原理与硬件](/infra/inference-fundamentals)、[推理 IO 优化：技术全景](/infra/inference-io-tech-complete)。

---

## 2. 项目一（读 / 服务侧）：KV Cache 分层卸载

**一句话**：受 LSM 分层 / compaction 启发，为大模型推理设计 KV Cache 多级卸载引擎——以 attention 局部性决定淘汰、以存储层级感知决定放置与预取，冷块下沉时量化压缩。

**设计动机**：现有 offload（LMCache / vLLM）淘汰多用朴素 LRU，无视 KV 的两个结构——attention 偏置（sink + 近期 token 更重要）与多轮前缀复用。本项目把存储引擎的分层 / compaction 思想迁移到 KV 卸载。

### 研究问题

- **RQ1（刻画）**：真实 trace 里 KV 块复用距离 / 前缀命中分布如何？LRU 为何次优？ → Fig 1
- **RQ2（淘汰）**：有 DRAM/SSD 兜底后，attention 感知淘汰 vs LRU 还重要吗？ → Fig 2/3
- **RQ3（放置 + 预取）**：LSM 式分层放置 + 预取能否降低 miss 代价？ → Fig 2/4
- **RQ4（compaction）**：冷块量化（FP16→INT4）用质量换容量是否划算？ → Fig 5

### 里程碑

- **Stage 0A**（本地机器，无 GPU）：`fio` + Python 对比 `read()` / `O_DIRECT` / `mmap`，冷热缓存对比；产出"DRAM 命中 / SSD 顺序 / SSD 随机"延迟·带宽小表（喂给模拟器）
- **Stage 0B**（云端单卡）：跑通 SOTA——vLLM `--enable-prefix-caching` → vLLM 原生 offloading（0.11+ `--kv-offloading-size`）→ vLLM + LMCache（CPU+Disk）；做"同 prompt 两次 TTFT"体感实验，搭好 TTFT 测量管线
- **Stage 1**（刻画）：下载 Mooncake trace，分析 `hash_ids` 复用结构 → Fig 1
- **Stage 2**（模拟器）：纯 Python 三级 tiering 模拟器（HBM/DRAM/SSD），可插拔策略，回放 trace → Fig 2/3/4
- **Stage 3**（真机验证）：挑 2–3 个工作点用 vLLM connector API / LMCache 钩子实现本项目策略，校准模拟器
- **Stage 4**（compaction）：冷块量化，测容量 vs 质量 → Fig 5
- **Stage 5**（敏感性）：扫 HBM 预算 → Fig 6 + 跨 trace 汇总表

### Trace / Baseline / 指标 / 图表

| 维度 | 内容 |
|---|---|
| **Trace** | 主力 `valeriol29/mooncake-traces`（HF，带 `timestamp`+`input/output_length`+`hash_ids`）；泛化用 LMSYS-Chat-1M、Aliyun Bailian(ATC'25)；对照 ShareGPT |
| **策略基线** | Recompute(no-offload) · **LRU(主基线)** · LFU/FIFO · Attention-drop(H2O/StreamingLLM) · **本方法** |
| **系统载体** | vLLM prefix caching → vLLM 原生 offloading connector → vLLM + LMCache(CPU+Disk) |
| **指标** | **TTFT p50/p90/p99(头条)** · TBT/ITL · 分层命中率(HBM/DRAM/SSD/miss) · Throughput@SLO · 重算节省 · 层间流量字节 · 容量效率 · 质量 delta(perplexity) |

| 图 | 内容 | 回答 |
|---|---|---|
| Fig 1 | KV 复用距离 / 前缀命中 CDF | RQ1：LRU 为何次优 |
| Fig 2 | 各策略 TTFT p99 vs 负载(req/s) | RQ2+3（头条） |
| Fig 3 | 分层命中率堆叠柱 | 机制解释 |
| Fig 4 | 消融：attention 淘汰→分层放置→预取→量化 | 隔离贡献 |
| Fig 5 | 冷块量化 容量 vs 质量 | RQ4 |
| Fig 6 | 性能 vs HBM 预算 | 显存越紧收益越大 |

::: warning 方法论：模拟器优先
不要一上来改 vLLM 内核。顺序：Stage 0 测分层延迟 → Mooncake `hash_ids` 当 KV 块访问流 → Python 模拟器出 Fig 1–4（无 GPU）→ 少量真机验证校准。trace-driven simulation 是系统研究的常用方法。
:::

### 验收标准

- 6 张图 + 1 张跨 trace 汇总表齐全
- 在 `REPORT.md` 里给出完整证据链：发现问题 → 假设 → 隔离变量实验 → 机制解释 → 消融 → 反直觉结论
- 至少 1 个反直觉发现（如 Fig 6：HBM 越紧本方法相对 LRU 提升越大）

---

## 3. 项目二（写 / 训练侧）：崩溃一致的异步 Checkpoint

**一句话**：实现一个大模型 checkpoint 写引擎，覆盖分片、异步落盘、崩溃一致性，对标业界方案（PyTorch DCP / ByteCheckpoint）的核心问题。**几乎不需要 GPU**（纯 IO + 序列化）。

**动机**：项目一偏读 / 缓存，碰不到写路径与持久化语义；而 fsync / 崩溃一致性 / checkpoint 是训练侧存储的核心问题。两个项目一拼，正好覆盖读 + 写、服务 + 训练、缓存 + durability 的全貌。

### 里程碑

- **C1**：把若干 tensor 序列化成「JSON header + 连续 blob」（仿 safetensors），实现 mmap 零拷贝 lazy load 单个 tensor
- **C2**：持久化正确性——临时文件 + `fsync` + 原子 `rename`，注入崩溃（写一半 kill）验证可恢复；对比有无 fsync 的差异
- **C3**：分片 + 异步——多线程分片写 + 异步落盘，让"序列化 CPU 工作"与"磁盘 IO"重叠；对比 `torch.save` 耗时
- **C4**：完整性——每分片 checksum，损坏检测与跳过
- **C5（可选 stretch）**：写到对象存储（MinIO/S3），轻触分布式存储

### 验收标准

- 能用实测数据回答："fsync 之后一定落盘了吗？怎么保证 checkpoint 崩溃可恢复？"
- 相比 `torch.save` 给出量化的写入提速 / overlap 收益

---

## 4. 背景知识 / 延伸阅读

项目本身不动手实现、但与存储面相关的广度主题；目标是掌握概念与权衡。

**分布式存储**
- 副本 vs 纠删码（成本 / 可靠性权衡）
- 一致性模型（强 / 最终一致、CAP、quorum）
- 对象存储（S3 语义、扁平命名、最终一致）
- 分布式 FS：Lustre / BeeGFS / JuiceFS / CephFS 各自定位
- 元数据瓶颈与扩展（为什么海量小文件压垮元数据）

**文件系统 / 块层**
- ext4 vs XFS、inode / extent、journaling
- IO scheduler（mq-deadline / none）、NVMe 多队列
- io_uring 相比 libaio 的优势（可做个 micro-demo）

**存储引擎**
- B+Tree vs LSM（读写放大、随机 vs 顺序）
- 写放大 / 读放大 / 空间放大三角
- bloom filter、compaction 策略（leveled / tiered）

**DataLoader / 数据集**（可选 demo）
- 小文件打包：WebDataset / tar / RecordIO
- prefetch + double buffering 消除 GPU 空等
- shuffle / 分片 / 多 worker 读放大

**硬件 / 网络**（已在笔记，过一遍即可）
- GDS / GPUDirect RDMA / pinned memory / bounce buffer → [推理 IO 优化笔记](/infra/inference-io-tech-complete)
- HBM / DRAM / CXL / SSD 层级与 NUMA → [推理基础笔记](/infra/inference-fundamentals)

---

## 5. 硬件需求

| 阶段 | 环境 | 说明 |
|---|---|---|
| IO 栈 / checkpoint / 模拟器 | 本地机器：好的 NVMe(2TB) + 64–128GB 内存 | 纯 CPU·DRAM·SSD，无需 GPU |
| 真实 KV / 模型加载 / GDS | 云端**单卡**（如 4090 24G 或 A100 40/80G） | 跑通 vLLM offloading、真机校准 |
| 偶尔多卡 | 按需 spot | 偶发验证 |

> RDMA / 多机网络与 CXL 没有对应硬件时，用 loopback + 注入延迟、远端 NUMA node + 注入延迟来模拟。大部分工作（trace 刻画、模拟器、checkpoint）在本地纯 CPU 环境即可完成，GPU 只在真机校准阶段短时租用。

---

## 6. 路线图（阶段）

- **热身**：项目一 Stage 0A（IO 栈）+ 项目二 C1–C3（checkpoint 核心）——先建立存储基本功，跑出第一个能展示的项目
- **项目一主力**：项目一 Stage 1–5（trace → 模拟器 → 真机 → 量化 → 敏感性）
- **进阶（可选）**：项目二 C4–C5（完整性 / 对象存储）、DataLoader 小文件加载 demo、GDS 真机路径
- **持续打磨**：每个里程碑在项目 `REPORT.md` 留带数字的图表

---

## 7. 项目要回答的关键技术问题

交付时应能用设计与实测数据回答：

- buffered read 和 O_DIRECT 各自什么场景更快？为什么 direct IO 不一定快？
- fsync 之后数据一定落盘了吗？怎么保证 checkpoint 崩溃一致性？
- 训练时 GPU 利用率低，怎么判断是不是 dataloader / IO 的锅？怎么定位？
- 海量小文件为什么是存储噩梦？怎么解决？
- KV Cache 显存放不下，分层方案怎么设计？换入换出 / 淘汰策略？（联系 LSM）
- GDS 相比传统路径省了什么？依赖什么条件？
- LSM vs B+Tree 的读写放大差异？compaction 在干什么？
- 副本 vs 纠删码怎么选？对象存储的一致性语义？

---

*本页随项目推进更新；细粒度任务与进度见仓库 Issues / Milestones。*
