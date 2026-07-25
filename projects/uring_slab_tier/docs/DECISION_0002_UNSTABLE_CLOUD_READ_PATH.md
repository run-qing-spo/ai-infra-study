# Decision 0002：无裸机条件下的读侧证据策略

日期：2026-07-26（Asia/Shanghai）  
状态：**ACCEPTED — IMPLEMENTING**

## 1. 背景

当前可用环境都有明确限制：

- RTX 3090 KVM 客户机的 native-FS load throughput robust CV 为
  `32.15%`；
- 原 192 MiB load phase 中位时长只有约 `0.17 s`，少量 storage-path
  straggler 就会决定整个 phase wall time；
- 原 15 个相邻 A/A pair 的两侧吞吐 Pearson 相关系数约为 `0.02`，
  不能假设简单 run-level pairing 会消除漂移；
- 可用 AutoDL 环境的内核/文件系统拒绝 NOWAIT，io_uring 请求全部
  `io-punt`，不能作为 native asynchronous io_uring 正向证据；
- 当前拿不到裸机或可验证物理 NVMe。

因此不再把“换到理想机器”作为项目依赖，也不通过增加重复次数、删除窗口或
提高 candidate 成功阈值掩盖环境问题。

## 2. 决定

RTX 3090 继续承担：

- candidate 开发与 correctness；
- vLLM 0.24 adapter 和 GPU 功能验证；
- 写侧实验；
- 通过新资格 gate 后的读侧条件式结论；
- gate 失败时的负结果与瓶颈边界。

读侧正式资格改为固定工作量的长窗口 paired FS/FS A/A：

- 3 个窗口，每窗口 10 pairs，共固定 30 pairs；
- 每个窗口的 A-first/B-first 严格 5/5 平衡；
- A、B 都是同一个 vLLM 0.24.0 原生 `FileSystemTierManager`；
- 每个 arm 使用独立 root，但 key、数据、job 划分完全相同；
- 4 个 warmup passes；
- 64 个 measured passes；
- 每 pass 1,024 × 196,608 bytes，即每 arm 固定 measured 12 GiB；
- measured phase 必须不少于 8 秒；
- 不 drop cache，不修改 sysctl、cgroup 或 mount；
- 每个 pair 完成后删除两侧 backing files。

这个 campaign 只资格化 warm steady-state load。它不替代：

- fresh-key store 微基准；
- cold-dentry/cold-host-cache 实验；
- candidate correctness；
- 真实 vLLM revisit TTFT。

## 3. 统计口径

主指标：

```text
load_sustained_throughput_mib_s
```

固定 effect：

```text
effect = log(sham_B / reference_A)
improvement = exp(median(effect)) - 1
```

推断：

- 30 个 pair effect 的 median；
- 按三个时间窗口分层的 paired bootstrap 95% CI；
- 固定 seed 的 100,000 次 sign-flip paired-median noise floor；
- 使用 A/A 实测噪声和 25% 目标效果估计固定 30-pair 检出能力；
- 额外报告各 arm unpaired robust CV、单 pair effect、窗口 effect、
  first/second position effect 和 pair correlation。

硬失败：

- 少于 30/30 完整 pair；
- 任一 completion/checksum/file/event/shutdown 错误；
- measured bytes 不等于 12 GiB；
- measured phase 少于 8 秒；
- pair 两个 measured phase 的间隔大于 5 秒；
- 运行中替换失败 pair 或看到性能结果后改标 INVALID。

统计判定：

| 条件 | PASS | CONDITIONAL | FAIL |
|---|---:|---:|---:|
| `abs(A/A median)` | ≤5% | 5–10% | >10% |
| paired-median noise floor | ≤10% | 10–15% | >15% |
| 最大窗口 median effect | ≤10% | 10–20% | >20% |
| first/second position effect | ≤5% | 5–10% | >10% |

此外：

- bootstrap CI 必须包含 0；
- 对 25% 效果的 empirical detection power 必须至少 80%。

`CONDITIONAL` 只允许后续对至少 25% 的效果作主张。`FAIL` 表示这台机器
不得承担 load-throughput 或 TTFT 正向主结论。

## 4. AutoDL 的角色

AutoDL 的全量 io-punt 不是需要“修掉”的实验噪声，而是 portability
失败边界：

- 可以运行 slab、C++ blocking pool 和 correctness；
- 可以证明旧内核/文件系统上 io_uring 会退化到 io-wq；
- 不得把该环境上的 io_uring 数字写成 native async I/O 机制收益；
- 不因 AutoDL 结果修改 RTX 3090 的成功阈值。

## 5. 失败后的项目叙事

若长窗口 A/A 仍 FAIL：

1. 不继续调整 trace 或增加有利重复；
2. 保留 candidate 的 correctness、contract、消融和 io-punt 边界；
3. 将读侧系统结论写成：

> 在不可控虚拟存储尾延迟下，更低的软件路径开销为什么不足以形成可重复的
> serving 收益，以及需要怎样的硬件/内核条件才能验证该收益。

这仍满足项目的作品集目标，但不声称当前云盘上存在稳定正向性能差异。

