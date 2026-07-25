# Second-tier experiment contract v1

状态：**FROZEN FOR NATIVE-FS NOISE QUALIFICATION**  
上游：vLLM `v0.24.0`  
commit：`ee0da84ab9e04ac7610e28580af62c365e898389`

这份 contract 冻结的是正式实验路径中 baseline 与 candidate 必须共享的
行为和观测边界，不冻结 candidate 的内部数据结构，也不承诺实现 vLLM
接口之外的通用存储能力。

## 1. 上游接口

被测对象位于 `SecondaryTierManager` 边界，必须提供：

```text
lookup(key, req_context) -> True | False | None
submit_store(job_metadata) -> None
submit_load(job_metadata) -> None
get_finished_jobs() -> Iterable[JobResult]
on_new_request(req_context) -> RequestOffloadingContext
on_request_finished(req_context) -> None
on_schedule_end() -> None
drain_jobs() -> None
shutdown() -> None
```

其中：

- `True` 表示 key 已存在且可读；
- `False` 表示不存在；
- `None` 表示结果或传输仍在途，调用者稍后重试；
- `submit_*` 必须轻量、非阻塞，数据复制不在 scheduler 调用线程执行；
- store 是 `CPU primary → secondary`；
- load/promotion 是 `secondary → CPU primary`；
- 完成只通过 `get_finished_jobs()` 被上层承认。

## 2. Job 前置条件

正式实验只生成满足以下条件的 job：

- `job_id` 在当前 tier 生命周期内唯一；
- `len(keys) == len(block_ids) >= 1`；
- 每个 `block_id` 指向 primary memoryview 中一个完整、有效的 slot；
- store 期间源 slot 已被上层 pin；
- load 期间目标 slot 已被上层预留；
- store 的 `is_promotion=false`；
- load 的 `is_promotion=true`；
- 一个 job 的所有 block 大小相同；
- block 大小和 primary 起始地址满足原生 FS `O_DIRECT` 对齐要求。

空 job、长度不匹配、无效 block id 属于调用方违约。它们不在本项目正式路径
内，也不作为 candidate 的必需通用能力。

## 3. Completion 与正确性

- 每个已接受 job 必须恰好产生一个 `JobResult`；
- `JobResult.job_id` 必须等于提交时的 id；
- 一个 block 失败时，整个 job 的 `success=false`；
- completion 不允许重复，也不允许引用未知 job；
- `drain_jobs()` 返回后，不能再有 I/O 访问 primary memoryview；
- 正式路径只在 `drain_jobs()` 完成后调用 `shutdown()`；
- store→drain→lookup→load→drain 后，全区 checksum 必须一致；
- successful store 的唯一 `.bin` 文件数和数据字节必须与提交量对账；
- duplicate store 可以避免重写，但对应 job 仍必须正常完成。

vLLM 0.24.0 原生 FS 的 `shutdown()` 会直接清除尚未开始的队列项，而不会为
这些取消项生成失败 completion。因此“带 pending job 直接 shutdown”不是
本项目的 A/B contract；强制要求 candidate 支持它反而会超出 baseline 语义。

## 4. 统一计时边界

所有 backend 使用同一个外部 wrapper，记录相同的单调时钟：

| 指标 | 定义 |
|---|---|
| `submit_call_ns` | `submit_*` 入口到返回，衡量 scheduler 同步开销 |
| `tier_job_observed_sojourn_ns` | `submit_*` 入口到第一次由 `get_finished_jobs()` 观察到结果 |
| `phase_wall_ns` | 一批 job 第一次提交前到最后一个 completion 被观察到 |
| `lookup_resolution_ns` | 首次 lookup 到同一批 key 全部离开 `None` 状态 |
| `poll_interval_ns` | runner 固定的 completion/lookup 轮询间隔 |

`tier_job_observed_sojourn_ns` 包含：

```text
tier 内部排队 + 文件系统/设备服务 + 固定 poll 量化
```

它不能命名为“设备 I/O latency”。当前 noise runner 不包含 vLLM scheduler
step、CPU→GPU 或 TTFT。

## 5. 事件与对账

事件采用 append-only JSONL，至少包含：

```text
schema_version
run_id
sequence
t_wall_ns
t_mono_ns
event
tier
direction
job_id
n_blocks
n_bytes
success
pending_jobs
```

每个 run 必须满足：

```text
submitted_jobs
= completed_success + completed_fail

submitted_bytes
= successful_job_bytes + failed_job_bytes
```

并额外校验：

- completion id 集合与 submission id 集合完全相等；
- store/load 各自 job、block、byte 数闭合；
- store 文件数和 `.bin` 字节数闭合；
- load 后 SHA-256 与 store 前一致；
- 所有 lookup 最终为 `True`；
- run 退出前 pending job 数为 0。

## 6. Native-FS 噪声 campaign

配置在 `configs/native_fs_noise_v1.json` 冻结，关键点是：

- baseline 直接实例化上游 `FileSystemTierManager`；
- 使用真实服务已经观察到的 196,608-byte KV block；
- 16 read-priority + 16 write-priority threads；
- 每个 run 使用新的隔离目录，避免 duplicate-store 静默变成 no-op；
- 数据通过 `O_DIRECT` 路径，禁止 drop cache；
- 每个 run 结束后先校验，再删除其隔离数据目录；
- 三个时间窗口分别重复，窗口之间保留间隔；
- gate 只判断环境噪声和 contract correctness，不产生 backend 优劣结论。

## 7. 变更规则

contract、计时点、阈值或 workload 参数一旦看到 native-FS campaign 数据后，
不得原地修改。确有缺陷时：

1. 保留失败 campaign；
2. 新建 contract/config 版本；
3. 写明修改原因；
4. 从 native FS 开始全部重跑。

candidate 不得反向改变 v1 的 baseline 语义或统计口径。
