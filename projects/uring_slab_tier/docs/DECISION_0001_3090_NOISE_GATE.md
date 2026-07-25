# Decision 0001：RTX 3090 云机噪声 gate

日期：2026-07-26（Asia/Shanghai）  
状态：**ACCEPTED — CONDITIONAL PASS**  
适用机器：当前 KVM 客户机呈现的 NVIDIA GeForce RTX 3090  
不公开 SSH 地址

## 1. 决定

这台机器可以继续用于 candidate 实现和正式微基准，但不是无条件的
performance PASS：

- vLLM/CUDA/second-tier 功能资格：**PASS**；
- 数据完整性与 native-FS contract：**PASS**；
- 预分配 O_DIRECT 写路径稳定性：**PASS**；
- 固定访问序列的 O_DIRECT 随机读稳定性：**WARN**；
- native FS 作为整体 baseline 的严格重复噪声 gate：**FAIL**。

因此项目不换机器，也不把 FAIL 改写成 PASS。后续采用预注册的成对随机化
A/B、足够重复和 practical-effect floor。读侧小于噪声地板的差异不得写成
正向结论。

## 2. 锁定环境

- KVM guest，Ubuntu 24.04，kernel `6.8.0-31-generic`；
- 16 个在线 vCPU，affinity `0-15`；
- 约 47 GiB guest memory；
- NVIDIA GeForce RTX 3090，24,576 MiB，driver 550.78；
- 实验路径：XFS `/dev/vda1` → virtio `/dev/vda`；
- vLLM source commit：
  `ee0da84ab9e04ac7610e28580af62c365e898389`；
- vLLM distribution：`0.24.0+cu129`；
- PyTorch：`2.11.0+cu129`，CUDA runtime 12.9；
- GPU checksum、tiering imports：PASS；
- 物理介质、宿主 cache、共享程度和云 QoS：UNKNOWN。

## 3. Native-FS 正式 campaign

campaign：`20260725T155651Z-36af5ae1`  
harness tree：
`4d4eb66bc2da73a4e75a89579d53b00cea401cf0f8f6b668bf364e07eb0f7c9c`

配置：

- 30 runs，3 个窗口，每窗口 10 次；
- 窗口间隔 300 秒；
- 每 run 1,024 × 196,608 bytes = 192 MiB/方向；
- 每 job 16 blocks；
- 16 read-priority + 16 write-priority threads；
- O_DIRECT，固定 0.5 ms completion poll；
- 每 run 新隔离目录，校验后删除 backing files。

正确性：

- 30/30 run PASS；
- 0 runner failure；
- 每 run store/load completion、192 MiB job bytes、1,024 个 `.bin`、
  全区 SHA-256 与 shutdown 全部闭合；
- 所有 backing files 已删除；
- 完整源码快照与 manifest 中 11 个文件 hash：PASS。

噪声结果：

| 指标 | median | robust CV | 95% CI 相对半宽 | 窗口 max/min | Gate |
|---|---:|---:|---:|---:|---:|
| store throughput | 926.08 MiB/s | 11.01% | 5.18% | 1.665 | FAIL |
| load throughput | 1,118.49 MiB/s | 32.15% | 22.66% | 1.812 | FAIL |
| store job p95 | 197.45 ms | 10.34% | 7.94% | 1.660 | WARN |
| load job p95 | 143.88 ms | 13.02% | 8.66% | 1.472 | PASS |

guest `/proc/diskstats` 支持 I/O 路径波动，而不是纯计时器噪声：

- store throughput 与 guest write-time Pearson `r=-0.849`；
- load throughput 与 guest read-time Pearson `r=-0.662`；
- process user CPU robust CV 2.85%；
- process system CPU robust CV 4.48%；
- 每 run process write bytes 均为 192 MiB；
- process read bytes 为 192 MiB 加最多几十 KiB 固定量级开销。

## 4. 设备控制组

### 4.1 v1：INVALID

campaign：`20260725T164752Z-efc632e8`

v1 在 20 个 run 后停止。原因是每个 repetition 使用不同 random seed，
访问序列不相同，不能把差异全部解释为时间噪声。

- 所有已产生 run 和精确源码快照保留；
- 15/15 manifest 文件 hash PASS；
- 结果不进入机器判定；
- 残留的单个 192 MiB backing file 已删除。

### 4.2 v2：固定访问序列

campaign：`20260725T165904Z-45c3988f`  
harness tree：
`cfa7488d4ded82622828f4216855c604c7c35a42b8267dc8873b36446ac190a5`

v2 使用：

- 一个预分配 192 MiB 文件；
- O_DIRECT + io_uring；
- 196,608-byte block；
- QD=32；
- 30 runs、3 个窗口；
- 每次完全相同的 randwrite/randread offset 序列；
- 同一组预注册阈值。

30/30 run I/O error=0、读写字节对账 PASS；backing file 已自动删除；
16/16 manifest 文件 hash PASS。

| 指标 | median | robust CV | 95% CI 相对半宽 | 窗口 max/min | Gate |
|---|---:|---:|---:|---:|---:|
| preallocated write throughput | 2,232.56 MiB/s | 1.74% | 0.59% | 1.006 | PASS |
| fixed randread throughput | 773.54 MiB/s | 19.01% | 10.68% | 1.225 | WARN |
| write clat p95 | 5.54 ms | 3.51% | 1.48% | 1.012 | PASS |
| read clat p95 | 10.81 ms | 25.16% | 16.36% | 1.094 | WARN |

这个控制组说明：

1. native FS 的 store 不稳定不能全部归因于云盘；file-per-block、目录/extent
   等 baseline 机制很可能贡献了额外波动；
2. 固定访问序列、预分配单文件仍有读侧 run-to-run 波动，因此 load 侧确有
   客户机不可消除的 storage-path noise；
3. 三个 read 窗口的中心位置比 native FS 稳定，机器并非完全不可用于
   performance study，但不适合用少量 run 判断小效果。

## 5. 正式 A/B 的追加成功条件

以下条件在任何 candidate 正式结果产生前冻结：

1. FS 与 candidate 必须按时间 block 成对运行，pair 内顺序随机化；
2. 至少 30 pairs，并分布在 3 个时间窗口；
3. 同一 pair 的 workload、字节、QD、CPU affinity 和 poll 配置完全相同；
4. 95% paired-bootstrap CI 必须排除 0；
5. 三个窗口的 paired median 改善方向必须一致；
6. practical-effect floor：

| 指标 | A/A 95% paired-median noise floor | 正式最小效果 |
|---|---:|---:|
| store throughput | 8.1% | 10% |
| load throughput | 20.1% | 25% |
| store job p95 | 7.0% | 10% |
| load job p95 | 8.3% | 10% |

A/A noise floor 来自 native-FS 30-run campaign 内相邻 run 配对，对每个 pair
随机翻转方向后枚举 sign assignment，取 `|paired median|` 的 95th
percentile。它不是 candidate 结果驱动的阈值。

如果 candidate 没有同时越过 CI 与 practical floor：

- 不增加有利 trace；
- 不删除不利窗口；
- 不把数值差异写成成功；
- 转为“更快 backend 为何没有稳定转化”或报告不可区分边界。

## 6. 对后续范围的影响

- 可以进入 independent engine + vLLM 0.24 adapter 实现；
- 微基准完整消融仍按原计划执行；
- load/TTFT 结果必须保留 cloud read-noise limitation；
- 端到端 p99 若受相同读长尾控制，只能报告条件式或负结果；
- 不进行 scheduler 协同优化；
- 不用此次结果证明线上 workload 出现频率。
