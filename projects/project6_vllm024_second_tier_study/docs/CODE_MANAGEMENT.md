# 代码、配置与实验结果管理

## 1. 目标

需要同时隔离四类变化：

1. vLLM 0.24.0 上游事实；
2. second-tier contract 与 instrumentation；
3. backend/engine 实现；
4. workload、运行编排和结果分析。

如果这四类混在一个脚本或一个 manager 中，微基准与端到端结果很难归因，
也很难知道一次结果到底对应哪个实现。

## 2. 推荐目录

当前目录先只放研究设计。进入实现后，目标结构为：

```text
project6_vllm024_second_tier_study/
├── README.md
├── SOURCE_LOCK.json
├── pyproject.toml
├── docs/
│   ├── VLLM_024_ARCHITECTURE.md
│   ├── EXPERIMENT_ROADMAP.md
│   ├── CODE_MANAGEMENT.md
│   ├── decisions/
│   └── runbooks/
├── src/vllm_second_tier_lab/
│   ├── plugin.py
│   ├── spec.py
│   ├── contract.py
│   ├── instrumentation/
│   │   ├── tier.py
│   │   ├── tiering_manager.py
│   │   └── events.py
│   ├── tiers/
│   │   ├── fs_adapter.py
│   │   └── uring_slab.py
│   ├── policies/
│   │   ├── slot_index.py
│   │   └── lru.py
│   └── replay/
│       ├── lifecycle.py
│       └── trace.py
├── csrc/
│   ├── engine_api.hpp
│   ├── uring_slab_engine.cpp
│   ├── cpp_pool_slab_engine.cpp
│   └── bindings.cpp
├── tests/
│   ├── contract/
│   ├── correctness/
│   ├── failure/
│   └── replay/
├── benchmarks/
│   ├── micro/
│   ├── contract/
│   ├── e2e/
│   └── analysis/
├── configs/
│   ├── hardware/
│   ├── models/
│   ├── workloads/
│   └── campaigns/
├── scripts/
│   ├── run.py
│   ├── validate_run.py
│   └── summarize.py
└── results/
    ├── README.md
    ├── manifests/
    └── summaries/
```

## 3. 组件边界

### 3.1 vLLM pin

- 不把整份 vLLM 源码复制进本项目；
- `SOURCE_LOCK.json` 固定 repo/tag/commit；
- 运行 manifest 再记录实际 import 到的 `vllm.__version__`、源码 commit/path；
- 如需上游 patch，只放小 patch 文件和用途说明；
- 正式 baseline 必须是未改数据路径的 0.24.0 FS tier。

### 3.2 FS baseline

分两种，命名不能混：

- `vllm-fs-contract`：直接实例化上游 `FileSystemTierManager`，用于正式对比；
- `py-pool-files-ablation`：只复刻其 I/O 形状，用于拆变量。

任何图表都不得把 ablation replica 简写成“vLLM FS”。

### 3.3 policy 与 engine

`UringSecondaryTierManager` 只做：

- key→slab slot；
- LRU/容量；
- duplicate 与 in-flight；
- pin/unpin；
- vLLM job 到 engine job 的转换；
- completion 后提交/回滚索引。

`UringSlabEngine` 只做：

- primary memory offset ↔ slab file offset；
- async submit/reap；
- short I/O/errno；
- drain/shutdown；
- 数据面 stats。

这样可以用相同 policy 替换 `cpp-pool-slab` 与 `uring-slab`，把提交模型作为
单一变量。

### 3.4 instrumentation

instrumentation 不嵌进 candidate 专有逻辑。统一 decorator 同时包 FS 与 uring，
并通过 custom `TieringOffloadingManager` 记录 promotion 漏斗。

所有事件写 append-only JSONL/Parquet，不周期性覆盖全量文件。

## 4. 与现有 project5 的关系

`project5_vllm_disk_tier` 已包含可复用的 engine、manager、微基准和 trace
基础设施，但本研究目录不继承其历史结论。

建议：

1. 先把 project5 当作 prototype dependency；
2. 用本目录的新 contract tests 和 P0/P2 gates 重新验证；
3. 不复制同一份 C++/Python engine 到两个目录；
4. gate 通过后，再单独做一次明确的 refactor：
   - 要么把稳定 backend 抽到 `libs/kv_uring_tier/`；
   - 要么把 project5 标记为 archive，并把唯一实现迁入本目录；
5. 未完成迁移前，run manifest 必须记录实际加载的 package path 与 git SHA。

移动/删除现有目录应作为单独变更，不和性能实验代码混在一个提交里。

## 5. 配置管理

### 5.1 不使用“长 shell 命令就是配置”

每次运行由四份显式配置合成：

```text
hardware.yaml
model.yaml
workload.yaml
campaign.yaml
```

生成最终 `resolved_config.json`，包括默认值。runner 只接受配置文件和少量
运行控制参数，不让命令行悄悄覆盖核心实验变量。

### 5.2 必须锁定的字段

- vLLM tag/commit/source path；
- candidate package git SHA 与 dirty diff hash；
- model revision/tokenizer revision；
- dtype、TP/PP、block sizes；
- GPU/CPU KV bytes；
- secondary capacity/path；
- FS thread counts；
- uring QD/batch/prewarm/O_DIRECT；
- `PYTHONHASHSEED`；
- kernel、filesystem、mount options、device topology；
- CPU/GPU 型号、NUMA、内存/cgroup limit；
- workload seed 与完整 arrival timestamps；
- SLO 与 gate thresholds。

## 6. 每次 run 的不可变产物

```text
run_id/
├── manifest.json
├── resolved_config.json
├── environment.json
├── command.json
├── server.log
├── client_samples.jsonl
├── tier_events.jsonl
├── engine_jobs.jsonl
├── system_metrics.jsonl
├── validation.json
└── DONE
```

规则：

- `run_id` 一旦创建不覆盖；
- 只有所有子进程正常结束、校验通过才写 `DONE`；
- 失败 run 保留并写 `failure.json`；
- 分析脚本只默认读取有 `DONE` 且 `validation.pass=true` 的 run；
- raw results 不进 Git；Git 只提交 manifest 索引、聚合表、图和分析代码；
- summary 必须列出被排除 run 及原因。

## 7. schema 与对账

事件至少有：

```text
schema_version
run_id
engine_step
request_id
job_id
t_wall
t_mono_ns
event
direction
n_blocks
n_bytes
success
reason
tier
```

一次 run 的 `validation.json` 必须对账：

```text
store_offered
= store_filtered
 + store_submitted
 + store_rejected

store_submitted
= store_completed_success
 + store_completed_fail
 + store_still_pending

secondary_hit
= promotion_accepted
 + promotion_rejected_primary_full
 + duplicate/inflight handling

promotion_accepted
= promotion_completed_success
 + promotion_completed_fail
 + promotion_still_pending
```

并校验：

- engine bytes 与 tier completed bytes；
- device bytes 与 engine bytes（允许文件系统/设备层固定容差）；
- loaded blocks/tokens 与 request external-hit tokens；
- FS 文件字节与成功唯一 store blocks；
- uring index present slots 与 backing capacity。

## 8. 测试分层

| 层 | 内容 | 运行频率 |
|---|---|---|
| unit | slot/index/LRU/job state | 每次提交 |
| contract | 两 tier 共用 lifecycle suite | 每次提交 |
| failure | ENOSPC、short I/O、queue full、shutdown | 每次提交或 nightly |
| micro smoke | 小数据正确性 + 一组性能 sanity | 每次提交 |
| full micro | 完整矩阵 | 明确触发 |
| lifecycle replay | primary/promotion map | 每次影响 manager 时 |
| GPU smoke | T0–T3 | release candidate |
| E2E campaign | 正式 workload | 只跑锁定版本 |

性能测试不应作为普通单元测试的 pass/fail；只有预注册 campaign 使用统计 gate。

## 9. Git 变更纪律

建议按以下边界提交：

1. source lock 与架构文档；
2. event schema + instrumentation；
3. contract tests；
4. lifecycle replay；
5. FS baseline adapter；
6. candidate policy；
7. engine/ablation；
8. microbench campaign；
9. E2E runner；
10. analysis 与结果摘要。

不要把“实现变化 + workload 变化 + 图表更新”放在一个提交里。正式结果对应的
commit 不再修改；后续修复产生新 run_id 和新结果。

