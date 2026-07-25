# ENV_REPORT

审计 ID：`20260725T081336Z`  
审计窗口：2026-07-25 08:10–08:31 UTC  
运行时复验 ID：`20260725T103442Z`  
目标：用户提供的远端 GPU 云虚拟机（公网地址不写入可公开材料）  
实验根目录：`/root/uring-slab-experiments`

## 1. 总结判定

当前机器已经具备 vLLM 0.24.0 原生 FS tier 与 `uring-slab` 数据面实验的
运行资格。正式性能 campaign 仍需先完成 adapter/contract gate 与 native-FS
重复噪声标定；这是实验代码/统计流程 gate，不再是机器环境 blocker。

| Gate | 判定 | 结论 |
|---|---:|---|
| vLLM 0.24.0 GPU runtime | **PASS（有锁定开关）** | 干净 venv 中的官方 `0.24.0+cu129` wheel、Torch `2.11.0+cu129`、CUDA checksum、tiering imports、Qwen2.5-0.5B EngineCore 与真实 generation 全部通过；固定 `VLLM_USE_FLASHINFER_SAMPLER=0` |
| 实际 vLLM 服务 PID 资源约束 | **PASS** | 已分别采集 API PID 25741 与承载 scheduler/FS tier 的 EngineCore PID 25855；二者随后有序停止 |
| 原生 FS store/load 闭环 | **PASS** | 400 tokens / 4,915,200 bytes 完成 cascade；重启后 local hit=0、external hit=400 tokens，device read 与 vLLM load bytes 均为 4,915,200 |
| 实验路径与客户机块设备映射 | **PASS** | `/root/uring-slab-experiments` → XFS `/dev/vda1` → virtio `/dev/vda` |
| `io_uring + O_DIRECT` 数据完整性 | **PASS** | fio SHA-256 写后校验通过，job `error=0`；strace 同时观察到 `O_DIRECT`、`io_uring_setup` 与 `io_uring_enter` |
| `RWF_NOWAIT` read | **PASS** | fio io_uring `nowait` randread 运行成功，job `error=0`，过滤后的 `io_uring_queue_async_work=0` |
| `RWF_NOWAIT` write | **UNKNOWN** | 本项目当前 engine 不显式要求它，本次没有把它作为写路径资格门槛 |
| 普通 io_uring load 是否 punt | **PASS / observed no punt** | 已写文件上的普通 O_DIRECT randread：30,681 I/O，`queue_async_work=0` |
| 普通 io_uring store 是否 punt | **WARN** | 已写文件上的普通 O_DIRECT randwrite：20,274 I/O，观察到 3,099 次 `queue_async_work` |
| 物理介质、宿主 cache、共享程度、云 QoS | **UNKNOWN** | 客户机边界外不可见；不能根据 virtio 的 `rotational=1` 推断物理 HDD/SSD |
| 延迟噪声 | **WARN** | 本次未出现秒级 I/O，但约 3 万次随机读中最大值约 208–211 ms；既往秒级长尾仅作为先验 WARN，正式阈值待 native-FS 重复实验确定 |

因此有三个不同层次的结论：

1. **“这台机器能不能跑 uring-slab？”——能。** ring 创建、direct I/O、
   checksum、普通 load 和显式 NOWAIT read 都已通过。
2. **“vLLM 0.24.0 原生 FS 的真实 store/load 路径能不能跑？”——能。**
   已完成一次 store、服务重启和一次持久化 FS promotion/load。
3. **“现在能不能直接产出正式性能结论？”——还不能。** 环境已经解除，
   但必须先完成独立 adapter、统一 contract/观测和 native-FS 重复噪声标定，
   再进入预注册的微基准与端到端 campaign。

## 2. 证据口径

本文使用三种证据等级：

- **OBSERVED**：由目标客户机、目标路径或目标进程直接返回；
- **INFERRED**：由多个 observed facts 推导，明确说明边界；
- **UNKNOWN**：云平台没有向客户机暴露，或当前权限/实验没有覆盖。

`PASS` 只对客户机可见的有效契约负责，不把虚拟设备属性扩写成物理宿主事实。

## 3. 云环境边界

### 3.1 OBSERVED

- `systemd-detect-virt=qemu`，DMI vendor 为 QEMU，CPU 带 `hypervisor` flag；
- 客户机在线 CPU 为 16 个（`0-15`），单 NUMA node；
- 客户机可见内存为 50,466,082,816 bytes，swap 约 4 GiB；
- PCI 中存在 virtio block、virtio balloon 和 NVIDIA AD102 设备；
- GPU 被呈现为 `NVIDIA GeForce RTX 4090`，驱动 550.78，
  `nvidia-smi` 报告 49,140 MiB；
- 初次审计时已有 PyTorch 2.2.2 / CUDA runtime 12.1；运行时复验使用隔离
  Python 3.12 venv、PyTorch `2.11.0+cu129` 与官方 vLLM
  `0.24.0+cu129`，GPU int64 checksum 和模型生成均通过。

`lscpu` 的在线 CPU 数为 16，而 `nproc --all` 返回 128。正式实验应使用
`/sys/devices/system/cpu/online`、`lscpu` 与实际 PID affinity，不得把
`nproc --all=128` 当作可用核数。

### 3.2 INFERRED

- NVIDIA PCI function 在客户机中由 `nvidia` 驱动管理，符合设备直通式呈现，
  但本审计不能证明云平台底层没有额外的资源管理；
- virtio balloon 的存在说明内存是虚拟机呈现资源；本次没有观察到可归因的
  balloon 调整；
- GPU 名称与 49,140 MiB 的非标准组合说明必须把它当作“平台呈现的 GPU
  配置”，不能拿消费级 4090 的公开规格替代实测值。

### 3.3 UNKNOWN

- 物理 CPU 的频率、turbo、宿主超售与长期 steal；
- `/dev/vda` 后面的物理 SSD/HDD、RAID、网络盘或缓存层；
- QEMU cache mode、宿主 page cache、阵列 write-back cache；
- 云厂商 IOPS/bandwidth burst、后台 QoS 与 noisy-neighbor；
- GPU 是否存在客户机不可见的宿主级调度约束。

## 4. CPU、内存与 I/O 约束

运行时复验分别采集了：

- API PID 25741；
- EngineCore PID 25855（scheduler、CPU primary 与 FS tier 位于此进程）。

二者位于 cgroup v2
`/user.slice/user-0.slice/session-239.scope`，有效结果为：

| 资源 | 实际服务观察值 | 解释 |
|---|---|---|
| CPU quota | `cpu.max=max 100000` | 没有额外 cgroup quota；VM 仍只有 16 个 online vCPU |
| CPU affinity/cpuset | `0-15` | 服务可使用 16 个 online vCPU |
| Memory | `memory.max=max`, `memory.high=max` | 没有额外 cgroup memory cap；VM 可见约 47 GiB |
| I/O | `io.max` 为空，`io.weight=default 100` | 没有客户机 cgroup I/O throttle；不代表云平台没有 QoS |
| PIDs | session `max`，上层 user scope 126,873 | 当前不是瓶颈 |
| `RLIMIT_MEMLOCK` | 6,308,257,792 bytes | 若注册大量 fixed buffers，需按实际池大小重新核算 |
| `RLIMIT_NOFILE` | soft 65,535 / hard 1,048,576 | 原生 file-per-block FS 与 slab smoke 均未触顶 |

这是本次启动方式下实际 PID 的认证结果。若正式 campaign 改用 systemd、
容器或其他 launcher，仍需在每个 run manifest 中重新采集。

## 5. 实验目录和存储路径

`/root/uring-slab-experiments` 的客户机可见路径为：

```text
/root/uring-slab-experiments
└── / (XFS, 4 KiB filesystem block)
    └── /dev/vda1 (major:minor 253:1)
        └── /dev/vda (virtio block, 200 GiB virtual disk)
```

审计时可用空间约 164,462,039,040 bytes（154 GiB）。客户机可见队列属性：

- logical/physical block size：512/512 bytes；
- scheduler：`mq-deadline`；
- `nr_requests=256`；
- `max_sectors_kb=1280`；
- `write_cache=write back`，`fua=0`；
- `wbt_lat_usec=75000`；
- `rotational=1`。

最后一项只是 virtio 设备属性，**不能据此声称底层是机械盘**。同样，
`O_DIRECT` 只证明绕过客户机 page cache，不证明绕过 QEMU/宿主/阵列缓存。

## 6. io_uring、O_DIRECT 与 NOWAIT

### 6.1 数据完整性 PASS

fio 3.36 使用：

- `ioengine=io_uring`；
- `direct=1`；
- 256 MiB；
- 1 MiB block；
- queue depth 32；
- `verify=sha256`、`verify_fatal=1`、写后 verify。

结果：

- write 268,435,456 bytes / 256 I/O；
- verify read 268,435,456 bytes / 256 I/O；
- fio 进程退出成功，job `error=0`；
- strace 观察到
  `openat(..., O_RDWR|O_CREAT|O_DIRECT, ...)`、
  `io_uring_setup(...) = 7` 和后续 `io_uring_enter(...)`。

这构成客户机目标路径上的 `io_uring + O_DIRECT + checksum PASS`。它不是
持久性/断电恢复证明，因为没有验证宿主 cache flush 或断电语义。

### 6.2 NOWAIT 与 io-wq

fio 的 io_uring engine 明确把 `nowait` 定义为“Use RWF_NOWAIT for
reads/writes”。本次只把它用于 randread：

- 5 秒资格探针：47,764 次 256 KiB read，job `error=0`；
- 过滤到 fio 的 3 秒 trace：31,302 次 read，
  `fio_queue_async_work=0`、`fio_request_failed=0`。

普通、未设置 `RWF_NOWAIT` 的方向拆分结果：

| 路径 | I/O 数 | p99 | max | `queue_async_work` | fio error |
|---|---:|---:|---:|---:|---:|
| randread | 30,681 | 3.850 ms | 208.090 ms | 0 | 0 |
| randwrite overwrite | 20,274 | 7.504 ms | 32.213 ms | 3,099 | 0 |

结论限定为：

- 普通 load 在这个短窗口中没有观察到 io-wq punt；
- 普通 store 即使覆盖已写 extent，仍有部分请求进入 io-wq；
- 这不会阻止现有 uring-slab 正确运行，但会影响 store CPU、尾延迟以及
  load/store 混压解释；
- 不把 `3,099 / 20,274` 直接当作稳定 punt ratio；正式实验必须随每个
  workload 同步采集 trace/worker/CPU 证据。

## 7. vLLM 0.24.0 与原生 FS tier 运行时

### 7.1 版本与 CUDA

锁定身份：

- source tag：`v0.24.0`；
- source commit：`ee0da84ab9e04ac7610e28580af62c365e898389`；
- distribution：`vllm==0.24.0+cu129`；
- module version：`vllm.__version__ == 0.24.0`；
- PyTorch：`2.11.0+cu129`；
- Python：3.12.3；
- 正式 venv：`/root/uring-slab-experiments/venvs/vllm024-cu129-clean`。

运行验证：

- vLLM、`OffloadingConnector`、`SecondaryTierManager`、
  `SecondaryTierFactory`、`FileSystemTierManager` import PASS；
- RTX 4090 CUDA int64 checksum PASS；
- Qwen2.5-0.5B-Instruct EngineCore 初始化、FlashAttention 2、KV cache
  分配与 8-token generation PASS。

环境固定 `VLLM_USE_FLASHINFER_SAMPLER=0`。默认 FlashInfer top-k/top-p
sampler 会在首次 JIT 时寻找完整 `nvcc`/CUDA_HOME，本机没有可发现的完整
CUDA 12.9 toolkit；关闭它后使用 vLLM 的回退 sampler，attention 仍使用
FlashAttention 2。该开关必须在 FS 与 uring A/B 两侧完全相同。

默认 PyPI wheel 曾因寻找 `libcudart.so.13` 导入失败；最终环境改用
vLLM 官方 v0.24.0 release 的 cu129 wheel。两次失败日志均保留，禁止用
软链接伪装 ABI。

原生 FS 全闭环在清理前的官方 cu129 venv 中完成；它与干净 venv 的
vLLM/Torch 主版本相同。之后已在干净 venv 中独立重跑 source/package/CUDA/
tiering 校验和真实模型生成，均 PASS。正式后续脚本默认使用干净 venv，其
完整 freeze 是主依赖锁；清理前 freeze 仅作诊断记录。

### 7.2 原生 FS store

原生配置：

- `TieringOffloadingSpec`；
- CPU primary：268,435,456 bytes，LRU；
- offloaded block size：16 tokens；
- `offload_prompt_only=true`；
- secondary：内置 `fs`，默认 16 read-priority + 16 write-priority
  threads；
- `PYTHONHASHSEED=0`；
- root：`/root/uring-slab-experiments/data/vllm-fs-smoke`。

第一次服务处理 411-token prompt：

- 25 个完整 block，即 400 tokens；
- 每个 block 196,608 bytes；
- vLLM store bytes：4,915,200；
- 新增 25 个 `.bin` 文件，总 block 数据字节 4,915,200；
- EngineCore `write_bytes` 增量与文件/metric 量级闭合。

### 7.3 重启后的 FS promotion/load

第一次服务 SIGTERM 后，用相同模型、配置、root 与 hash seed 重启。此时
GPU/CPU cache 均为空，FS 文件保留。再次发送同一 prompt：

- local prefix hit：0；
- external query：411 tokens；
- external hit：400 tokens（97.3%）；
- `prompt_tokens_by_source{source="external_kv_transfer"}`：400；
- vLLM load bytes：4,915,200；
- EngineCore `/proc/<pid>/io` `read_bytes` 增量：4,915,200。

这证明上层真实执行了 FS existence lookup、promotion 到 CPU primary 和
CPU→GPU load，而不是仅仅“初始化了一个未使用的 FS manager”。

### 7.4 uring-slab prototype 资格检查

选择性移植的旧原型在独立 venv 中重新构建：

- 16 个策略/账本测试 PASS；
- uring 与 C++ blocking-pool 自带 smoke PASS；
- 新增 64 × 1 MiB 全区 checksum gate：
  O_DIRECT store → 全区清零 → load，前后 SHA-256 一致；
- 128 个 block I/O，2 个 job，0 failed，0 SQ-full。

一次 256 MiB/方向的四路径短诊断全部成功，但仅用于确认量级和发现变量，
没有重复、随机化、稳态窗口或置信区间，**不得进入正式性能 claim**。

## 8. 延迟 WARN 与正式噪声阈值

存储资格探针和本轮 native-FS store/load 都不是性能基准。前者使用很小的
重复工作集；后者只做一次功能闭环。宿主 cache 与 QoS 未知，且尚未完成
native FS 的随机化重复标定。

当前 audit 没有观察到秒级 I/O，但普通 randread 和显式 NOWAIT randread
都出现了约 208–211 ms 的最大值，而 p99 仅约 3.6–3.9 ms，说明该虚拟存储
路径存在明显高分位之后的尾部扩张。

处理方式：

1. 既往观察到的秒级长尾只记为 `WARN / legacy observation`，不进入新项目
   的数值结论；
2. 正式噪声阈值由 vLLM 0.24.0 native FS 在相同路径、相同时间窗口的重复
   run 测量；
3. 在得到重复基线前，不因一次快或一次慢修改 trace 以追求正结果。

## 9. 进入正式实验前的解除条件

已完成的环境 gate：

1. 已锁定 vLLM `v0.24.0` source 与官方 cu129 wheel；
2. 已启动真实 vLLM FS-tier 服务；
3. 已对 parent/EngineCore PID 重跑 cloud runtime collector；
4. 已记录实际 PID 的 CPU affinity、cgroup CPU/memory/I/O、memlock 与
   nofile；
5. 已完成 native FS store 与跨重启 load 功能闭环；
6. 已确认正式实验路径及 XFS/virtio 客户机边界。

正式性能 campaign 前仍需完成：

1. 为 native FS 与 candidate 建立同一 contract/instrumentation gate；
2. 用新框架重跑 uring-slab 正式微基准，不引用旧数据或本次短诊断作为结果；
3. 运行 native FS 重复噪声标定并预注册异常阈值；
4. 每次 run manifest 固定 wheel、source commit、模型 revision、sampler
   开关、实验路径与实际 PID 约束；
5. candidate adapter 保持 vLLM 上层语义不变，只替换 second-tier engine。

不需要把以下 UNKNOWN 变成已知才可实验：物理介质型号、宿主 cache、
云平台共享程度和后台 QoS。它们必须保留为 external-validity limitation，
并通过随机化顺序、重复实验、同机 A/B 与系统指标采集控制影响。

## 10. 证据

静态/存储资格证据见
[evidence/env/20260725T081336Z/INDEX.md](evidence/env/20260725T081336Z/INDEX.md)；
vLLM、实际 PID、native FS 与 prototype 运行证据见
[evidence/runtime/20260725T103442Z/INDEX.md](evidence/runtime/20260725T103442Z/INDEX.md)。
