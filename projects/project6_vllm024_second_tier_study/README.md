# vLLM 0.24.0 Second-tier 实验研究

本目录用于回答一个具体问题：

> 在明确限定为“已经会使用 second tier 的 workload”后，`uring-slab`
> 相对 vLLM 0.24.0 原生 FS tier 是否能降低 revisit TTFT、提高 SLO
> goodput，优势来自哪里，以及上层调度是否会吞掉纯 I/O 优势。

## 先给结论

1. **不建议在“second tier 在真实业务里是否常见”上投入大量实验。**
   如果最终 claim 是“在会深度复用、且 KV 已被挤出 GPU/CPU 的场景中，
   uring-slab 是更好的 secondary backend”，那么只需要一个很小的触发验证，
   不需要证明这个场景在所有真实业务里普遍存在。
2. **必须做一个最小的上层可用性 gate。** 原因不是为了证明真实 workload，
   而是 vLLM 的 CPU primary 可能因为 store/cascade/load 的 pin 被占满。
   这时即使 FS 命中，promotion 也会被当作 miss，纯 I/O 优势无法进入 TTFT。
3. **微基准必须分两层。**
   - 数据面基准：拆出 file-per-block、Python/线程池、slab、io_uring
     各自的贡献。
   - 完整 tier-contract 基准：直接使用 vLLM 0.24.0 的
     `FileSystemTierManager`，走 `lookup/submit/get_finished` 生命周期。
4. **端到端主指标建议定为 SLO goodput，revisit TTFT 作为第二主指标。**
   原因是 second tier 的价值是用存储资源换 GPU prefill 计算；单用户 TTFT
   能说明机制是否成立，而固定 TTFT/ITL SLO 下可服务的请求率更接近系统价值。
5. **从干净起点估算 4 周，Stage 3 最多占 2 个 GPU 日。**
   如果复用现有 engine、trace 和采集工具，预计可压缩到 2–3 周。

## 文档入口

- [vLLM 0.24.0 源码事实与接口](docs/VLLM_024_ARCHITECTURE.md)
- [实验路线、矩阵、指标与时间线](docs/EXPERIMENT_ROADMAP.md)
- [代码、配置、运行结果管理方案](docs/CODE_MANAGEMENT.md)
- [锁定的上游版本](SOURCE_LOCK.json)

## 建议的研究 claim

推荐先注册一个范围清楚的条件式 claim：

> 对于 prefix KV 的 reuse distance 已超过 GPU 与 CPU primary 容量、
> promotion 能够获得 CPU staging slot、并且 secondary I/O 位于请求关键路径
> 的 workload，预分配 slab + io_uring 的 secondary tier 相对 vLLM 0.24.0
> file-per-block FS tier，能够以更低的 tier job tail latency 和 CPU/GiB
> 提高 revisit TTFT 或 SLO goodput。

这条 claim **不声称**：

- 所有生产 workload 都频繁使用 second tier；
- 所有模型、文件系统和内核上 io_uring 都更快；
- 单纯提升块设备带宽必然等比例提升端到端 TTFT；
- slab 的容量、恢复和运维语义与 FS 的持久文件语义完全等价。

## 总体流程

```mermaid
flowchart LR
    A["P0: 观测漏斗与 contract"] --> B["P1: 强制 spill 触发 gate"]
    B --> C{"promotion 可用?"}
    C -- "否" --> D["先解决 primary pin / backpressure"]
    C -- "是" --> E["P2: 微基准与消融"]
    E --> F{"相邻工作点有稳定优势?"}
    F -- "否" --> G["停止 engine 方向或缩小 claim"]
    F -- "是" --> H["P3: scheduler-aware replay"]
    H --> I{"I/O 优势能穿过调度?"}
    I -- "否" --> D
    I -- "是" --> J["P4: 合成 trace 端到端实验"]
```

P0/P1 是低成本 gate；P2 是核心机制实验；P3 用无 GPU 或最少 GPU 的方式
验证上层影响；只有前三者都通过，才进入昂贵的 P4。

