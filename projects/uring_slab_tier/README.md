# uring-slab tier

这是一个可独立拆分为单独仓库的实验项目。它只研究一个问题：

> 在不改变 vLLM 0.24.0 上层语义的前提下，以 uring-slab 替换原生 FS
> second-tier engine，能否改善已确定会命中 second tier 的工作负载。

当前阶段尚未实现 candidate。先完成以下顺序：

1. 冻结原生 FS 与未来 candidate 共用的最小 contract；
2. 用同一个观测 wrapper 和 runner 重复运行原生 FS；
3. 判断目标云机器的噪声是否足以支持可信 A/B；
4. 机器通过后，才选择性迁移旧 engine 并实现 vLLM 0.24 adapter。

截至 2026-07-26，前三项已完成。当前 3090 的判定是
**conditional pass**：功能与写侧 PASS，固定访问读侧 WARN，native FS
整体严格噪声 gate FAIL。项目可以进入 candidate 实现，但正式 A/B 必须使用
成对随机化和由 A/A 数据冻结的 practical-effect floor。详见
`docs/DECISION_0001_3090_NOISE_GATE.md`。

## 当前入口

- `docs/SECONDARY_TIER_CONTRACT_V1.md`：冻结的实验路径 contract；
- `docs/DECISION_0001_3090_NOISE_GATE.md`：3090 噪声判定与正式 A/B 门槛；
- `configs/native_fs_noise_v1.json`：预注册的 native-FS 噪声 campaign；
- `src/uring_slab_tier/instrumentation.py`：两种 backend 共用的观测 wrapper；
- `src/uring_slab_tier/native_fs_noise.py`：直接实例化上游
  `FileSystemTierManager` 的 runner；
- `src/uring_slab_tier/noise_gate.py`：不看 candidate 结果的机器噪声判定；
- `tests/`：不依赖 vLLM 安装的 instrumentation 和判定逻辑测试。
- `evidence/INDEX.md`：环境、正式/无效 campaign 与源码快照索引。

## 运行

目标机必须使用锁定的干净 venv，并在进程启动前固定
`PYTHONHASHSEED=0`：

```bash
cd /root/uring-slab-experiments/source/uring-slab-tier
PYTHONHASHSEED=0 \
PYTHONPATH="$PWD/src" \
/root/uring-slab-experiments/venvs/vllm024-cu129-clean/bin/python \
    -m uring_slab_tier.native_fs_noise \
    --config configs/native_fs_noise_v1.json \
    --output-root /root/uring-slab-experiments/results/native-fs-noise \
    --data-root /root/uring-slab-experiments/data/native-fs-noise
```

每个 run 的数据文件在校验后删除；manifest、事件、结果、失败记录和
campaign summary 保留。任何 run 目录都不覆盖。

## 边界

- 正式 baseline 直接使用 vLLM 0.24.0 的
  `FileSystemTierManager`，不复刻一个“类似 FS”的实现；
- 当前 runner 只做 tier-contract 噪声资格检查，不产生 candidate 性能结论；
- 不 drop cache、不修改 sysctl/cgroup/mount；
- 单个 run 的 backing data 上限为 1 GiB；
- `ai-infra-study` 中的旧 prototype 和历史结果都不自动继承为有效证据。
