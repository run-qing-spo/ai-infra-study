---
AI 存储 · KV Cache 基础项目总览
---

# AI 存储 · KV Cache 基础项目总览

我围绕大模型**推理中的 KV Cache 存储**，计划做 10 个渐进式的 C++ 小项目——从单层缓存到 4 层完整路径，每个项目都有可见产出。**每周一个交付**，直接对齐 FlexKV/Mooncake 的模块需求。

::: tip 关于本页
这是我 10 个项目的**总览与路线图**；每个项目的详细设计见对应目录的 `README.md`。覆盖矩阵中的标记：**[建]** = 我动手实现 · **[读]** = 我掌握概念并能讲清。
:::

---

## 1. 硬件需求

| 阶段 | 硬件要求 | 时间段 |
|------|---------|--------|
| Phase 1-2（单层 + 异步 I/O） | 本地机器（16GB RAM + 256GB SSD） | Week 1-4 |
| Phase 3（+ GPU） | 云端 GPU（4090/A100） | Week 5-8 |
| Phase 4-5（+ Remote 模拟） | 本地 loopback + 延迟注入 | Week 9-16 |

> **Remote 层我不打算真的搭多机** —— 用 `127.0.0.1` + 注入延迟模拟 RDMA，这是系统研究里常用的做法。

---

## 2. 技术覆盖矩阵

| 主题 | 对应项目 | 标记 |
|------|---------|------|
| 并发控制 | P1-P2 | [建] |
| 异步 I/O（io_uring） | P3 | [建] |
| 多级缓存（LRU/LFU） | P4-P6 | [建] |
| GPU I/O（pinned memory） | P5-P6 | [建] |
| RadixTree 索引 | P7 | [建] |
| 分布式层模拟（RPC + 延迟注入） | P8 | [建] |

---

## 3. 与 FlexKV/Mooncake 对齐

| FlexKV 模块 | 对应项目 | 说明 |
|-------------|---------|------|
| 本地 StorageEngine | P4-P5 | 多级存储（DRAM/SSD/HBM） |
| 本地 TransferEngine | P6 | 跨层级异步传输 |
| RadixTree | P7 | prefix 复用索引 |
| 分布式 TransferEngine | P8 | Remote 层（用 loopback 模拟） |

---

## 4. 10 个项目详解

### Phase 1：单层缓存基础（Week 1-2）

#### 项目 1：Thread-safe LRU Cache

**交付**：C++ 实现 `get(key)` / `put(key, value)` 接口，16 线程并发测试通过，`-fsanitize=thread` 0 race。

**重点**：
- `std::mutex` + `std::list` + `std::unordered_map` 组合
- 锁竞争分析（为什么 `get` 不能用 `shared_mutex` 简单优化）
- TSan 使用
- Sharding 优化（64 shards，对比全局锁）

**详细设计**：[项目 1 README](./project1_lru/README)
**背景阅读**：[多线程缓存设计综述](../algorithms/concurrent-cache.md) · [LRU/ARC/LFU 算法对比](../algorithms/lru-arc-lfu-comparison.md)

**代码**：`projects/project1_lru/`

---

#### 项目 2：Bounded SPSC Lock-free Queue

**交付**：ring buffer + atomic 索引，性能数据对比 `std::queue + mutex`（吞吐 + p99 延迟）。

**重点**：
- `std::atomic` + memory order
- cache line 对齐（`alignas(64)`）避免 false sharing
- 为什么单线程下 mutex 几乎无开销，跨核才显优势

**代码**：`projects/project2_queue/`

---

### Phase 2：异步 I/O 基础（Week 3-4）

#### 项目 3：Async I/O 存储后端

**交付**：基于 `liburing` 封装 io_uring，SSD 读/写接口，吞吐 + p99 延迟数据表。

**重点**：
- `liburing` API（提交队列 / 完成队列）
- 零拷贝 I/O
- 为什么异步 I/O 在高并发场景优于传统 `read()/write()`

**代码**：`projects/project3_io_uring/`

---

#### 项目 4：2 层缓存引擎

**交付**：DRAM + SSD，block 化布局，支持 LRU / LFU / W-TinyLFU / ARC 淘汰策略，真实 trace 回放验证，策略间命中率对比。

**重点**：
- Block 化布局：固定大小 block（对齐 FlexKV/vLLM 的 16 tokens/block），block id → 物理地址偏移，与后续 GPU KV 同 shape
- 冷热分层（什么时候从 DRAM 淘汰到 SSD）
- 多种淘汰策略对比：LRU / LFU 作为基线，W-TinyLFU / ARC 作为优化，trace 回放对比命中率（对齐方向文档候选 A 的 PR 切入点）
- 预取策略（什么时候提前把 SSD 的数据拉回 DRAM）
- trace-driven 验证方法

**代码**：`projects/project4_2tier/`

---

### Phase 3：多级缓存 + GPU（Week 5-8）

#### 项目 5：3 层缓存引擎

**交付**：HBM + DRAM + SSD，block 化布局贯穿三层，租用 GPU 几小时验证完整路径。

**重点**：
- Block 化布局贯穿三层：block id 在三层间统一，升降级只改 block 所在 tier 标记，不重写数据
- pinned memory（为什么需要，如何分配）
- `cudaMemcpyAsync` + CUDA stream
- GPU ↔ CPU ↔ SSD 的数据流

**代码**：`projects/project5_3tier/`

**硬件**：云端 GPU（4090/A100）

---

#### 项目 6：FlexKV TransferEngine 简化版

**交付**：跨层级传输的异步流水线，隐藏延迟，性能对比图。

**重点**：
- CUDA stream 的 pipeline parallelism
- 如何让计算与传输重叠
- FlexKV 本地 TransferEngine 的核心思想

**代码**：`projects/project6_transfer/`

---

### Phase 4：扩展（Week 9-12，stretch goal）

#### 项目 7：RadixTree 索引

**交付**：支持 prefix 复用的 RadixTree，对齐 FlexKV 的索引结构。

**重点**：
- RadixTree 数据结构（如何支持高效的 prefix 命中）
- 并发安全的读路径（读路径无锁，写路径加锁）
- 为什么 RadixTree 比 hashmap 更适合 KV Cache 的 prefix 复用
- Copy-on-Write block 共享：多请求共享相同 prefix block，新增 token 才分裂（stretch goal）

**代码**：`projects/project7_radixtree/`

---

#### 项目 8：Remote 层模拟

**交付**：用 loopback + 延迟注入模拟 RDMA，4 层完整路径，延迟瀑布图。

**重点**：
- RPC 抽象（如何封装跨节点调用）
- 延迟建模（如何用本地注入模拟网络延迟）
- 4 层协同（HBM/DRAM/SSD/Remote 之间的调度）

**代码**：`projects/project8_4tier/`

---

### Phase 5：整合（Week 13-16）

#### 项目 9：完整 KV Cache 引擎

**交付**：4 层 + RadixTree + 异步传输，对标 FlexKV 本地路径，完整性能报告。

**重点**：
- 系统整合（如何把前面的模块拼起来）
- 性能调优（瓶颈定位与优化）
- 与 FlexKV 的设计差异分析

**代码**：`projects/project9_full_engine/`

---

#### 项目 10：深度博客

**交付**：围绕我最满意的项目写一篇技术博客，发布到知乎/掘金/火山引擎开发者社区。

**内容要求**：
- 架构图
- 代码引用
- 我的实验数据
- 设计 trade-off 讨论

**用途**：技术总结与对外分享

---

## 5. 验收标准

- **每个项目**我都会有 `tests/` 目录 + 可运行的 demo
- **每个项目**我都会有 `README.md` 说明设计决策
- **Week 8**：2 层 vs 3 层性能对比图
- **Week 12**：RadixTree vs hashmap 的 prefix hit 率对比
- **Week 16**：4 层完整路径的延迟瀑布图

---

## 6. 背景阅读（可选，按需）

以下主题我**不打算在项目里深入实现**，但会按需了解概念，帮助理解相关系统背景：

- **分布式一致性**：Raft 论文（只读 abstract + 核心章节）
- **文件系统**：ext4 vs XFS、journaling（概念层）
- **块层**：IO scheduler、NVMe 队列（概念层）
- **存储引擎**：LSM vs B+Tree、写放大（概念层）

---

*我会随项目推进更新本页；每个项目的详细设计见对应目录的 `README.md`。*
