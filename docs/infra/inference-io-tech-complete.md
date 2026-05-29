---
title: 推理 IO 优化：完整技术全景
description: 从芯片内到跨数据中心的全部 IO 相关技术，每项技术说明是什么、为什么需要、用在推理的哪个环节
---

# 推理 IO 优化：完整技术全景

覆盖从芯片内到跨数据中心的全部 IO 相关技术，每项技术说清楚：是什么、为什么需要、用在推理的哪个环节

## Layer 1: 芯片内 IO 优化 — 减少 HBM 读写总量

*算子级*

GPU 计算的最内层瓶颈：Tensor Core 算力 312 TFLOPS，但 HBM 带宽只有 2 TB/s。Decode 阶段每步都要完整读取权重 + KV Cache，算力利用率不足 5%。这一层的所有技术目标一致：减少从 HBM 搬到 SM 的字节数，或者让搬运更高效。

### Flash Attention

| | |
|---|---|
| **是什么** | 重写 Attention 计算的内存访问模式：将 Q/K/V 分块（tiling）加载到 SM 的 SRAM（shared memory，~200 KB/SM），在 SRAM 内完成 softmax + matmul，避免将中间矩阵（attention score matrix，大小 seq×seq）写回 HBM 再读回来。 |
| **关键数字** | 标准 Attention HBM 读写量 = O(N²·d)；Flash Attention = O(N·d)。4096 token 输入，HBM 访问量从 ~4 GB 降到 ~100 MB（减少 40x）。 |
| **为什么推理需要它** | Prefill 阶段处理完整输入序列，seq×seq 的 attention matrix 对 HBM 带宽的消耗巨大。Flash Attention 让 Prefill 的 IO 瓶颈大幅缓解，使其回归 compute-bound，GPU 利用率从 ~30% 提升到 ~70%。 |
| **用在哪里** | Prefill 阶段的 self-attention 计算。Decode 阶段因为每步只有 1 个 query token，attention matrix 是 1×seq 的向量，Flash Attention 的收益较小（但 Flash-Decoding 针对 decode 做了优化）。 |

### GQA / MQA — 从架构层面减少 KV Cache 体积

| 注意力类型 | KV Head 数 | KV Cache 大小（Llama-3 70B, 4096 seq, FP16） | HBM 带宽节省 | 代表模型 |
|------------|------------|-----------------------------------------------|--------------|----------|
| MHA（Multi-Head Attention） | = Q Head 数（64） | 2 × 80 × 64 × 128 × 4096 × 2B = 80 GB | 基线 | GPT-3, OPT |
| GQA（Grouped-Query Attention） | Q Head / G（如 64/8=8） | 2 × 80 × 8 × 128 × 4096 × 2B = 10 GB | 减少 8x | Llama-2/3 70B, Gemma |
| MQA（Multi-Query Attention） | 1 | 2 × 80 × 1 × 128 × 4096 × 2B = 1.25 GB | 减少 64x | PaLM, Falcon, StarCoder |

GQA/MQA 是模型架构层面的 IO 优化——直接减少 KV Cache 的物理大小。Decode 每步读 KV 的字节数成比例下降，TBT 随之改善。这不是系统工程师能改的（模型训练时决定），但必须理解它，因为它决定了 KV Cache 的绝对 IO 量。

### KV Cache Quantization — 运行时压缩 KV 精度

| | |
|---|---|
| **是什么** | 将 KV Cache 从 FP16（2 bytes）量化到 INT8（1 byte）或 FP8/INT4，Decode 读取时反量化。KV 体积直接减半或减至 1/4。 |
| **关键数字** | FP16 → INT8：KV IO 减半，TBT 理论提速 ~30%（KV 读取占 decode 总 IO 的约 60%）。FP16 → INT4：KV IO 减至 1/4，但精度损失更大。 |
| **为什么推理需要它** | 与 GQA 互补：GQA 在架构层减少 KV head 数，量化在运行时减少每个 element 的字节数。两者叠加可以让 KV Cache IO 降低 16–64x。 |
| **用在哪里** | Decode 阶段 KV Cache 的存储和读取。vLLM 和 TRT-LLM 都支持 FP8/INT8 KV Cache。量化后 KV 还更容易装进 HBM，减少溢出到 CXL/CPU 的概率。 |

### Weight Quantization — 减少权重读取 IO

| | |
|---|---|
| **是什么** | 将模型权重从 FP16 量化到 INT8/INT4/FP8（AWQ、GPTQ、SmoothQuant 等）。Decode 每步读取的权重字节数成比例下降。 |
| **关键数字** | Llama-3 70B FP16 = 140 GB，INT4 = 35 GB。Decode 每步权重 IO 从 35 GB/卡（TP=4）降到 ~8.75 GB/卡。 |
| **为什么推理需要它** | Decode 的 HBM 读取 = 权重 + KV Cache。权重量化直接减少前半部分。INT4 权重 + INT8 KV + GQA 组合后，decode 每步 HBM 读取可从 37.5 GB 降到 ~10 GB，TBT 从 ~47 ms 降到 ~12 ms。 |
| **用在哪里** | 模型加载时权重已量化存储。Decode 每步的矩阵向量乘（GEMV）在 Tensor Core 上执行 INT8/FP8 运算或反量化后 FP16 运算。 |

### Speculative Decoding — 改变 Decode 的 IO 模式

| | |
|---|---|
| **是什么** | 用一个小模型（draft model）快速生成 K 个候选 token，再用大模型一次性并行验证。大模型的验证步是 Prefill 模式（K+1 token 并行），而非逐 token Decode 模式。 |
| **关键数字** | 假设 draft model 接受率 70%，K=5：平均每次大模型调用生成 ~3.5 个 token，但只做 1 次权重读取。等效 TBT 降至原来的 1/3.5。 |
| **为什么它是 IO 优化** | Decode 的核心瓶颈是"每生成 1 个 token 就要读一遍全部权重"。Speculative Decoding 将多个 token 的生成合并为一次权重读取，本质上是用算力换 IO——把 decode 的 GEMV（内存密集）变成 GEMM（计算密集），大幅提升 HBM 带宽利用率。 |
| **用在哪里** | Decode 阶段。draft model 可以是小模型、模型的早期 exit 层、或基于 n-gram 的 lookup。vLLM、TRT-LLM、Medusa 等都支持。 |

### Kernel Fusion — 减少 HBM 往返次数

| | |
|---|---|
| **是什么** | 将多个连续算子合并为一个 CUDA kernel。例如 LayerNorm + Linear + GELU 融合后，中间激活不需要写回 HBM 再读回来，全程在 SM 寄存器/shared memory 中完成。 |
| **关键数字** | 每次 HBM 往返（写+读）的延迟 ~1 μs + 带宽开销。一个 Transformer 层有 ~10 个算子，融合可减少 ~60% 的中间 HBM 读写。 |
| **为什么推理需要它** | 推理不需要保留中间激活用于反向传播（训练才需要），因此可以更激进地融合。TensorRT 的 Layer Fusion 和 TRT-LLM 的 fused kernel 是典型应用。 |
| **用在哪里** | Prefill 和 Decode 全阶段。常见融合：QKV projection 融合、attention + softmax 融合（Flash Attention 就是一种）、FFN 的 gate/up + SiLU 融合、RMSNorm + residual 融合。 |

---

## Layer 2: GPU 内存管理 — KV Cache 怎么放、怎么调度

*内存级*

HBM 容量有限（A100 80 GB，H100 80 GB），权重占 ~35 GB（TP=4），剩余空间全部给 KV Cache。怎么管理这 ~40 GB 的 KV 空间，直接决定能服务多少并发请求。

### PagedAttention — KV Cache 的虚拟内存

| | |
|---|---|
| **是什么** | 借鉴操作系统的虚拟内存分页机制：将 KV Cache 分成固定大小的物理块（如 16 tokens/block），用 block table 做虚拟→物理映射。请求的 KV 不需要连续物理内存，按需分配块，结束后回收。 |
| **关键数字** | 传统连续分配浪费 60–80% 内存（预分配 max_seq_len）。PagedAttention 将浪费降到 <4%（最后一个块的内部碎片）。等效并发容量提升 2–4x。 |
| **为什么它是 IO 关键** | 1. KV 块的分配/回收/swap 触发所有 KV 相关的 IO 操作（HBM↔CPU、HBM↔CXL、HBM↔NVMe）。2. block table 的虚拟→物理映射决定了 KV 读取的内存访问模式——如果块分散，实际 HBM 带宽利用率（MBU）会下降。3. 跨请求的 prefix 共享（如系统 prompt）通过 copy-on-write block 实现，直接减少重复 KV 的 IO。 |
| **用在哪里** | vLLM 的 block_manager 管理所有 KV 的分配。Decode 每步的 attention kernel 通过 block table 间接寻址 KV。Swap（HBM↔CPU）和 KV 迁移（跨节点）都以 block 为最小单位。 |

### CUDA Memory Pool / cudaMallocAsync — 减少内存分配开销

| | |
|---|---|
| **是什么** | CUDA 11.2+ 的异步内存池：预分配一大块 HBM，后续的 malloc/free 在池内完成（无系统调用），且可以与 CUDA stream 异步执行。PyTorch 的 CUDACachingAllocator 基于类似思想。 |
| **与推理 IO 的关系** | 推理的 KV Cache、激活 buffer 频繁分配释放。每次 cudaMalloc 是同步系统调用（~50 μs），Decode 每步都调用会严重拖慢 TBT。内存池将分配延迟从 ~50 μs 降到 ~0.1 μs。vLLM 在启动时预分配整个 KV Cache 池，运行时只做块表更新。 |

### Pinned Memory（Page-locked Memory）

| | |
|---|---|
| **是什么** | 用 cudaMallocHost() 分配的 CPU 内存，物理页被锁定不会被操作系统 swap 到磁盘。GPU DMA 引擎可以直接访问这块内存，无需 CPU 先做虚拟→物理地址翻译。 |
| **与推理 IO 的关系** | 1. KV Cache swap：vLLM 的 swap_out 将 KV 从 GPU 搬到 CPU pinned memory，swap_in 搬回来。非 pinned memory 的 H2D/D2H 带宽只有 pinned 的 ~50%（因为需要中间 staging buffer）。2. GPUDirect RDMA 要求：ibv_reg_mr 注册的 GPU 内存区域必须是 page-locked 的，否则 NIC 无法 DMA。3. GDS 要求：cuFile 要求 GPU buffer 是对齐的、且映射到 BAR1 的区域。 |

### Continuous Batching + Chunked Prefill — IO 调度策略

| | |
|---|---|
| **是什么** | 传统 static batching：等一个 batch 全部完成才处理下一个。Continuous Batching：某个请求完成就立即填入新请求，GPU 永远不空转。Chunked Prefill：将长输入分成小块（如 512 token/chunk），与 decode token 混合在同一 batch 中执行。 |
| **与 IO 的关系** | 1. 决定了 HBM 中同时存在多少请求的 KV Cache → 直接影响 KV 总 IO 量和溢出概率。2. Chunked Prefill 避免长 prefill 独占 GPU 导致 decode 请求的 TBT 飙升（Head-of-Line blocking）。3. 调度器（scheduler）决定何时触发 KV swap/预取 — 是 IO 操作的触发源。 |

---

## Layer 3: 节点内互联 — GPU 之间、GPU 与 CPU/存储之间

*节点级*

### PCIe — 节点内一切互联的物理基础

| 版本 | 单向带宽/lane | x16 单向带宽 | 双向带宽 | 推理中的角色 |
|------|---------------|--------------|----------|--------------|
| PCIe 3.0 | ~1 GB/s | ~16 GB/s | ~32 GB/s | 旧集群的 GPU↔CPU 通道 |
| PCIe 4.0 | ~2 GB/s | ~32 GB/s | ~64 GB/s | A100 默认 CPU↔GPU 通道 |
| PCIe 5.0 | ~4 GB/s | ~64 GB/s | ~128 GB/s | H100/CXL 设备的物理层 |
| PCIe 6.0 | ~8 GB/s | ~128 GB/s | ~256 GB/s | 下一代 CXL 3.0+ 的物理层 |

PCIe 是 CPU↔GPU、GPU↔NIC、GPU↔NVMe、CPU↔CXL 的共同物理层。它是 GPUDirect 系列技术的承载通道，也是 CXL 的底层传输层。理解 PCIe 拓扑（哪些设备共享 PCIe switch、NUMA affinity）对 IO 路径设计至关重要——`nvidia-smi topo -m` 是必用工具。

### NVLink / NVSwitch — GPU 间高速直连

| 版本 | 单链路带宽 | 链路数/GPU | 总双向带宽/GPU | 典型平台 |
|------|------------|------------|----------------|----------|
| NVLink 3.0 | 50 GB/s | 12 | 600 GB/s | A100（DGX A100 8 卡全互联） |
| NVLink 4.0 | 50 GB/s | 18 | 900 GB/s | H100（DGX H100 8 卡全互联） |
| NVLink 5.0（Grace Blackwell） | 50 GB/s | 18+ | 1.8 TB/s（NVLink + NVLink-C2C） | GB200 NVL72 |

**NVLink 是什么**：GPU 间的高速点对点互联，带宽是 PCIe 的 7–14 倍。支持 GPU 之间直接读写对方的 HBM（load/store 语义），无需 CPU 参与。

**NVSwitch 是什么**：NVLink 交换芯片：让节点内所有 GPU 两两全互联（all-to-all），而非只能点对点。DGX 节点中 8 卡通过 NVSwitch 实现任意两卡间全速通信。

推理中的作用：TP（Tensor Parallel）的 All-Reduce 在同节点内走 NVLink/NVSwitch。NCCL 自动检测 NVLink 拓扑并选择 NVLink 路径。如果 NVLink 不可用（如非 DGX 节点），NCCL 退化为 PCIe P2P（带宽降 7x）。

### GPUDirect P2P — GPU 间零拷贝直传

| | |
|---|---|
| **是什么** | 允许一个 GPU 通过 NVLink 或 PCIe 直接读写另一个 GPU 的 HBM。cuMemcpy 在两个 GPU context 之间执行时，数据通过 DMA 引擎直接传输，CPU 不接触数据。 |
| **底层机制** | GPU A 的 DMA 引擎通过 NVLink 或 PCIe BAR1 映射访问 GPU B 的 HBM 物理地址。CUDA 驱动通过 cuDeviceCanAccessPeer() 检查两卡是否支持 P2P，cuCtxEnablePeerAccess() 启用。 |
| **推理中的作用** | 1. TP All-Reduce：NCCL 在节点内使用 P2P + NVLink 实现 All-Reduce/All-Gather，每层 2 次。2. KV Cache 跨卡共享：某些 KV 分片可能在邻卡，P2P 读取避免 CPU 中转。3. Pipeline Parallel 节点内：激活张量从 stage i 的 GPU 传到 stage i+1 的 GPU。 |
| **关键配置** | nvidia-smi topo -m 查看 P2P 连接类型（NV18 = NVLink 18 lanes, PIX = PCIe switch）。若 P2P 跨 NUMA 节点且无 NVLink，带宽可能只有 ~10 GB/s（vs NVLink 900 GB/s）。 |

### GPUDirect Storage（GDS）— 存储直通 GPU

| | |
|---|---|
| **是什么** | NVMe SSD 的 DMA 控制器直接将数据写入 GPU HBM（通过 PCIe BAR1 映射），完全绕过 CPU DRAM。NVIDIA cuFile API 提供编程接口（cuFileRead / cuFileWrite）。 |
| **关键数字** | 传统路径：NVMe → CPU → GPU，受限于 CPU 内存拷贝，实际 ~7 GB/s。GDS：NVMe → GPU 直传，~14 GB/s（PCIe 5.0 NVMe）。多路 NVMe RAID0 + GDS 可达 ~100 GB/s。 |
| **推理中的作用** | 1. 模型冷启动加速：百 GB 权重从 NVMe 直接加载到 GPU，启动时间从分钟级降到秒级。2. KV Cache 持久化/预取：长上下文对话的 KV 可以归档到 NVMe，下次请求时通过 GDS 直接预取到 GPU，无需重新 prefill。3. Checkpoint 恢复：推理服务故障恢复时快速重建模型状态。 |
| **依赖条件** | 内核模块 nvidia-fs.ko，cuFile 库，支持 GDS 的文件系统（ext4/XFS/GDS-aware）或裸块设备。CUDA 11.4+。 |

### CXL（Compute Express Link）— 缓存一致的内存扩展

| | |
|---|---|
| **是什么** | 建立在 PCIe 5.0/6.0 物理层之上的协议族，提供三个子协议：CXL.io（设备发现，等同 PCIe）、CXL.cache（设备缓存 host 内存）、CXL.mem（host 访问设备侧内存）。推理最关心 CXL.mem Type 3：外挂大容量 DRAM 模组，CPU/GPU 通过 load/store 指令像访问本地内存一样访问。 |
| **关键数字** | 单通道 CXL 3.0：~64 GB/s（PCIe 5.0 x16）。延迟：~150–300 ns（vs DDR5 ~80 ns，HBM ~100 ns）。单条 CXL DIMM：256 GB。单节点可扩展到数 TB。 |
| **推理中的作用** | 1. KV Cache 容量扩展：HBM 80 GB 不够时，温热 KV 放 CXL 内存（TB 级），按需预取到 HBM。比 swap 到 CPU DRAM 带宽更高（CXL 可多通道），比 NVMe 快 10x。2. 权重缓存：模型权重常驻 CXL 内存，冷启动时从 CXL 加载（~100 GB/s vs NVMe ~14 GB/s）。3. Memory Pooling（CXL 2.0+）：多节点共享一个 CXL 内存池，动态分配 KV 容量——忙的节点多分，闲的节点少分。 |
| **当前状态** | 2024–2025 年进入量产（三星 CMM-D、Micron CZ120）。Linux 内核 6.3+ 支持 CXL 设备枚举和 NUMA 绑定。GPU 直接访问 CXL 内存目前需要通过 CPU 代理或 NVIDIA Grace CPU 的 NVLink-C2C 通道。 |

### DMA 引擎 / IOMMU — 所有零拷贝技术的底层

GPUDirect 系列（P2P、RDMA、Storage）的本质都是让外设的 DMA 引擎直接访问 GPU HBM。IOMMU 负责地址翻译（虚拟→物理），ATS（Address Translation Service）让设备自己做翻译而不用 CPU 参与。理解 DMA 和 IOMMU 是理解所有零拷贝路径的前提——每个 GPUDirect 优化本质上都是"让 DMA 能直接到达目标内存"。

---

## Layer 4: 跨节点网络通信 — 多节点推理的 IO 路径

*网络级*

### RDMA（Remote Direct Memory Access）

| | |
|---|---|
| **是什么** | 网卡（NIC/HCA）直接读写远端机器的内存，CPU 不参与数据路径。操作类型：RDMA Write（单边写）、RDMA Read（单边读）、Send/Recv（双边）。传输可靠性通过 RC（Reliable Connection）QP 保证。 |
| **两种网络实现** | InfiniBand：专用 RDMA 网络，延迟最低（~1 μs），带宽最高（400 Gb/s NDR）。RoCE v2：在标准以太网上跑 RDMA（UDP 封装），成本低但需要 PFC/ECN 拥塞控制配合。 |
| **推理中的全部使用场景** | 1. 跨节点 TP All-Reduce：NCCL 底层使用 RDMA 传输 All-Reduce 数据（通过 GPUDirect RDMA）。2. 跨节点 PP 激活传递：Pipeline Parallel 层间的激活张量通过 RDMA Send/Recv 或 Write 传输。3. Disaggregated Inference 的 KV 迁移：Prefill 节点 → Decode 节点的 KV Cache 传输，延迟直接叠加到 TTFT。4. 分布式 KV Cache Pool：多节点共享 KV 时，远程 KV 块通过 RDMA Read 获取。5. 模型权重广播：模型更新时新权重的跨节点分发。 |
| **关键 API** | ibv_reg_mr（注册内存区域）→ ibv_create_qp（创建队列对）→ ibv_post_send（发起 RDMA Write/Read）→ ibv_poll_cq（轮询完成事件）。生产系统通常不直接用 verbs，而是通过 UCX 或 NCCL 间接使用。 |

### GPUDirect RDMA — 让 RDMA 直达 GPU HBM

| | |
|---|---|
| **是什么** | 标准 RDMA 只能访问 CPU 内存。GPUDirect RDMA 通过 nvidia-peermem 内核模块，让 NIC 的 DMA 引擎直接访问 GPU HBM 的 BAR1 映射区域。数据路径：GPU HBM ↔ PCIe ↔ NIC ↔ 网络，CPU 完全不接触数据。 |
| **与普通 RDMA 的区别** | 普通 RDMA：GPU HBM → CPU DRAM（cudaMemcpy D2H）→ NIC → 网络 → NIC → CPU DRAM → GPU HBM（cudaMemcpy H2D）。GPUDirect RDMA：GPU HBM → NIC → 网络 → NIC → GPU HBM。省掉两次 PCIe 拷贝（~35 ms 每次），延迟从 ~6 ms 降到 ~2 ms（含网络）。 |
| **推理中的作用** | 1. NCCL 跨节点通信的底层路径：ncclAllReduce 在检测到 GPU 和 NIC 在同一 PCIe switch 下时自动使用 GPUDirect RDMA。2. KV Cache 迁移：nixl/mooncake 基于 GPUDirect RDMA 做 GPU-to-GPU 的 KV 传输。3. Expert Parallel（MoE 模型）的 All-to-All：token 路由到不同节点的 expert，GPUDirect RDMA 减少跨节点延迟。 |
| **依赖条件** | nvidia-peermem 内核模块（CUDA 11.4+ 自带）；GPU 和 NIC 必须在同一 PCIe root complex 或通过 PCIe switch 直连（否则跨 NUMA 性能下降 30–50%）；NIC 必须支持 PeerDirect（Mellanox/NVIDIA ConnectX-5+）。 |

### DPU / SmartNIC（BlueField）— IO 路径卸载

| | |
|---|---|
| **是什么** | NVIDIA BlueField-3 DPU = ConnectX-7 NIC + 16 个 Arm 核 + 专用加速器。可以将网络协议处理、存储 IO 路径、安全加密等全部从 host CPU 卸载到 DPU 上运行。 |
| **推理中的作用** | 1. GDS 卸载：存储 IO 的控制路径由 DPU 的 Arm 核处理，host CPU 和 GPU 完全不被 IO 中断打扰。2. 网络虚拟化卸载：多租户推理集群中，DPU 处理 SR-IOV / VxLAN 等网络虚拟化，RDMA 性能不受虚拟化开销影响。3. KV 迁移控制面：DPU 可以运行 KV 迁移的调度逻辑，GPU 只关心计算。 |

---

## Layer 5: 通信库栈 — 从 verbs 到框架 API

*软件栈*

硬件提供了 RDMA、NVLink、PCIe 等物理通道，通信库将它们抽象为可编程的 API。推理框架不会直接调 ibv_post_send，而是通过多层抽象间接使用。

| 层级 | 库 | 核心职责 | 推理中怎么用 | 关键接口 / 环境变量 |
|------|-----|----------|--------------|---------------------|
| L0 硬件驱动 | libibverbs / rdma-core / MLNX_OFED | RDMA NIC 的用户态驱动，提供 QP/MR/CQ 原语 | 自定义 KV 迁移通道的最底层接口 | ibv_post_send, ibv_reg_mr, ibv_poll_cq |
| L0 硬件驱动 | nvidia-peermem（内核模块） | 让 NIC DMA 访问 GPU BAR1 映射 | GPUDirect RDMA 的内核基础 | modprobe nvidia-peermem |
| L0 硬件驱动 | nvidia-fs（内核模块） | 让 NVMe DMA 访问 GPU BAR1 映射 | GPUDirect Storage 的内核基础 | cuFile API |
| L1 传输抽象 | UCX（Unified Communication X） | 自动在 RDMA / SHM / TCP 间选最优传输 | torch.distributed 的可选后端；vLLM 多进程通信 | UCX_TLS=rc,cuda_copy,cuda_ipc  UCX_NET_DEVICES=mlx5_0:1 |
| L1 传输抽象 | libfabric（OFI） | 另一种传输抽象层（AWS EFA 使用） | AWS 推理集群的底层传输 | FI_PROVIDER=efa |
| L2 集合通信 | NCCL | GPU 集合通信（All-Reduce / All-Gather / Send-Recv） | TP All-Reduce、PP 激活传递、EP All-to-All | NCCL_ALGO=Ring/Tree  NCCL_PROTO=Simple/LL/LL128  NCCL_NET_GDR_LEVEL=5 |
| L2 集合通信 | Gloo | CPU 集合通信（TCP/SHM） | 控制面通信（barrier、metadata 广播）；非 GPU 数据交换 | torch.distributed backend='gloo' |
| L3 自定义传输 | nixl（NVIDIA） | KV Cache 跨节点 RDMA 传输（块级 DMA pipeline） | Disaggregated Inference 的 KV 迁移 | nixl_xfer_send / nixl_xfer_recv |
| L3 自定义传输 | mooncake（Moonshot AI） | KV Cache 传输 + 分布式 KV 存储 | 长上下文推理的 KV 共享池 | mooncake transfer engine |
| L4 框架层 | torch.distributed | process group 管理 + 集合通信 Python API | 推理框架初始化 TP/PP group，调用 all_reduce | init_process_group(backend='nccl') |
| L4 框架层 | vLLM distributed | 封装 torch.distributed + 自定义 P2P | TP/PP group 划分、KV 迁移触发 | vllm/distributed/parallel_state.py |

::: tip NCCL 的核心地位
NCCL 是推理通信的枢纽：向上给 torch.distributed 提供 all_reduce/send/recv 接口，向下自动选择最优物理路径（NVLink P2P / PCIe P2P / GPUDirect RDMA / host staging）。
理解 NCCL 的传输选择逻辑（NCCL_NET_GDR_LEVEL、NCCL_P2P_LEVEL、ring vs tree 算法选择）是调优跨节点通信延迟的关键。
:::

---

## Layer 6: 并行策略 — 决定"什么数据走什么路径"

*架构级*

并行策略决定了推理中数据切分和通信模式。不同策略产生不同的 IO 需求，是 IO 优化的需求来源。

| 并行策略 | 切分方式 | 产生的 IO 通信 | 通信频率 | 走哪条物理路径 | 影响哪个指标 |
|----------|----------|----------------|----------|----------------|--------------|
| Tensor Parallel（TP） | 每层的权重矩阵按列/行切分到多卡 | 每层 2 次 All-Reduce（attention 后 + FFN 后） | 每层每步（Prefill + Decode 都有） | 节点内：NVLink P2P / 跨节点：GPUDirect RDMA | TBT（All-Reduce 延迟叠加到每步） |
| Pipeline Parallel（PP） | 模型按层分段到不同 GPU/节点 | 层间激活张量 Send/Recv | 每段边界每步 1 次 | 节点内：P2P / 跨节点：GPUDirect RDMA | TBT + 气泡率（bubble ratio） |
| Expert Parallel（EP / MoE） | 不同 expert 分布在不同 GPU | All-to-All：每个 token 路由到对应 expert 所在 GPU | 每 MoE 层 2 次 All-to-All | 节点内：NVLink / 跨节点：GPUDirect RDMA | TBT（All-to-All 通信量大） |
| Data Parallel（DP / 推理中少用） | 每卡完整模型，不同请求分到不同卡 | 无通信（各卡独立推理） | 无 | 无 | 纯 Throughput 提升 |
| Prefill-Decode 分离 | Prefill 和 Decode 在不同节点组执行 | KV Cache 整体迁移（Prefill → Decode） | 每请求 1 次 | 跨节点：GPUDirect RDMA + RDMA | TTFT（KV 迁移延迟直接叠加） |
| Sequence Parallel（SP） | 序列维度切分（长上下文） | All-Gather + Reduce-Scatter 替代 All-Reduce | 每层每步 | 同 TP 路径 | 长上下文下的通信效率 |

---

## Layer 7: 异步 IO / 计算-通信重叠

*调度级*

前面的技术解决"IO 走哪条路径、用什么带宽"，这一层解决"IO 和计算能不能同时跑"。理想情况：GPU 在做当前层计算时，IO 引擎（DMA/NIC）同时在搬下一层需要的数据。

| 技术 | 机制 | 推理中的应用 | 效果 |
|------|------|--------------|------|
| CUDA Streams（多流并发） | GPU 上的异步执行队列，不同 stream 可并行执行 kernel 和 memcpy | 计算流做 attention/FFN，IO 流同时做 KV prefetch/swap/RDMA 传输 | 计算和 IO 重叠，隐藏 IO 延迟 |
| CUDA Events（流间同步） | 轻量级同步原语，标记某 stream 的某个点，其他 stream 可等待 | 计算流等待 IO 流的 KV prefetch 完成再开始该层计算 | 精确的计算-IO 依赖管理 |
| cudaMemcpyAsync | 异步 H2D/D2H 内存拷贝，不阻塞 host CPU | KV Cache swap_in/swap_out 在后台执行 | CPU 不被阻塞，可继续调度其他请求 |
| cuFile 异步 IO | GDS 的异步版本，IO 请求提交后立即返回，CUDA event 通知完成 | 权重分片异步加载（边加载边计算已加载的层） | 冷启动时间缩短 ~40%（加载与初始化重叠） |
| NCCL 异步集合操作 | NCCL 操作提交到 CUDA stream，与同 stream 的计算串行但与其他 stream 并行 | 当前层 compute + 上一层 All-Reduce 重叠 | TP 的通信开销部分隐藏 |
| 逐层 pipeline KV 传输 | KV Cache 按层逐块 RDMA 传输，而非等全部完成 | Disaggregated Inference 中，首层 KV 到达即开始 decode | TTFT 从 Prefill + 全量迁移 → Prefill + 首层迁移 |
| Double buffering | 两块 buffer 交替使用：一块在被计算使用，另一块在被 IO 填充 | KV prefetch + 权重预加载 | IO 和计算完全流水化 |

---

## Layer 8: 推理框架 — IO 操作的触发点

*框架级*

所有底层 IO 技术最终通过推理框架的代码路径被触发。理解框架的哪个模块触发什么 IO，是优化落地的关键。

| 框架模块 | 触发的 IO 操作 | 使用的底层技术 | 关键代码入口 |
|----------|----------------|----------------|--------------|
| vLLM Scheduler | 决定哪些请求进入/退出 batch → 触发 KV swap_in/swap_out | cudaMemcpyAsync（H2D/D2H） | vllm/core/scheduler.py: _schedule() |
| vLLM BlockManager | 分配/释放 KV 物理块 → 触发 KV 块的内存操作 | CUDA Memory Pool + PagedAttention | vllm/core/block_manager.py |
| vLLM Worker | 执行单卡前向推理 → 触发权重读取 + KV 读写 + All-Reduce | HBM 带宽 + NCCL + GPUDirect P2P | vllm/worker/worker.py: execute_model() |
| vLLM ModelLoader | 启动时加载模型权重到 GPU | GDS (cuFileRead) 或 torch.load + cudaMemcpy | vllm/model_executor/model_loader/ |
| vLLM KV Connector | Disaggregated 模式下 KV 迁移 | GPUDirect RDMA + nixl/自研传输 | vllm/distributed/kv_transfer/ |
| TRT-LLM Runtime | TensorRT 引擎执行 → fused kernel 内的 HBM 访问 | Kernel Fusion + Flash Attention | tensorrt_llm/runtime/ |
| TRT-LLM Plugin | 自定义算子插入点 → 可嵌入 IO 逻辑 | GPUDirect / RDMA / 自定义 DMA | tensorrt_llm/plugin/IPluginV3 |
| SGLang Scheduler | RadixAttention prefix 共享 → 减少重复 KV prefill | KV 块 copy-on-write（GPU HBM 内） | sglang/srt/managers/scheduler.py |
| torch.distributed | init_process_group → 初始化 NCCL communicator | NCCL + UCX/Gloo | torch.distributed.init_process_group() |

---

## Layer 9: 观测与 Profiling — 量化 IO 瓶颈的工具

*工具级*

所有优化的前提是量化瓶颈：哪一段 IO 路径耗时最长？带宽利用率多少？是否有 stall？

| 工具 | 观测什么 | 推理 IO 优化中怎么用 |
|------|----------|----------------------|
| Nsight Systems (nsys) | CUDA kernel 时间线、stream 并发、NCCL 通信、memcpy 时序 | 定位 Decode 每步的 HBM stall、All-Reduce 与 compute 的重叠度、IO 等待气泡 |
| Nsight Compute (ncu) | 单个 kernel 的 HBM 带宽利用率、L2 cache 命中率、SM 占用率 | 分析 attention kernel 的 MBU 是否达标（目标 >60%） |
| nvidia-smi topo -m | GPU 间互联类型（NVLink/PIX/PHB/SYS） | 确认 TP 通信走 NVLink 还是 PCIe，NUMA affinity 是否正确 |
| nvidia-smi dmon | GPU 功耗、温度、SM 利用率、HBM 带宽实时监控 | decode 阶段如果 SM 利用率 <10% 但 HBM BW 高 → 确认是 IO-bound |
| ib_send_bw / ib_write_bw | RDMA 点对点带宽和延迟 | 验证 GPUDirect RDMA 带宽是否达到线速（如 200 Gb/s → ~25 GB/s） |
| ib_send_lat / ib_write_lat | RDMA 单向延迟 | 验证 KV 迁移延迟是否达标 |
| perftest --use_cuda | GPUDirect RDMA 模式下的带宽/延迟 | 对比 GPU 内存 vs CPU 内存的 RDMA 性能差异 |
| nccl-tests (all_reduce_perf) | NCCL 集合通信带宽（bus BW、algo BW） | 验证 TP All-Reduce 的实际带宽是否达到理论值 |
| fio / GDS benchmark | NVMe 顺序/随机读写带宽和 IOPS | 验证 GDS 路径的模型加载带宽 |
| numastat / numactl | NUMA 内存分配分布 | 确认 KV Cache / GPU buffer 是否分配在正确的 NUMA 节点 |
| perf / bcc (eBPF) | CPU 调用栈、系统调用延迟、上下文切换 | 排查 CPU 侧 IO 调度瓶颈（如非预期的内核 memcpy） |
| DCGM (Data Center GPU Manager) | 集群级 GPU 指标（利用率、ECC 错误、NVLink 带宽） | 生产环境持续监控推理集群的 IO 健康度 |

---

## 全部技术定位一览

| 技术 | 层级 | 一句话定位 | 影响的指标 |
|------|------|------------|------------|
| Flash Attention | 芯片内 | Attention 分块计算，将 HBM 访问量从 O(N²) 降到 O(N) | TTFT |
| GQA / MQA | 芯片内（模型架构） | 减少 KV Head 数 → KV Cache 体积缩小 8–64x | TBT, 内存容量 |
| KV Cache Quantization | 芯片内（运行时） | KV 精度降低 → 每步 IO 字节数减半或更多 | TBT, 内存容量 |
| Weight Quantization | 芯片内（运行时） | 权重精度降低 → Decode 每步权重 IO 减半或更多 | TBT |
| Speculative Decoding | 芯片内（算法） | 多 token 合并为一次权重读取，GEMV → GEMM | TBT |
| Kernel Fusion | 芯片内 | 合并算子减少中间激活的 HBM 往返 | TTFT, TBT |
| PagedAttention | 内存管理 | KV Cache 分页管理，碎片率 <4%，容量利用率 >96% | Throughput, 内存容量 |
| CUDA Memory Pool | 内存管理 | 异步内存分配，延迟从 50 μs 降到 0.1 μs | TBT（减少分配 stall） |
| Pinned Memory | 内存管理 | Page-locked CPU 内存，DMA 可直接访问 | Swap 带宽, GDR 前提 |
| Continuous Batching | 调度 | 请求级动态 batching，GPU 不空转 | Throughput |
| Chunked Prefill | 调度 | 长 prefill 分块，避免 decode 饥饿 | TBT（避免 HoL blocking） |
| PCIe | 节点内互联 | CPU↔GPU、GPU↔NIC、GPU↔NVMe 的物理层 | 所有节点内 IO 的基础 |
| NVLink / NVSwitch | 节点内互联 | GPU 间直连 900 GB/s（vs PCIe 64 GB/s） | TP All-Reduce 延迟 |
| GPUDirect P2P | 节点内互联 | GPU 间 DMA 直传（走 NVLink 或 PCIe） | 节点内 TP/PP 通信 |
| GPUDirect Storage | 节点内 IO | NVMe DMA 直写 GPU，绕过 CPU | 冷启动, KV 持久化 |
| GPUDirect RDMA | 跨节点 | NIC DMA 直达 GPU HBM，CPU 旁路 | 跨节点 TP/PP, KV 迁移 |
| RDMA (IB/RoCE) | 跨节点 | 网卡直接读写远端内存 | 跨节点通信延迟/带宽 |
| DPU / SmartNIC | 跨节点 | IO 控制面卸载，GPU 专注计算 | IO 中断隔离 |
| CXL | 内存扩展 | TB 级低成本内存池，缓存一致访问 | KV 容量, Throughput |
| NCCL | 通信库 | GPU 集合通信的统一抽象 | TP/PP/EP 通信 |
| UCX | 通信库 | 传输层自动选优（RDMA/SHM/TCP） | 分布式后端灵活性 |
| nixl / mooncake | 通信库 | KV Cache 专用 RDMA 传输 | Disagg KV 迁移延迟 |
| CUDA Streams | 异步调度 | IO 与 compute 并行执行 | 隐藏 IO 延迟 |
| Double Buffering | 异步调度 | 两块 buffer 交替使用，IO 和计算流水化 | 隐藏预取延迟 |
| Prefill-Decode 分离 | 系统架构 | Prefill 和 Decode 独立节点池，各自优化 | TTFT + Throughput |

---

*推理 IO 优化完整技术全景 · 25 项技术 · 9 层架构*
