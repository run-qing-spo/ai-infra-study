---
title: 推理基础：原理与硬件
description: Transformer 推理原理与 GPU / NIC / NVLink / PCIe / NVMe / CXL 硬件架构
---

# 推理基础：原理与硬件

Part 1: 为什么推理框架需要调用这些接口（推理原理） · Part 2: 硬件组件内部结构与组织方式

## Part 1: Transformer 推理原理

::: info 这部分回答的问题
推理框架调用了 flash_attn、all_reduce、paged_attention、scheduler 等接口，但为什么需要这些步骤？每一步在数学上做了什么？数据为什么要这样流动？
:::

### 1.1 从文本到向量：Tokenizer + Embedding

| | |
|---|---|
| **做了什么** | "Hello world" → Tokenizer 切成子词 → token ID 序列 [15496, 995] → Embedding 层查表 → 每个 ID 对应一个 d 维向量（如 d=8192）。 |
| | 输出：形状 [seq_len, hidden_dim] 的矩阵，每行是一个 token 的向量表示。 |
| **为什么需要这步** | 神经网络不能直接处理文字，必须转成数值向量。Embedding 是一个可学习的查找表：训练过程中，语义相近的词（如"猫"和"狗"）会被映射到空间中距离接近的向量。 |
| **IO 视角** | Embedding 表 = vocab_size × hidden_dim × dtype。Llama-3 的词表 128K × 8192 × 2B = 2 GB。TP 场景下词表按行切分到各卡。这是一次 HBM 读操作（查表）。 |

### 1.2 Self-Attention：每个 token 如何"理解"上下文

::: tip 核心直觉
"The cat sat on the mat because it was tired" — 当模型处理"it"这个词时，需要"回头看"所有前面的词，判断"it"指的是"cat"而不是"mat"。Self-Attention 就是这个"回头看"的数学机制。
:::

| | |
|---|---|
| **QKV 三个矩阵的含义** | 输入 X（每行是一个 token 的向量）分别乘以三个权重矩阵： |
| | · Q = X × W_Q ——"我在找什么信息？"（Query） |
| | · K = X × W_K ——"我包含什么信息？"（Key） |
| | · V = X × W_V ——"我能提供什么信息？"（Value） |
| | 类比：Q 是搜索词，K 是网页标题，V 是网页内容。QK 相似度高 = 这个网页与搜索词相关 = 多关注它的 V。 |
| **Attention 计算** | `Attention(Q,K,V) = softmax(Q × K^T / sqrt(d_k)) × V` |
| | · Q × K^T：每个 token 与所有其他 token 的相似度得分（seq × seq 矩阵） |
| | · / sqrt(d_k)：缩放，防止得分过大导致 softmax 饱和 |
| | · softmax：将得分归一化为概率（0–1，总和为 1） |
| | · × V：按概率加权组合所有 token 的 Value，得到"综合了上下文信息"的新表示 |
| **Multi-Head：为什么要多个"头"** | 一个 head 只能关注一种关系（如语法）。多个 head 各自学习不同关系（语法、指代、语义），最后拼接。Llama-3 70B 有 64 个 Q head。 |

**为什么框架调用 flash_attn_func()**

标准 Attention 需要先算出完整的 seq×seq 得分矩阵，存入 HBM（Prefill 时 4096×4096×2B = 32 MB/head，64 head = 2 GB），再从 HBM 读回来做 softmax。Flash Attention 将 Q/K/V 分块加载到 SM 的 SRAM（~200 KB），在 SRAM 内完成 softmax+matmul，避免中间矩阵写回 HBM。HBM 访问量从 O(N²) 降到 O(N)。

### 1.3 KV Cache：为什么要缓存 K 和 V

| | |
|---|---|
| **问题** | 自回归生成时，模型逐个产出 token： |
| | · Step 1：输入 [A B C]，生成 D → attention 计算涉及 A/B/C 的 K,V |
| | · Step 2：输入 [A B C D]，生成 E → attention 计算涉及 A/B/C/D 的 K,V |
| | · Step 3：输入 [A B C D E]，生成 F → attention 计算涉及 A/B/C/D/E 的 K,V |
| | A/B/C 的 K 和 V 在每一步都完全相同（因为它们的位置和内容不变），但每步都重新计算 = 纯浪费。 |
| **解决方案：缓存** | Prefill 时一次性计算所有输入 token 的 K,V 并存入 GPU HBM（即 KV Cache）。 |
| | Decode 每步只计算新 token 的 Q、K、V： |
| | · 新的 K,V 追加到 cache 末尾 |
| | · 新的 Q（只有 1 个 token）与整个 cache 的 K 做 attention |
| | · 计算量从 O(N²) 降到 O(N)（每步只处理 1 个 query vs N 个 key） |
| **代价** | KV Cache 占大量 HBM。Llama-3 70B GQA(8 heads)、4096 seq: ~10 GB/请求。这就是为什么内存管理（PagedAttention、CXL 扩展）如此重要。 |

::: info 这解释了为什么 Prefill 和 Decode 性质不同
Prefill 处理 N 个 token：Q 是 [N, d] 矩阵 × K^T 是 [d, N] 矩阵 → 矩阵-矩阵乘（GEMM）→ 计算密集。

Decode 处理 1 个 token：Q 是 [1, d] 向量 × K^T 是 [d, N] 矩阵 → 矩阵-向量乘（GEMV）→ 算力几乎用不上，瓶颈是从 HBM 读 N 个 K,V 向量。
:::

### 1.4 GQA / MQA：为什么 KV Head 数可以比 Q Head 少

| | |
|---|---|
| **MHA 的问题** | 标准 MHA（Multi-Head Attention）：64 个 Q head 对应 64 个 K head 和 64 个 V head。KV Cache = 2 × 80 layers × 64 heads × 128 dim × seq × 2B。4096 seq 时 = 80 GB。一个请求就占满一张 A100。 |
| **解决方案：共享 KV Head** | 实验发现，多个 Q head 可以共享同一组 K,V（因为不同 head 学到的 K,V 有很大冗余）。 |
| | · GQA（Grouped）：每 8 个 Q head 共享 1 组 K,V → KV head 从 64 降到 8 → KV Cache 缩小 8x |
| | · MQA（Multi-Query）：所有 Q head 共享 1 组 K,V → KV head 只有 1 → KV Cache 缩小 64x |
| | Llama-3 70B 使用 GQA(8)，所以 KV Cache = ~10 GB/请求而非 80 GB。 |
| **IO 意义** | Decode 每步读取的 KV 字节数直接按 head 数成比例下降 → TBT 改善。这是模型架构层面的 IO 优化——你无法改变它，但必须理解它来计算 IO 量。 |

### 1.5 FFN（前馈网络）：Attention 之后为什么还需要 FFN

| | |
|---|---|
| **Attention 的局限** | Attention 做的是"信息混合"——让每个 token 整合来自其他 token 的信息。但它本质上是线性操作（加权求和）。仅靠线性操作无法逼近复杂函数。 |
| **FFN 的角色** | FFN 对每个 token 独立施加非线性变换：`FFN(x) = W_down · SiLU(W_gate · x) · (W_up · x)`。 |
| | 类比：Attention = "开会讨论，整合各方意见"，FFN = "每个人回去独立思考，加深理解"。两者交替进行 80 次（80 层），最终形成对输入的深层理解。 |
| **IO 视角** | FFN 的权重占模型总权重的 ~2/3（gate + up + down 三个大矩阵）。Decode 每步读取权重 35 GB 中约 23 GB 是 FFN 权重。这也是为什么权重量化对 TBT 影响最大——FFN 权重量化直接减少最大头的 IO。 |

### 1.6 RMSNorm + Residual：为什么每层前后都有

Residual（残差连接）：输出 = 输入 + 层的变换结果。作用：防止深层网络梯度消失，让信息能"跳过"不需要的层直接传递。

RMSNorm（归一化）：将每个 token 向量的模长缩放到稳定范围。作用：防止激活值在 80 层传播中爆炸或消失。

从 IO 角度：两者都是逐元素操作（elementwise），计算量小但涉及 HBM 读写。Kernel Fusion 将它们与相邻算子合并，减少 HBM 往返。

### 1.7 Tensor Parallel：为什么每层需要 2 次 All-Reduce

| | |
|---|---|
| **问题** | Llama-3 70B FP16 = 140 GB 权重，A100 单卡只有 80 GB HBM（扣掉 KV 后可用 ~45 GB）。单卡装不下。必须切分。 |
| **切分方式** | Tensor Parallel 将每层的权重矩阵按列或按行切分到 4 张 GPU： |
| | · Column Parallel（QKV projection, FFN up/gate）：权重按列切，每卡算部分列 → 输出是"部分结果" |
| | · Row Parallel（Attention out projection, FFN down）：权重按行切，每卡算部分行，输出需要求和 → All-Reduce |
| **All-Reduce：为什么必须做** | Column Parallel 切分后，每张 GPU 只有部分列的结果。接下来的 Row Parallel 需要完整的输入。因此： |
| | · Attention 结束 → All-Reduce #1（合并 attention 输出）→ 送入 FFN |
| | · FFN 结束 → All-Reduce #2（合并 FFN 输出）→ 送入下一层 |
| | All-Reduce = 每张 GPU 将自己的部分结果发送给所有其他 GPU，同时接收所有其他 GPU 的部分结果，最终每张卡都有完整的求和结果。 |
| **IO 视角** | 这就是为什么框架每层调用 2 次 `torch.distributed.all_reduce()`。NCCL 自动选择最优路径（NVLink P2P 或 GPUDirect RDMA），将数据在 GPU 间搬运。80 层 × 2 = 160 次/请求。 |

### 1.8 LM Head + Sampling：从向量到下一个词

80 层 Transformer 之后，最后一个 token 位置得到一个 hidden_dim 维的向量。

LM Head（一个线性层）将它投影到 vocab_size 维（Llama-3 = 128K）→ 每个维度的值称为 logit，表示对应词作为下一个 token 的"打分"。

Sampling：softmax 将 logits 转为概率分布 → top-p/temperature 截断 → `torch.multinomial` 随机采样 → 输出 1 个 token ID。

IO 视角：LM Head 权重 = hidden_dim × vocab_size × dtype = 8192 × 128K × 2B ≈ 2 GB。Decode 每步也要读这 2 GB。

### 1.9 Scheduling + PagedAttention：为什么需要调度和内存管理

| | |
|---|---|
| **问题** | 推理服务同时处理多个请求。每个请求的 KV Cache 占 ~2.5 GB/卡（TP=4, GQA）。A100 扣除权重后只剩 ~40 GB → 最多 ~16 个请求同时在 GPU 上。 |
| | 而且不同请求的输入长度不同、生成步数不同、到达时间不同。怎么管理？ |
| **PagedAttention 解决内存碎片** | 传统方式：为每个请求预分配 max_seq_len 的连续内存。请求只用了 100 token，却占 4096 token 的空间 → 浪费 97%。 |
| | PagedAttention：像操作系统管理虚拟内存一样，将 KV Cache 分成固定大小的块（如 16 token/块），按需分配。请求用多少分配多少，结束后回收。浪费率从 60–80% 降到 <4%。 |
| **Scheduler 解决并发调度** | 每轮迭代前，Scheduler 决定： |
| | · 新请求能否加入（KV 块够不够？） → `BlockManager.can_allocate()` |
| | · 内存不足时谁被换出（优先级最低的请求的 KV swap 到 CPU） → `swap_out()` |
| | · 被换出的请求能否换回（GPU 有空闲块了？） → `swap_in()` |
| | 这就是为什么框架有 Scheduler 和 BlockManager——它们控制着所有 KV 相关 IO 的触发时机。 |

### 1.10 Continuous Batching + Chunked Prefill

| | |
|---|---|
| **Continuous Batching** | 传统 static batching：等一个 batch 的所有请求都生成完，再处理下一个 batch。短请求等长请求 → GPU 空转。 |
| | Continuous Batching：某个请求完成后立即移出，空出的槽位立即填入新请求。GPU 永远在工作。 |
| **Chunked Prefill** | 一个 4096 token 的 Prefill 可能需要 ~800 ms。在这期间，decode 请求无法执行（GPU 被独占），TBT 飙升。 |
| | Chunked Prefill：将长 prefill 切成小块（如 512 token），每块与 decode token 混合在同一 batch 中执行。Prefill 慢一点，但 decode 请求的 TBT 不会被饿死。 |
| **为什么这与 IO 相关** | 调度策略决定了每轮 GPU 上同时有多少请求 → 决定了 KV Cache 总占用 → 决定了是否触发 swap IO → 决定了 Throughput 天花板。 |

### 小结：一条请求的完整计算流

| 步骤 | 数学操作 | 为什么需要 | 对应的框架接口 | 产生的 IO |
|------|----------|------------|----------------|-----------|
| Tokenize | 文本 → token ID 序列 | 网络只能处理数值 | `tokenizer.encode()` | 无（CPU 操作） |
| Embedding | token ID → d 维向量 | 将离散 ID 映射到连续空间 | `model.embed_tokens()` | HBM 读：词表 shard |
| × 80 层：QKV Proj | X × W_Q/W_K/W_V（矩阵乘） | 为 Attention 准备 Query/Key/Value | `ColumnParallelLinear()` | HBM 读：QKV 权重 shard |
| × 80 层：Attention | softmax(QK^T/√d) × V | 让每个 token 整合上下文信息 | `flash_attn` / `paged_attn` | HBM 读：KV Cache（decode 时） |
| × 80 层：KV 写入 | 新 K,V 追加到 cache | 缓存已计算的 K,V 避免重复计算 | `reshape_and_cache()` | HBM 写：KV 块 |
| × 80 层：All-Reduce #1 | 各卡部分结果求和 | TP 切分后需要合并完整结果 | `all_reduce(tp_group)` | GPU↔GPU：NVLink/RDMA |
| × 80 层：FFN | W_down · SiLU(W_gate·x)·(W_up·x) | 对每个 token 施加非线性变换 | `RowParallelLinear()` | HBM 读：FFN 权重（最大头） |
| × 80 层：All-Reduce #2 | 各卡部分结果求和 | TP 切分后需要合并完整结果 | `all_reduce(tp_group)` | GPU↔GPU：NVLink/RDMA |
| × 80 层：RMSNorm | x / RMS(x) × γ | 稳定激活值范围 | `fused_rmsnorm()` | HBM 读写（可 fuse） |
| LM Head | hidden → vocab logits | 将向量投影到词表维度 | `lm_head.forward()` | HBM 读：LM Head 权重 ~2 GB |
| Sampling | logits → probability → token ID | 从概率分布中选出下一个词 | `torch.multinomial()` | GPU 内计算 |
| Output | token ID → 文本字符 | 返回给用户可读的文字 | `detokenizer.decode()` | GPU→CPU ~4B（token ID） |

---

## Part 2: 硬件组件内部结构与组织方式

::: info 这部分回答的问题
推理中提到了 SM、Tensor Core、HBM、NVLink、NIC、PCIe、CXL 设备等组件。它们各自内部是什么结构？彼此怎么连接？数据怎么在它们之间流动？
:::

### 2.1 GPU 内部结构

::: tip GPU = 数千个小核心 + 超大带宽内存
与 CPU（少量强核心）不同，GPU 有数千个简单核心并行执行相同操作。推理的矩阵乘法天然适合这种大规模并行：每个核心负责结果矩阵的一小块。
:::

| 组件 | 位置 | 容量 / 规格（A100） | 访问延迟 | 作用 |
|------|------|----------------------|----------|------|
| Registers（寄存器） | 每个线程内 | 255 个 32-bit / 线程 | 0 cycle | 线程的私有变量存储——最快的存储层级 |
| Shared Memory / L1 Cache（SRAM） | 每个 SM 内 | ~164 KB / SM（可配置） | ~20 cycles（~30 ns） | SM 内线程共享的高速暂存区。Flash Attention 的 tiling 就在这里做——将 QKV 分块加载到 SRAM 计算 |
| L2 Cache | 全局共享 | 40 MB | ~200 cycles（~200 ns） | HBM 的缓存层，缓解重复 HBM 访问 |
| CUDA Core | SM 内 | 6912 个 FP32 core（108 SM × 64） | — | 通用浮点计算单元。处理逐元素操作（RMSNorm、激活函数等） |
| Tensor Core | SM 内 | 432 个（108 SM × 4） | — | 矩阵乘法专用硬件。一次执行 4×4 或更大的矩阵块乘法。推理中所有 GEMM/GEMV 都在 Tensor Core 上执行 |
| HBM（高带宽内存） | GPU 封装外围 | 80 GB · 2 TB/s | ~300–500 ns | 主存。所有权重、KV Cache、激活都在这里。多层 DRAM 堆叠 + 硅通孔（TSV）实现超高带宽 |
| Memory Controller | GPU 芯片上 | — | — | 管理 SM 到 HBM 的读写请求。MBU 就是衡量这条通道利用率 |
| NVLink 接口 | GPU 芯片边缘 | 12 条链路 × 50 GB/s = 600 GB/s | ~1 μs | GPU 间直连高速通道。GPUDirect P2P 走这条路 |
| PCIe 接口 | GPU 芯片边缘 | PCIe 4.0 x16 = 32 GB/s | ~1–2 μs | 连接 CPU、NIC、NVMe。GPUDirect RDMA 和 GDS 走这条路 |
| Copy Engine（DMA 引擎） | GPU 芯片上 | 多个独立引擎 | — | 专门执行 memcpy（H2D/D2H/P2P），不占用 SM 计算资源。`cudaMemcpyAsync` 由它执行 |

关键理解：SM 执行计算（kernel），Copy Engine 执行数据搬运（memcpy），两者可以并行——这就是 CUDA Streams 实现计算-IO 重叠的硬件基础。

### 2.2 HBM：为什么 GPU 内存带宽这么高（但容量这么贵）

| | |
|---|---|
| **物理结构** | HBM = 多层 DRAM 芯片垂直堆叠（3D 堆叠），通过数千个硅通孔（TSV）连接到 GPU。 |
| | A100 有 5 个 HBM2e 堆栈，每个 16 GB，共 80 GB。每个堆栈通过 1024-bit 宽的接口连接。 |
| | 带宽 = 5 堆栈 × 1024-bit × ~3.2 Gbps = ~2 TB/s。比 DDR5（~100 GB/s）快 20 倍。 |
| **为什么容量受限** | 3D 堆叠良率低（一层坏 = 整个堆栈报废），且必须与 GPU 共封装（interposer 面积有限）。成本远高于 DDR DRAM（约 10 倍/GB）。 |
| | 这正是 CXL 的价值：用廉价的 DDR DRAM 模组（TB 级）扩展 GPU 可访问的内存空间，弥补 HBM 容量不足。 |

### 2.3 NIC / HCA（网卡）内部结构

以 NVIDIA ConnectX-7（400 Gb/s InfiniBand / RoCE）为例。NIC 是 RDMA 和 GPUDirect RDMA 的硬件基础。

| 组件 | 作用 | 与推理 IO 的关系 |
|------|------|------------------|
| Network Ports | 400 Gb/s InfiniBand 或以太网物理接口 | 数据进出网络的物理通道 |
| DMA 引擎 | 直接读写主机（或 GPU）内存，不需要 CPU 逐字节拷贝 | RDMA 零拷贝的核心：NIC 的 DMA 引擎直接从 GPU HBM（通过 PCIe BAR1 映射）读数据发送到网络 |
| QP（Queue Pair）硬件 | 每个 RDMA 连接用一对 Send Queue + Receive Queue 管理 | `ibv_create_qp` 创建的 QP 在 NIC 硬件中有对应状态。NIC 自主处理 QP 中的 Work Request，CPU 只需提交 |
| CQ（Completion Queue） | NIC 完成一个 WR 后写入 CQ 通知 CPU/应用 | `ibv_poll_cq` 轮询 CQ 检查传输完成——这是异步 RDMA 的通知机制 |
| PCIe 接口 | x16 PCIe 5.0 连接到 CPU 或 GPU 所在的 PCIe switch | GPUDirect RDMA 要求 NIC 和 GPU 尽量在同一 PCIe switch 下（同一 NUMA 节点），否则跨 NUMA 带宽下降 30–50% |
| 硬件 offload 引擎 | 协议处理（可靠传输、拥塞控制）全部在 NIC 硬件中完成 | CPU 不参与 RDMA 数据路径的任何协议处理——所有重传、ACK、拥塞控制由 NIC 自主完成 |

### 2.4 NVLink + NVSwitch：GPU 间高速直连

| | |
|---|---|
| **NVLink 物理结构** | NVLink 是 GPU 芯片边缘引出的高速串行链路。每条链路 50 GB/s 双向。A100 有 12 条，H100 有 18 条。 |
| | 两个 GPU 之间可以有多条 NVLink 并行，带宽叠加。NVLink 支持 load/store 语义：GPU A 可以直接用指针访问 GPU B 的 HBM 地址空间。 |
| **NVSwitch 的作用** | 没有 NVSwitch 时，8 卡之间需要两两连 NVLink → 每张卡的 NVLink 数量不够（12 条分给 7 张卡 → 每条通道只有 ~85 GB/s）。 |
| | NVSwitch 是一个交换芯片（类似网络交换机）：所有 GPU 的 NVLink 连到 NVSwitch，NVSwitch 提供全互联的交叉开关（crossbar）→ 任意两卡间都能跑满 NVLink 全速。 |
| | DGX A100 有 6 个 NVSwitch；DGX H100 有 4 个 NVSwitch（Gen3）。 |
| **推理中的作用** | TP All-Reduce 在节点内走 NVLink + NVSwitch。NCCL 的 ring/tree 算法利用 NVSwitch 的全互联带宽，All-Reduce 延迟极低。 |

### 2.5 PCIe：一切节点内互联的物理基础

| 概念 | 说明 | 推理中的影响 |
|------|------|--------------|
| Lane（通道） | PCIe 的最小数据通路。PCIe 5.0 单 lane 单向 ~4 GB/s | x16 插槽 = 16 lanes = 64 GB/s 单向。GPU/NIC 通常用 x16 |
| Root Complex | CPU 芯片内的 PCIe 控制器，是 PCIe 总线树的根节点 | CPU 通过 Root Complex 连接所有 PCIe 设备 |
| PCIe Switch | 扇出芯片：将一个上游 PCIe 端口分成多个下游端口 | GPU 和 NIC 如果在同一 PCIe Switch 下 → P2P/RDMA 带宽最优（不过 Root Complex → 跨 NUMA） |
| Endpoint | PCIe 末端设备（GPU、NIC、NVMe 都是 Endpoint） | 每个 Endpoint 有 BAR（Base Address Register）暴露自己的内存空间给主机 |
| BAR（基地址寄存器） | Endpoint 将自己的内存映射到 CPU 地址空间的窗口 | GPU 的 BAR1 将 HBM 映射出来 → NIC/NVMe 的 DMA 引擎通过 BAR1 直接访问 GPU HBM = GPUDirect 的基础 |
| IOMMU / ATS | 地址翻译：将虚拟地址转为 DMA 物理地址。ATS 让设备自己做翻译 | GPUDirect RDMA 时 NIC 需要知道 GPU HBM 的物理地址——IOMMU/ATS 负责翻译 |

理解 PCIe 拓扑至关重要：`nvidia-smi topo -m` 显示 GPU/NIC/NVMe 之间的连接关系。如果 NIC 和 GPU 分属不同 NUMA 节点（跨 Root Complex），GPUDirect RDMA 带宽可能只有一半。

### 2.6 NVMe SSD：存储设备

| | |
|---|---|
| **内部结构** | · Controller：管理读写请求队列（NVMe 支持 64K 队列 × 64K 深度），执行 FTL（Flash Translation Layer）将逻辑地址映射到 NAND 物理页。 |
| | · DMA 引擎：从 NAND 读出的数据通过 DMA 写入主机内存（或通过 GDS 直接写入 GPU BAR1）。 |
| | · NAND Flash：实际存储介质。顺序读 ~7–14 GB/s，随机读受 IOPS 限制。 |
| **推理中的角色** | · 模型权重持久化存储（百 GB 级 safetensors 文件） |
| | · KV Cache 归档层（最冷的 KV 可以持久化到 NVMe，下次同 prompt 请求时预取） |
| | · GPUDirect Storage（GDS）让 NVMe 的 DMA 引擎直接将数据写入 GPU HBM（通过 PCIe BAR1），完全绕过 CPU DRAM |

### 2.7 CXL Type 3 内存扩展设备

| | |
|---|---|
| **内部结构** | · CXL Controller：处理 CXL.mem 协议请求（类似内存控制器），将 host 的 load/store 指令转为 DDR 读写。 |
| | · DDR DRAM 颗粒：与普通服务器内存相同的 DRAM 芯片。每条 CXL 模组 128–256 GB。 |
| | · PCIe 5.0 物理接口：CXL 复用 PCIe 物理层（同样的连接器），但运行 CXL 协议而非 PCIe 协议。 |
| **与普通 DRAM 的区别** | · 延迟稍高：~150–300 ns（vs DDR5 ~80 ns），因为多了一层 CXL 协议处理 |
| | · 容量优势：单节点可挂多条 CXL 模组达到 TB 级（DDR 插槽数有限） |
| | · 对 OS 透明：出现为一个新的 NUMA 节点，应用可通过 `numactl` 绑定 |
| **推理中的角色** | KV Cache 溢出池：热 KV 在 GPU HBM，温 KV 在 CXL 内存（按需预取），冷 KV 在 NVMe。CXL 的 ~100 GB/s 带宽远优于 NVMe 的 ~14 GB/s，预取延迟对 TBT 影响可忽略。 |

### 2.8 节点整体拓扑：所有组件如何组织在一起

::: tip 以 DGX A100（8 × A100 + 4 × ConnectX-6）为例
理解拓扑的核心问题：任意两个组件之间的数据要走哪条路？带宽多少？经过几跳？
:::

| 连接 | 通道 | 带宽 | 用途 |
|------|------|------|------|
| GPU ↔ GPU（同组 4 卡） | NVLink（经 NVSwitch 全互联） | 600 GB/s（A100） | TP All-Reduce、GPUDirect P2P |
| GPU ↔ CPU | PCIe 4.0 x16 | 32 GB/s 单向 | KV swap（cudaMemcpy H2D/D2H）、控制面通信 |
| GPU ↔ NIC（同 PCIe switch） | PCIe 4.0 via switch | ~25 GB/s（扣除协议开销） | GPUDirect RDMA（GPU HBM ↔ NIC 直传） |
| GPU ↔ NIC（跨 NUMA） | PCIe 4.0 via 2 × Root Complex | ~12–16 GB/s | GPUDirect RDMA 跨 NUMA（性能下降 30–50%） |
| GPU ↔ NVMe（同 PCIe switch） | PCIe 4.0 via switch | ~14 GB/s | GPUDirect Storage（NVMe DMA 直写 GPU） |
| CPU ↔ CXL 内存 | CXL 3.0 via PCIe 5.0 | ~64–100 GB/s | KV Cache 溢出到 CXL DRAM |
| CPU ↔ CPU（NUMA 互联） | UPI / Infinity Fabric | ~40–80 GB/s | 跨 NUMA 内存访问（尽量避免） |
| NIC ↔ 网络 ↔ 远端 NIC | InfiniBand / RoCE | 400 Gb/s = ~50 GB/s | 跨节点 RDMA 通信 |

#### 节点内数据路径优先级

1. GPU ↔ GPU → 优先走 NVLink（快 10x）
2. GPU ↔ NIC → 必须走 PCIe（NIC 没有 NVLink 接口）
3. GPU ↔ NVMe → 必须走 PCIe（NVMe 没有 NVLink 接口）
4. GPU ↔ CXL → 目前经 CPU 代理或 GPU BAR 映射（PCIe 通道）

#### NUMA Affinity 的核心原则

- 同一 PCIe switch 下的设备通信带宽最高、延迟最低
- 跨 NUMA（跨 CPU socket）的 PCIe 通信要过 UPI → 带宽减半、延迟翻倍
- 关键实践：GPU0 应与其配对的 NIC、NVMe 在同一 NUMA 节点
- 验证工具：`nvidia-smi topo -m`（看连接类型）+ `numactl -H`（看 NUMA 布局）

---

## 两部分的关联

Part 1 告诉你"推理在计算什么、为什么要这样算" → 这决定了数据的种类（权重、KV Cache、激活、AllReduce payload）和大小。

Part 2 告诉你"数据在什么硬件上存储和传输" → 这决定了每种数据走什么物理路径、带宽多少、延迟多少。

两者结合：你就能对推理全链路的每一步回答"多少字节的数据、从哪个组件、经过什么通道、到达哪个组件、耗时多少" —— 这就是 IO 优化的起点。

---

*推理基础 · Transformer 原理 · GPU / NIC / NVLink / PCIe / NVMe / CXL 硬件架构 · 节点拓扑*
