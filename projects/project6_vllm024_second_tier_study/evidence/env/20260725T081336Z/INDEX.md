# Environment evidence index — 20260725T081336Z

对应报告：[ENV_REPORT.md](../../../ENV_REPORT.md)

## 证据状态

| 文件 | 用途 | 状态 |
|---|---|---|
| `raw/static_env.txt` | OS、CPU、memory、GPU、filesystem、block queue、io_uring policy、工具链 | primary |
| `raw/cloud_runtime_v2.txt` | QEMU 边界、实验路径、service PID、cgroup chain、CUDA smoke | primary |
| `raw/runtime_inventory.txt` | conda、pip、vLLM command/process/image inventory | primary |
| `raw/fio_io_uring_options.txt` | fio io_uring `nowait` 定义与可用 tracepoints | primary |
| `raw/fio_uring_odirect_checksum.json` | 256 MiB SHA-256 write/verify | primary |
| `raw/fio_uring_odirect_strace.json` | 16 MiB traced checksum smoke | primary |
| `raw/strace_uring_odirect.6263` | `O_DIRECT` open 与 io_uring syscalls | primary |
| `raw/fio_uring_odirect_nowait.json` | 5 秒 NOWAIT read 资格探针 | primary |
| `raw/bpftrace_uring_normal_filtered.txt` | fio-filtered 普通 write+verify punt count | diagnostic |
| `raw/bpftrace_uring_nowait_filtered.txt` | fio-filtered NOWAIT read punt count | primary |
| `raw/fio_uring_normal_trace_filtered.json` | 上述普通 trace 对应 fio result | diagnostic |
| `raw/fio_uring_nowait_trace_filtered.json` | 上述 NOWAIT trace 对应 fio result | primary |
| `raw/fio_uring_direction_prepare.json` | direction split 的 64 MiB prepare/verify | primary |
| `raw/fio_uring_normal_randread.json` | 普通 randread result | primary |
| `raw/bpftrace_uring_normal_randread.txt` | 普通 randread fio-filtered punt count | primary |
| `raw/fio_uring_normal_randwrite.json` | 普通 overwrite randwrite result | primary |
| `raw/bpftrace_uring_normal_randwrite.txt` | 普通 randwrite fio-filtered punt count | primary |
| `raw/cleanup_v2.txt` | 所有 probe 后无 audit `.bin`；用户原有 16 GiB file 未触碰 | primary |

## 被保留但不用于最终判定的证据

- `raw/cloud_runtime.txt`：collector schema v1 使用普通文件 size 判断 procfs/cgroup
  pseudo-file 是否为空，导致部分内容误标 `<empty>`；由 schema v2 替代。
- `raw/cleanup.txt`：在最后一轮 direction-split probe 之前采集；由
  `cleanup_v2.txt` 替代。
- `raw/bpftrace_uring_normal.txt`、`raw/bpftrace_uring_nowait.txt`：最初为
  host-wide tracepoint count；由带 `comm == "fio"` filter 的版本替代。
- `raw/fio_uring_normal_trace.json`、`raw/fio_uring_nowait_trace.json`：
  fio 数据有效，但配对的 BPF count 未过滤；仅作诊断留档。
- `raw/strace_uring_odirect.6261`、`.6262`：fio parent/control process；
  数据路径证据主要位于 `.6263`。

## 核心命令参数

### Checksum gate

```text
fio
  --name=uring_odirect_checksum
  --size=256M --bs=1M --rw=write
  --ioengine=io_uring --iodepth=32 --direct=1
  --verify=sha256 --verify_fatal=1 --do_verify=1
  --verify_state_save=0 --refill_buffers=1
```

### NOWAIT read gate

```text
fio
  --name=uring_odirect_nowait
  --size=256M --bs=256K --rw=randread
  --ioengine=io_uring --iodepth=16 --direct=1 --nowait=1
  --time_based=1 --runtime=5
```

### Direction-split diagnostic

```text
fio --rw=randread  --size=64M --bs=256K --ioengine=io_uring \
  --iodepth=16 --direct=1 --time_based=1 --runtime=3

fio --rw=randwrite --size=64M --bs=256K --ioengine=io_uring \
  --iodepth=16 --direct=1 --time_based=1 --runtime=3
```

两个方向均由 `trace_io_uring_punts.bt` 同时计数
`io_uring_queue_async_work` 和 `io_uring_req_failed`，并过滤
`comm == "fio"`。

## 清理

所有由本次审计创建的 `.bin` 文件均已删除。远端原有
`/root/vllm-tier-probe/fio-probe-16g.bin` 的 size、inode 与 mtime 在最终
cleanup evidence 中记录，未被本次命令作为测试目标。

## 完整性

在本目录运行：

```bash
shasum -a 256 -c SHA256SUMS
```

raw evidence 默认不提交 Git；`SHA256SUMS`、本索引和最终报告提交 Git。

