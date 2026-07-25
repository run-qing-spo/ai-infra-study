# Runtime and native-FS evidence index — 20260725T103442Z

对应报告：[ENV_REPORT.md](../../../ENV_REPORT.md)

## 判定摘要

| Gate | 结果 | 主证据 |
|---|---:|---|
| 干净 vLLM source/package identity | PASS | `raw/vllm024_clean_runtime_validation.json`、`raw/vllm024_clean_environment.freeze.txt` |
| 干净环境 PyTorch CUDA kernel | PASS | `raw/vllm024_clean_runtime_validation.json` |
| 干净环境 Qwen2.5-0.5B EngineCore + generation | PASS | `raw/vllm024_clean_qwen05b_smoke.log` |
| 实际 API/EngineCore PID 约束 | PASS | `raw/native_fs_api_pid_runtime.txt`、`raw/native_fs_enginecore_pid_runtime.txt` |
| 原生 FS cascade/store | PASS | `raw/native_fs_request.json`、`raw/native_fs_files_after.txt`、`raw/native_fs_service_prime.log` |
| 重启后原生 FS promotion/load | PASS | `raw/native_fs_revisit_request.json`、`raw/native_fs_revisit_metrics_after.txt`、`raw/native_fs_service_revisit.log` |
| uring-slab prototype contract/checksum | PASS | `raw/uring_prototype_pytest.log`、`raw/uring_prototype_full_checksum.json` |
| uring-slab prototype 短诊断 | PASS（非正式性能结果） | `raw/uring_prototype_diagnostic_microbench.json` |

## 运行时锁

- vLLM source tag：`v0.24.0`
- source commit：`ee0da84ab9e04ac7610e28580af62c365e898389`
- vLLM distribution：`0.24.0+cu129`
- vLLM module version：`0.24.0`
- PyTorch：`2.11.0+cu129`
- Python：3.12.3
- venv：`/root/uring-slab-experiments/venvs/vllm024-cu129-clean`
- 模型：`Qwen/Qwen2.5-0.5B-Instruct`
- smoke 固定开关：`VLLM_USE_FLASHINFER_SAMPLER=0`

`VLLM_USE_FLASHINFER_SAMPLER=0` 只关闭可选的 FlashInfer top-k/top-p
sampler；FlashAttention 2 仍被用于 attention。A/B 两侧必须使用同一开关。

`raw/vllm024_clean_environment.freeze.txt` 是正式依赖快照。该快照同时包含
cu12.9 runtime 与若干无 `-cu12` 后缀的 CUDA 13 工具包依赖；不能根据包名
猜测实际加载的 ABI。运行时校验直接确认 vLLM distribution、Torch build、
`torch.version.cuda == 12.9`、GPU kernel 与 tiering imports。旧
`raw/vllm024_environment.freeze.txt` 仅保留为清理前诊断证据，不再作为正式
环境锁。

## 原生 FS 字节与 token 对账

Prime 请求：

- prompt：411 tokens；
- 完整可 offload token：400 = 25 × 16；
- 每个 offloaded block：196,608 bytes；
- 新增 block 文件：25；
- vLLM store metric：4,915,200 bytes；
- 文件数据字节：25 × 196,608 = 4,915,200 bytes。

使用相同配置、FS root 与 `PYTHONHASHSEED=0` 重启后，同一请求：

- local prefix hit：0 tokens；
- external query：411 tokens；
- external hit：400 tokens；
- `prompt_tokens_by_source{source="external_kv_transfer"}`：400；
- vLLM load metric：4,915,200 bytes；
- EngineCore `/proc/<pid>/io` `read_bytes` 增量：4,915,200 bytes。

这构成 native FS 的
`GPU → CPU → FS → restart → FS → CPU → GPU` 功能闭环。它不是性能结论。

## 失败边界

- `raw/vllm024_runtime_validation_default_cu13_fail.json`：从默认 PyPI
  wheel 安装后，`vllm` 导入寻找 `libcudart.so.13`；Torch cu129 自身仍
  checksum PASS。随后用官方 v0.24.0 cu129 release wheel修复。
- `raw/vllm024_qwen05b_flashinfer_nvcc_fail.log`：默认 FlashInfer sampler
  首次 JIT 需要可发现的完整 CUDA toolkit/nvcc；当前客户机没有。正式 A/B
  统一关闭该 sampler。
- `raw/vllm024_runtime_validation_cu129_validator_false_fail.json`：初版
  validator 把 `vllm.__version__ == 0.24.0` 与 distribution
  `0.24.0+cu129` 混为一个字段，产生误判。已由双字段校验替代，不作为环境
  FAIL。
- `raw/vllm024_runtime_validation_cu129.json` 与
  `raw/vllm024_qwen05b_smoke.log`：清理前 cu129 venv 的成功记录。原生 FS
  全闭环运行于该 venv；其 vLLM/Torch 主版本与干净 venv 相同。之后用干净
  venv 独立重复 source/package/CUDA/tiering 校验与真实模型生成，均 PASS。

## uring-slab prototype

- 旧策略测试：16 passed；
- 旧 uring 与 C++ pool smoke：PASS；
- 新增全区 gate：64 × 1 MiB，O_DIRECT store、全区清零、load，
  SHA-256 `e55873771fd4691b8254020a8de3157deec1c6426492847ed9ffa194f54f4fd1`
  前后一致，128 个 I/O、0 failed；
- 256 MiB/方向的四路径结果只用于确认功能与量级。样本过短、未重复、
  未随机化，禁止用于核心性能 claim。

## 实际 PID

Prime 服务：

- API PID：25741；
- EngineCore PID：25855；
- 两者均在 `session-239.scope`；
- EngineCore affinity：`0-15`；
- CPU quota：none；
- memory max/high：none；
- cgroup I/O throttle：未设置；
- `RLIMIT_NOFILE`：soft 65,535 / hard 1,048,576；
- `RLIMIT_MEMLOCK`：6,308,257,792 bytes。

这些 PID 已在采集后通过 SIGTERM 有序停止。revisit 服务 PID
27623/27731 也已停止。

## 完整性

在本目录运行：

```bash
shasum -a 256 -c SHA256SUMS
```

raw evidence 默认不提交 Git；`SHA256SUMS`、本索引和最终报告提交 Git。
