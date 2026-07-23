# 实验路线：从“会用到 second tier”到端到端收益

## 1. 研究问题拆分

整个工作不要用一张大实验表同时回答所有问题。应拆成四层 claim：

| 层次 | 要回答的问题 | 需要 GPU 吗 | 失败意味着什么 |
|---|---|---:|---|
| C0 接口正确性 | uring-slab 是否完整实现 second-tier contract | 否 | 不能进入性能实验 |
| C1 数据面 | 相同 block I/O 流下，哪里比 FS 快，为什么 | 否 | engine 方向不成立或需缩小范围 |
| C2 调度可达性 | second-tier hit 是否能成功 promotion，优势是否被 primary/scheduler 吞掉 | 大部分不需要 | 需先改 backpressure/primary sizing |
| C3 服务效果 | 在限定 workload 下，TTFT/SLO goodput 是否改善 | 是 | 只能保留微基准 claim |

推荐最终 claim 停在 C3 的**条件式 workload**，不花大量精力证明其生产普遍性。

## 2. 目标函数

### 2.1 主指标

默认建议：

1. **SLO goodput**：在同时满足预注册的 p99 TTFT 与 p99 ITL SLO 时，
   系统可持续完成的最大请求率；
2. **revisit TTFT**：p50/p95/p99，专门统计由 second tier 命中的请求。

如果最终目标明确是单用户交互体验，可以交换两者顺序，但不要只报告平均 TTFT。

SLO 数值应在正式运行前写入配置。跨硬件比较时，可把 unloaded baseline 的
p99 TTFT/ITL 乘一个固定系数作为 SLO，但这个系数也要预注册，不能看完结果再选。

### 2.2 守护指标

- 冷请求 TTFT：不能为了 revisit 用户严重伤害新用户；
- decode ITL p95/p99：background store/load 不应明显干扰 token generation；
- promotion acceptance rate；
- store absorption/completeness；
- I/O correctness 与 job failure；
- 磁盘空间、inode、CPU primary pin 水位。

### 2.3 机制指标

按 block 和 job 记录：

- secondary lookup：hit/miss/retry；
- promotion：attempted/accepted/rejected-primary-full；
- store：offered/filtered/submitted/completed/failed；
- load：submitted/completed/failed；
- job blocks/bytes；
- `t_lookup_start`、`t_promotion_submit`、`t_tier_observed_done`、
  `t_cpu_gpu_submit`、`t_cpu_gpu_done`、`t_first_token`；
- queue delay、service delay、completion→scheduler-observed delay；
- primary slot 数：free/evictable/pinned-by-store/pinned-by-cascade/
  pinned-by-promotion/pinned-by-cpu-gpu；
- tier queue depth/outstanding blocks；
- bytes read/written、CPU core-seconds/GiB、context switches、syscalls；
- 设备 `r/s, w/s, rMB/s, wMB/s, await, aqu-sz, util`。

vLLM 0.24.0 的内置指标主要覆盖 GPU↔CPU，不足以回答上述漏斗；P0 必须补齐。

## 3. 一个足够用的性能模型

令：

- `b`：每 token KV 字节数；
- `L`：可复用 prefix token 数；
- `B = b × L`：需要 promotion 的字节数；
- `T_recompute(L, c)`：并发 `c` 下重算这段 prefix 的时间；
- `T_lookup`：secondary metadata/index lookup；
- `T_disk(B, c, r)`：读写混合 `r` 下 secondary queue + service；
- `T_H2D(B)`：CPU→GPU；
- `T_poll`：scheduler step 与 completion polling 造成的额外等待。

对单请求 TTFT，second tier 值得使用的必要条件是：

```text
T_lookup + T_disk + T_H2D + T_poll < T_recompute
```

对吞吐，还要满足：

```text
secondary_read_Bps  < sustainable_read_Bps_under_mix
secondary_write_Bps < sustainable_write_Bps_under_mix
promotion staging demand < available CPU-primary slots
```

写流量近似来自所有 eligible 新 block：

```text
write_Bps ≈ eligible_prefill_token_rate × b × (1 - duplicate_fraction)
```

有用读流量来自深度命中：

```text
read_Bps ≈ request_rate
         × reused_tokens
         × P(GPU miss)
         × P(CPU miss)
         × P(secondary hit)
         × P(promotion accepted)
         × b
```

### 3.1 什么时候才可以说 FS 是 TTFT 瓶颈

至少同时满足：

1. **关键路径证据**：request 在 secondary lookup/promotion 期间确实不能执行，
   且该区间占 revisit TTFT 的可观比例；
2. **排队证据**：提高 storage offered load 后，tier job sojourn/queue depth
   增长，而不是 GPU prefill 单独变慢；
3. **反事实证据**：只替换 backend 后，TTFT 或 SLO goodput 改善；
4. **流量对账**：设备读写量、tier job bytes、最终 loaded tokens 能闭合。

“设备 util 100%”“FS 读了很多字节”或“纯 I/O 微基准更快”单独都不够。

如果 observed second-tier critical-path share 为 `S`，即使把 backend 变成无限快，
TTFT 最大改善也受 Amdahl 上限约束。`S < 10%` 时，不建议继续做昂贵 TTFT 实验；
可以保留 CPU/GiB 或微基准方向。

## 4. P0：观测与 contract（2–3 天，无 GPU）

### 4.1 先实现统一 InstrumentedTier

对 FS 与 uring-slab 使用同一个 decorator，在
`SecondaryTierManager` 接口高度记录：

- lookup 三态；
- submit 时刻、方向、job/block/byte 数；
- 第一次 `get_finished_jobs()` 观察到完成的时刻；
- job success；
- 每次 poll 的 pending job 数。

该 job sojourn 包含 tier 内排队、I/O 和 scheduler poll 粒度，命名必须是
`tier_job_observed_sojourn`，不能直接叫“设备 I/O latency”。

### 4.2 补 manager 级 promotion 漏斗

只包 tier 看不到 primary-full rejection，因为拒绝发生在
`TieringOffloadingManager._initiate_promotion()`。需要自定义 manager/spec
或窄范围 instrumentation，至少输出：

```text
secondary_hit
promotion_attempt
promotion_accept
promotion_reject_primary_full
promotion_complete_success/fail
cpu_to_gpu_complete
```

### 4.3 contract tests

两种 backend 都要跑同一套：

- store→drain→lookup→load→checksum；
- duplicate store；
- 同 key store-in-flight lookup；
- 并发 load pin 下 eviction；
- capacity full，有可逐出/无可逐出；
- partial I/O failure 的整 job 回滚；
- queue/ring full；
- reset/drain/shutdown；
- 空 job；
- short read/write；
- 进程重启后的索引/文件语义；
- `has_pending_work()` 保证 idle engine 仍会 poll 完成。

### 4.4 P0 gate

必须满足：

- 所有已接受 job 恰好得到一次 completion；
- 任意失败不留下 present 但数据不完整的 key；
- manager/tier/engine 三层 job 与 byte 计数误差为 0；
- checksum 全部通过；
- shutdown 后无 primary memoryview 在途访问。

## 5. P1：second tier 触发与 primary gate（1–2 天，最多 1 个 GPU 日）

这一步替代“大量研究真实场景何时使用 FS”。只验证源码推导能在运行时闭环。

### 5.1 四个最小场景

固定模型、固定 prefix 长度，依次做：

| 场景 | reuse distance | 预期 |
|---|---:|---|
| T0 无复用 | 无 revisit | 有 store，无 secondary load |
| T1 CPU 内复用 | `< CPU capacity` | CPU hit，无 FS load |
| T2 深度复用 | churn 足以分别逐出 GPU 与 CPU | FS hit、promotion accepted、device read |
| T3 staging 压力 | T2 + 小 CPU/并发 store | FS hit，但出现 primary-full rejection |

这里的 reuse distance 不用请求数表示，而用唯一 eligible KV 字节：

```text
RD_ratio = unique_KV_bytes_since_prime / cpu_primary_bytes
```

建议取 `RD_ratio ∈ {0.5, 1.25, 2.0}`，只用于验证边界。

### 5.2 无 GPU 的 lifecycle replay

在真实 `CPUPrimaryTierOffloadingManager + TieringOffloadingManager` 上，
用 fake mmap 和真实 FS/uring tier 回放：

```text
lookup → on_schedule_end → poll
prepare_store → complete_store → cascade
tick → lookup → promotion → tick → complete
```

扫：

- CPU block count；
- 每 tick store/load job；
- secondary latency 注入；
- scheduler poll 间隔；
- simultaneous promotion wave size。

它可以精确回答“primary 满时 load/store 谁成功”，无需先烧 GPU。

### 5.3 P1 gate

用于后续“qualifying workload”的配置必须满足：

- FS/uring 都有真实 secondary device reads；
- revisit loaded tokens 与期望 prefix 对账；
- promotion acceptance ≥ 99%；
- prime/churn 已完成持久化，revisit 前没有未 drain 的 store，除非该实验
  明确研究 mixed I/O；
- GPU 与 CPU local hit 已被排除。

T3 是单独的容量/调度实验，不与 backend 性能主结果混在一起。

## 6. P2：微基准与消融（5–7 天，无 GPU）

### 6.1 两层基准

#### A. 数据面

统一 memoryview、block size、job stream，比较：

1. `py-pool-files`：vLLM FS 的 O_DIRECT、file-per-block、tmp+rename；
2. `py-pool-slab`：同样 Python pool，改成预分配单文件 offset I/O；
3. `cpp-pool-slab`：同样 slab，改成 C++ blocking thread pool；
4. `uring-slab`：同样 slab 与 C++，改成 io_uring submit/reap。

成对归因：

| 对比 | 主要解释 |
|---|---|
| files vs py-pool-slab | 文件布局与元数据事务 |
| py-pool-slab vs cpp-pool-slab | Python/GIL/运行时与线程池实现 |
| cpp-pool-slab vs uring-slab | 提交模型与批量化 |

所有组统一：

- O_DIRECT 开关；
- 数据块与内存对齐；
- 文件 extent 预热状态；
- 读写数据量；
- outstanding job/block 数；
- 设备与 CPU 亲和性；
- 正确性校验。

#### B. 完整 tier contract

- baseline 直接实例化 vLLM 0.24.0 原生 `FileSystemTierManager`；
- candidate 实例化 `UringSecondaryTierManager`；
- 都走 `lookup/submit_store/submit_load/get_finished_jobs/drain_jobs`；
- 不重新实现一份“看起来像 FS”的 baseline。

原生 FS 没有容量上限。公平主对比中让 uring slab 容量大于唯一工作集，
并限制 FS 运行总数据量，避免 ENOSPC/inode 成为意外变量。bounded-capacity
与 eviction 另列 feature 实验。

### 6.2 负载维度

不要做全笛卡尔积。先测设备 sweet spot，再围绕拐点加密。

| 维度 | 建议值 |
|---|---|
| block bytes | 从目标模型推导的真实值；再加 0.25/1/4 MiB 锚点 |
| blocks/job | 1, 4, 16, 64, 256 |
| outstanding jobs | 1, 4, 16, 32, 64 |
| read share | 0%, 25%, 50%, 75%, 100% |
| access | sequential slots、uniform random、Zipf hot/cold |
| duplicate store | 0%, 50%, 90% |
| duration | ≥30 s steady state，另有短作业 latency 模式 |
| metadata state | warm 与 churn 后 cold-ish 分开 |

两种驱动模式：

- closed-loop QD：找设备/engine 最大能力与 sweet spot；
- open-loop arrival：观察过载点、queue growth 与 tail latency。

### 6.3 报告指标

- effective GiB/s 与 IOPS；
- job latency p50/p95/p99；
- queue/service 分解；
- CPU core-seconds/GiB；
- syscalls/block、context switches、线程数；
- device await/aqu-sz/util；
- file/inode count 与 metadata syscall；
- failed/rejected jobs；
- correctness。

### 6.4 P2 gate

继续端到端的最低标准：

- 在至少两个相邻、与目标 workload 对应的工作点上，
  `uring-slab` 相对完整 FS tier 满足以下之一：
  - p95 tier job sojourn 改善 ≥ 15%；或
  - CPU core-seconds/GiB 改善 ≥ 25%；
- 95% bootstrap CI 不跨 0；
- 所有 correctness 与 completion 对账通过；
- 优势不是只来自 FS baseline 未预热、容量不足或不同 O_DIRECT 语义。

如果优势只出现在极端单点，缩小 claim，不继续铺大规模端到端矩阵。

## 7. P3：scheduler-aware bottleneck gate（3–4 天，最多 1 个 GPU 日）

这一步回答原问题中的第 3 点，但严格限时。

### 7.1 要做什么

1. 用 lifecycle replay 扫 `CPU capacity × promotion wave × store pressure`；
2. 找到：
   - promotion acceptance 高的“backend 比较区”；
   - primary-full 明显的“上层瓶颈区”；
3. 只在前者做 FS vs uring 比较；
4. 在后者比较“扩大 CPU primary / 限制 store / 改 backend”三种动作，
   证明瓶颈到底在上层还是数据面。

### 7.2 低成本 TTFT share 估算

在一个 GPU smoke workload 上采：

```text
request_arrival
secondary_lookup_start
promotion_submit
tier_observed_done
cpu_gpu_done
first_token
```

计算：

```text
secondary_wait_share
= (cpu_gpu_done - secondary_lookup_start) / TTFT
```

并同时报告：

- metadata lookup share；
- tier job observed sojourn share；
- CPU→GPU share；
- scheduler polling gap share。

### 7.3 是否需要证明真实世界普遍性

如果论文/项目 claim 是：

> “我们优化了 vLLM 在 qualifying deep-reuse workload 下的 secondary tier”

则到这里为止只需证明该 workload 可构造、路径真实执行、I/O 在关键路径。
不需要真实 trace，也不需要估计生产占比。

只有 claim 改成：

> “大多数线上 vLLM 服务都会从该 engine 获益”

才必须引入生产 trace/代表性工作负载。当前不建议扩大到这条 claim。

### 7.4 P3 gate

- qualifying workload 中 promotion acceptance ≥99%；
- `secondary_wait_share ≥15%`，或 FS queue saturation 对 SLO miss 有明确贡献；
- backend 替换前后的 loaded tokens、GPU compute、request mix 一致；
- 如果 primary-full 是主因，先停止 backend 端到端实验，处理调度/backpressure。

## 8. P4：合成 trace 端到端实验（5–7 个 GPU 日）

### 8.1 baseline

至少四组：

1. `no-offload/APC recompute`；
2. `CPU-only`；
3. `CPU + vLLM FS`；
4. `CPU + uring-slab`。

机制消融可加 `CPU + cpp-pool-slab`，但不作为每个 workload 的必跑组。

所有组固定：

- 模型、dtype、TP/PP；
- GPU memory utilization；
- GPU KV block size；
- offloaded block size；
- CPU primary bytes；
- prompt/output 长度；
- request arrival timestamps；
- `PYTHONHASHSEED`；
- serve 与 shutdown 方式。

### 8.2 trace 生成模型

一个 request 由：

```text
shared_prefix_id + unique_suffix + output_length
```

组成。控制参数：

- prefix tokens `L`；
- prefix pool size `K`；
- reuse distance `RD_bytes / CPU_bytes`；
- revisit probability；
- hot-prefix Zipf 参数；
- open-loop arrival rate；
- revisit burst concurrency；
- background unique churn rate；
- output tokens。

不要把“request 读写比”当成 storage 读写比。正式横轴使用运行时观测到的：

```text
observed_read_share
= completed_secondary_read_bytes
 / (completed_secondary_read_bytes + completed_secondary_write_bytes)
```

压力使用归一化值：

```text
read_load  = offered_read_Bps  / standalone_sustainable_read_Bps
write_load = offered_write_Bps / standalone_sustainable_write_Bps
wave_size  = revisit_wave_bytes / cpu_primary_bytes
```

### 8.3 三个 campaign

#### E2E-A：纯 revisit / read critical path

```text
prime prefixes
→ 等所有 secondary stores drain
→ unique churn 挤出 GPU 与 CPU
→ 等 background stores drain
→ revisit burst
```

扫 `concurrency ∈ {1, 4, 16, 32}`。这组最干净地测 revisit TTFT 与 read
backend，不混 background write。

#### E2E-B：mixed interference

background unique churn 持续运行，同时周期性发 revisit wave。选两个主工作点：

- read-heavy：observed read share 约 75%；
- balanced：observed read share 约 50%。

若 write-only 微基准显示明显问题，再补 write-heavy 25% read；不要一开始
就跑完整 5×4 矩阵。

测：

- revisit TTFT；
- churn/cold TTFT；
- decode ITL；
- promotion rejection；
- store absorption；
- SLO goodput。

#### E2E-C：primary pressure

固定 backend，扫：

```text
revisit_wave_bytes / CPU_primary_bytes ∈ {0.25, 0.5, 1.0, 2.0}
```

只用于说明上层 staging/pin 何时成为瓶颈，不与 backend 主结果混合。

### 8.4 每轮有效性条件

一轮只有同时满足以下条件才进入统计：

- prime block 已持久化；
- revisit 前目标 prefix 在 GPU 与 CPU 均 miss；
- secondary existence hit；
- promotion accepted；
- device read bytes 与理论 KV bytes 在容差内；
- loaded tokens 与理论 prefix tokens 在容差内；
- 没有 ENOSPC/inode exhaustion/OOM；
- 没有未记录的 job failure；
- request 到达序列与各组一致。

### 8.5 重复与统计

- 每个工作点至少 3 次独立 run；
- 同一 run 内做多轮交错，降低跨 serve 漂移；
- 报告样本数、median、p95、p99、bootstrap 95% CI；
- 运行顺序做 A/B/B/A 或随机化；
- 不只报告最优 queue depth；
- warmup 与正式窗口分开；
- raw sample 不丢弃，异常剔除必须有预注册规则。

### 8.6 P4 成功标准

预注册以下任一作为系统收益：

- SLO goodput 提高 ≥10%；或
- revisit p95 TTFT 降低 ≥10%；

同时要求：

- 95% CI 不跨 0；
- p99 ITL 退化不超过 5%；
- cold/churn TTFT 无不可接受退化；
- promotion acceptance ≥99%；
- store completeness ≥99%；
- 数据正确性与 loaded-token 对账通过。

## 9. 推荐时间线

从干净起点、单人推进：

| 时间 | 工作 | 产物 | GPU |
|---|---|---|---:|
| D1–D2 | 锁定源码、调用链、配置与失败语义 | 架构文档、source lock | 0 |
| D3–D5 | 统一观测、promotion 漏斗、contract tests | P0 可重复测试 | 0 |
| W2 | 数据面 + 完整 tier 微基准、消融 | P2 原始结果与 gate | 0 |
| W3 前半 | lifecycle replay、primary capacity map | 调度可达性报告 | 0 |
| W3 后半 | T0–T3 GPU smoke、TTFT share | P1/P3 gate | ≤2 天 |
| W4 | E2E-A、E2E-B 主工作点、goodput | 系统结果 | 5–7 天 |
| W5 可选 | 复测、第二模型/第二设备、恢复与运维 | 外推与鲁棒性 | 按需 |

控制原则：

- P3 的“second tier 是否常见”最多 2 个 GPU 日；
- P2 gate 不过，不进入 W4；
- primary-full 主导时，停止 backend 对打，先解决 staging/backpressure；
- 第一轮端到端只用一个模型、一个 block size、TP=1；
- 微基准已经覆盖 block size 泛化，除非 claim 需要，不急着加第二模型。

## 10. 对原始六个问题的直接回答

1. **FS 什么时候用？** 每个 eligible 完整 block 会尝试 store；只有 GPU/CPU
   都 miss、FS hit、primary slot 可分配时，FS load 才真正服务 request。
2. **second tier 怎么接？** 实现 `SecondaryTierManager`，只读写 CPU primary
   memoryview；建议用自定义 spec + factory registration，避免 fork。
3. **是否值得做新 engine？** 对“条件式 deep-reuse claim”，源码已给出合理动机；
   只需 P1/P3 小 gate，不需要大规模证明生产普遍性。是否值得继续端到端由
   P2 的稳定优势和 P3 的 critical-path share 决定。
4. **要不要完整实现 FS tier？** 不要重写；完整基准直接实例化 vLLM 0.24.0
   原生 FS tier。消融中的 replica 只用于归因，并与正式 baseline 分开命名。
5. **并发、读写比、revisit 怎么设计？** revisit 决定逻辑 deep-hit，
   并发决定 promotion wave/排队，background unique KV 决定 write pressure。
   用 observed secondary bytes 定义读写比，不用请求数猜。
6. **代码怎么管？** 见 [CODE_MANAGEMENT.md](CODE_MANAGEMENT.md)。
