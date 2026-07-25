# Second-tier experiment contract v2

状态：**FROZEN BEFORE CANDIDATE PERFORMANCE DATA**  
上游：vLLM `v0.24.0`  
commit：`ee0da84ab9e04ac7610e28580af62c365e898389`

v1 及其原始 campaign 保持不变。v2 补齐 candidate 在正式实验路径上真正需要
的接口、状态和统计语义；仍不承诺生产系统的全部通用能力。

## 1. 正式代码边界

```text
vLLM 0.24 scheduler / tiering manager（不修改）
        │
        ▼
Python UringSecondaryTierManager adapter
        │  job id / key / primary slot / direction
        ▼
C++ uring-slab data engine
```

- Python 负责 lookup、容量、LRU、pin、in-flight、失败回滚和 vLLM 对接；
- C++ 只负责已验证 offset 的异步数据传输与 completion；
- 正式 baseline 是原生 `FileSystemTierManager`；
- candidate 不修改 scheduler、promotion 或 CPU primary 语义。

## 2. 实验路径接口

adapter 必须提供：

```text
lookup(key, req_context) -> True | False | None
submit_store(job_metadata) -> None
submit_load(job_metadata) -> None
get_finished_jobs() -> Iterable[JobResult]
on_new_request(req_context) -> RequestOffloadingContext
on_request_finished(req_context) -> None
on_schedule_end() -> None
touch(keys, req_context) -> None
has_pending_work() -> bool
drain_jobs() -> None
shutdown() -> None
```

其中：

- `submit_*` 和 `get_finished_jobs()` 对 scheduler 线程保持轻量、非阻塞；
- engine 或本地 completion queue 有工作时，`has_pending_work()` 必须为真；
- `touch()` 更新已存在 key 的 LRU；
- completion 只有被 `get_finished_jobs()` 返回后才被上层承认；
- `drain_jobs()` 返回后，不得再访问 primary memoryview。

## 3. 状态与失败语义

正式 candidate correctness 必须覆盖：

- duplicate key 和 in-flight store/load；
- 有限 slab capacity；
- LRU eviction；
- store source pin 与 load target reservation；
- load 期间 secondary slot pin；
- queue/ring full；
- short I/O；
- 单 block I/O failure；
- job 整体回滚；
- accepted job exactly-once completion；
- drain 和 clean shutdown；
- O_DIRECT 对齐失败显式报错，禁止 buffered fallback。

实验路径之外不要求：

- crash recovery；
- 多进程共享写；
- 在线扩容；
- 长期碎片整理；
- 生产监控和运维接口。

## 4. 公平比较

两侧必须相同的是 adapter-visible offered load：

- key、block bytes、blocks/job；
- job submit 顺序；
- outstanding jobs；
- poll interval；
- measured logical bytes；
- CPU affinity；
- warmup 和数据生命周期。

不强制相同的是 backend 内部机制：

- FS read/write thread 数；
- candidate ring queue depth；
- blocking threads 或 io-wq 数；
- file-per-block 与 slab layout；
- batching 和提交模型。

这些差异本身就是 treatment，不能通过强制“相同内部 QD”消除。

## 5. 分阶段证据

1. FS/FS paired A/A 先资格化环境与 runner；
2. candidate correctness 全部通过；
3. native FS 与 candidate 使用同一个 paired runner；
4. 完整消融只在微基准执行；
5. 端到端只比较两个最终系统；
6. scheduler 协同优化不属于本项目。

环境资格协议与失败边界见
`DECISION_0002_UNSTABLE_CLOUD_READ_PATH.md`。

