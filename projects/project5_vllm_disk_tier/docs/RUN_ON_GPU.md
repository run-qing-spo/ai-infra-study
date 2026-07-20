# GPU 机器上的完整测试流程

目标:证明 "SPSC + 单线程 io_uring 批量提交 + O_DIRECT 单大文件" 的磁盘 tier
在长上下文前缀复用场景下, 相对 vLLM 自带 fs tier(线程池 + file-per-block +
O_DIRECT)降低 revisit TTFT 和 CPU 占用, 且不产生负优化。

对照组设计(所有组跑同一份负载脚本):

| 组 | 配置 | 回答的问题 |
|----|------|-----------|
| A  | 无 offload | 重算 prefill 的 TTFT 下界(最坏基线) |
| B  | 仅 CPU tier | CPU 容量被 churn 挤爆后还剩多少收益 |
| C1 | CPU + fs tier | 官方磁盘 tier 的水平(主要对照) |
| C2 | CPU + uring tier(本项目) | 我们的水平 |
| D  | C2 但负载全 miss | 负优化检查:挂着 tier 但没命中时的开销 |

---

## 0. 环境与已知坑

- 平台:AutoDL RTX 4090 × 1(接入方式见 `projects/offload/HANDOFF.md`)。
- **AutoDL 的 non-interactive ssh 没有 conda/cuda PATH**,每条 ssh 命令前:
  ```bash
  export PATH=/root/miniconda3/bin:/usr/local/cuda/bin:$PATH
  ```
  (或按 HANDOFF 的办法写进 `/root/.bashrc` 顶部,一劳永逸。)
- 数据盘 `/root/autodl-tmp`:所有 backing 文件、模型缓存都放这里。
  先确认文件系统与剩余空间:`df -T /root/autodl-tmp`(ext4/xfs 都支持
  O_DIRECT;NFS 类网络盘不行,那就换 `--dir`)。
- 内核:`uname -r`。**5.15 + md RAID 数据盘会让 io_uring 100% punt 到
  io-wq**(md 的 REQ_NOWAIT 支持 5.17 才进主线),引擎退化成隐形线程池,
  CPU 优势不成立 —— 详见 BENCH_ANALYSIS.md 的 punt 机制一节。在这种宿主上跑 e2e 依然
  有效,但对 C2 的预期要按 §4.4 的修正版看。
  **【BENCH_ANALYSIS 修正】**punt 是警报不是判决:一台 md RAID1 + 企业盘宿主实测
  iou_wrk 非零但 uring 三轴全赢(串行单列也够打满低延迟阵列, io-wq 税
  低于线程池)。筛机流程改成:iou_wrk 非零 → 先跑四引擎微基准, 用对照
  数字下判决, 不直接释放。
- **conda libstdc++ 冲突**(两台 AutoDL 都踩过):`make` 用系统 g++,
  产物要 `GLIBCXX_3.4.30`,conda Python 却优先加载 miniconda 自带的旧
  libstdc++ → import 时报 `GLIBCXX_3.4.30 not found`。修法任选:
  ```bash
  export LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libstdc++.so.6   # 临时, 每 shell
  ln -sf /usr/lib/x86_64-linux-gnu/libstdc++.so.6 \
        /root/miniconda3/lib/libstdc++.so.6                    # 一劳永逸
  ```
- **HF 模型下载三件套**(2026-07-10 组 A 两次死在这才定位):国内直连
  官方 hub 时 xet 后端(HF 新的分块存储)去 `cas-server.xethub.hf.co`
  拿数据会 401;学术加速代理又会让镜像的 TLS 握手超时。稳定组合:
  ```bash
  unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY all_proxy ALL_PROXY
  export HF_ENDPOINT=https://hf-mirror.com   # 国内镜像, 直连
  export HF_HUB_DISABLE_XET=1                # 镜像不含 xet 凭证, 退回普通 HTTP
  export HF_HOME=/root/autodl-tmp/hf         # 缓存放数据盘, 保 30G 系统盘
  ```
  三个 export 已进 `run_e2e_overnight.sh`;代理是 shell 环境的事,起
  脚本前自查 `env | grep -i proxy`。断点续传按**文件**粒度(下完的
  blob 直接跳过,半截的走 Range 续传),中断重跑不亏 —— 进度条的
  "Fetching 0/14"和字节数是本次会话的计数,不代表缓存状态。
- **新机器三查再跑长任务**(缺一个就浪费一晚):`make` 编 .so、
  `python3 -c "import kv_uring_tier.manager"` 验接口、`tmux` 装了没。
  前两条 `run_e2e_overnight.sh` 预检已代查。
- **vLLM 内部接口没有稳定性承诺**(2026-07-10 实战踩雷):0.24 删了
  `LookupResult` 枚举(`lookup()` 改返回 `bool | None`),而 manager
  旧代码把"vLLM 在不在"和"名字对不对"混在同一个 `try/except
  ImportError` 里 —— 枚举一缺,整块 import 静默滑进 Mac 单测用的替身
  分支,`RequestOffloadingContext` 变成没有 `policy` 字段的空壳,serve
  启动一切正常,**第一个请求进 scheduler 才炸** AttributeError。教训:
  环境探测和接口校验必须分开,宿主上 import 失败要炸在启动阶段(修复
  ae0763f8,manager.py 已改两段式)。每次升 vLLM 后先跑
  `python3 -c "import kv_uring_tier.manager"` 对合同。

## 1. 安装依赖

```bash
# vLLM:tiering 是 2026 年才进主线的新代码, 必须用足够新的版本
pip install -U vllm
# 验证 tiering 存在(过不了就 pip install -U --pre vllm 或从源码装):
python3 -c "from vllm.v1.kv_offload.tiering.factory import SecondaryTierFactory; print('tiering OK')"

# 编译依赖
apt install -y liburing-dev        # Ubuntu 22.04 自带 liburing 2.1, 够用
apt install -y fio                 # 盘上限标定用(第 3 步)
pip install pybind11 pytest openai
```

## 2. 部署本项目

Mac 上打包三个相互依赖的 project 目录传上去(引擎 include 了 P2/P3 的头):

```bash
# 本地
cd ~/ai-infra-study/projects
tar czf /tmp/p5.tgz project2_spsc/include project3_async_IO \
        project5_vllm_disk_tier
scp -P <port> /tmp/p5.tgz root@<host>:/root/

# 远端
cd /root && tar xzf p5.tgz && cd project5_vllm_disk_tier
make                       # 产出 python/kv_uring_tier/_kvtier*.so
pip install -e .           # 注册 vllm.general_plugins entry point
python3 -m pytest test/ -q # 策略层单测(9 个)

# 验证 plugin 注册成功:
python3 - <<'EOF'
from vllm.plugins import load_general_plugins
load_general_plugins()
from vllm.v1.kv_offload.tiering.factory import SecondaryTierFactory
print("registered:", list(SecondaryTierFactory._registry))  # 应含 'uring'
EOF
```

## 3. 引擎冒烟 + 微基准(不涉及 vLLM)

**新宿主先跑一次 fio 盘上限标定**(一次性,不进例行预检;换宿主/换数据盘
才重跑)。目的:拿到这块盘在**我们的 IO 形状**下的带宽上限,之后
"引擎打没打满盘"就有客观分母,不用拿引擎自己的数字互相佐证。
bs 必须对齐真实 block 大小(理由同微基准 --block-mb:盘的上限随请求
大小变,小了测出来偏低),iodepth 对齐引擎 QD:

```bash
cd /root/autodl-tmp
fio --name=wceil --rw=randwrite --bs=1M --size=8G --direct=1 \
    --ioengine=io_uring --iodepth=32 --numjobs=1 \
    --runtime=30 --time_based --group_reporting
fio --name=rceil --rw=randread  --bs=1M --size=8G --direct=1 \
    --ioengine=io_uring --iodepth=32 --numjobs=1 \
    --runtime=30 --time_based --group_reporting
rm -f wceil* rceil*
```

两个数(写/读 MB/s)记进当次 RESULTS.md 抬头。注意 fio 的 io_uring 在
punt 宿主(§0)上吃同一笔 io-wq 税 —— 这正合适:它代表"理想 io_uring
用户在这台机器的天花板",引擎该跟它比。若怀疑单 job 没打满盘,加
`--numjobs=4` 对照一次,差值就是还没提取的余量。

```bash
# 正确性:64 块写→抹→读→逐字节校验
python3 bench/bench_engine.py --smoke --dir /root/autodl-tmp

# 三引擎对比(uring / pool=复刻fs语义 / pool-slab=消融组)
# --block-mb 设成目标模型的真实 stride:起一次 vLLM 后从日志或
# /metrics 里读 kv_bytes_per_offloaded_block, 4090+Qwen2.5-7B 约 1 MiB 量级
python3 bench/bench_engine.py --dir /root/autodl-tmp \
    --block-mb 1 --blocks 2048 --job-blocks 32 --json > micro.json

# sweep:批量大小对提交模型的影响(uring 收益应随 job-blocks 增大而放大)
for jb in 1 8 32 128; do
  python3 bench/bench_engine.py --dir /root/autodl-tmp --blocks 1024 \
      --job-blocks $jb --engines uring
done
```

微基准看四个数:`MB_s`(带宽)、`iops`、`cpu_util`(单线程提交省 CPU 的
直接证据, uring 组应显著低于 pool 组)、`syscalls`(提交模型差异的解释变量)。
`pool` 与 `pool-slab` 的差 = file-per-block 元数据开销;`pool-slab` 与
`uring` 的差 = 线程池 vs 单线程批量提交。两段拆开, 归因才站得住。

## 4. 端到端 TTFT 对比

### 4.1 起服务(五组配置)

共同参数:`--max-model-len 16384`,其余默认。**容量故意压小**,让
phase 2 的 churn 能把前缀从 GPU、CPU 逐级挤下去 —— 踩不到盘的配置量出来
全是安慰剂。CPU tier 给 4 GiB ≈ 4k 个 1 MiB block ≈ 2~3 个 8k 前缀,
16 个会话必然溢出到磁盘。

```bash
M=Qwen/Qwen2.5-7B-Instruct

# A 无 offload
vllm serve $M --max-model-len 16384

# B 仅 CPU tier (4 GiB)
vllm serve $M --max-model-len 16384 --kv-transfer-config '{
  "kv_connector": "OffloadingConnector", "kv_role": "kv_both",
  "kv_connector_extra_config": {"spec_name": "CPUOffloadingSpec",
    "cpu_bytes_to_use": 4294967296}}'

# C1 CPU + 官方 fs tier (20 GiB)
PYTHONHASHSEED=0 vllm serve $M --max-model-len 16384 --kv-transfer-config '{
  "kv_connector": "OffloadingConnector", "kv_role": "kv_both",
  "kv_connector_extra_config": {"spec_name": "TieringOffloadingSpec",
    "cpu_bytes_to_use": 4294967296,
    "secondary_tiers": [{"type": "fs", "root_dir": "/root/autodl-tmp/kv_fs"}]}}'

# C2 CPU + uring tier (本项目, 20 GiB)
vllm serve $M --max-model-len 16384 --kv-transfer-config '{
  "kv_connector": "OffloadingConnector", "kv_role": "kv_both",
  "kv_connector_extra_config": {"spec_name": "TieringOffloadingSpec",
    "cpu_bytes_to_use": 4294967296,
    "secondary_tiers": [{"type": "uring",
      "path": "/root/autodl-tmp/kv_tier.bin",
      "disk_bytes_to_use": 21474836480,
      "queue_depth": 32}]}}'
# queue_depth=32: 微基准 sweep 的 sweet spot (BENCH_ANALYSIS 的引擎参数一节)。
# 现在也是引擎默认值(旧默认 512 是错参数, 已改), 显式写出是为了实验
# 配置自文档化。gather threshold 在引擎内随 QD 联动 min(32, QD/2),
# 低 QD 不会 lockstep(BENCH_ANALYSIS 的引擎参数一节)。
```

起服务后在日志里确认:`Created secondary tier #0 (uring)`(C2)/
`(fs)`(C1)。看不到就说明 plugin 没加载, 回第 2 步查。

### 4.2 跑负载

```bash
# 每组 serve 起好后:
python3 bench/long_context_ttft.py --sessions 16 --prefix-words 6000 \
    --churn 24 --json group_C2.json
```

D 组(负优化检查):C2 的 serve 配置不变,负载换成互不重复的一次性请求
(`--sessions 0` 不适用,直接用 `--churn 40` 只跑 phase 2 的数据),对比
A 组同负载的吞吐和 TTFT —— 差值就是"挂着 tier 但全 miss"的纯开销,
应接近 0(store 是 fire-and-forget,不在关键路径)。

### 4.3 每组记录什么

- `revisit` 的 TTFT mean/p50/p99(核心指标)与 `revisit/prime` 比值
- `prime`/`churn` 的 TTFT(确认各组负载对等)
- serve 进程 CPU:负载期间 `pidstat -p <pid> 5`(C1 vs C2 的 CPU 差
  = 线程池 vs 单线程提交, 这是"减 CPU 瓶颈"论点的端到端证据)
- 磁盘侧:`iostat -x 5` 的 `w/s, r/s, wMB/s, rMB/s, aqu-sz`
  (uring 组队列深度应该打得更满)
- vLLM 侧:`curl localhost:8000/metrics | grep -i offload`
  (CPU tier 占用率、跳过的 store 数)
- C2 组结束后引擎计数器:目前通过日志/调试接口,`engine_stats()` 暴露
  `submit_calls / sq_full_events / bytes_*`
- **iou-wrk 采样**(punt 目击,C2 组):serve 是长进程,外部循环就行:
  ```bash
  PID=$(pgrep -f 'vllm serve' | head -1)
  while sleep 1; do
    echo "$(date +%T) $(grep -l '^iou-wrk' /proc/$PID/task/*/comm 2>/dev/null | wc -l)"
  done >> iou_wrk_C2.log
  ```
  注意引擎在 vLLM 的 worker 子进程里,如果主 PID 采出来恒 0,换
  `pgrep -f VLLM::Worker` 之类找真正持有 ring 的进程。

### 4.4 预期形状(用来判断实验是否跑对了)

- A 组 revisit ≈ prime(全部重算,ratio ≈ 1)
- B 组 revisit 略好于 A 但接近(CPU tier 已被 churn 挤光)
- C1/C2 revisit 明显低于 A(盘上命中);C2 ≤ C1,差距主要出现在
  **并发 revisit / 大批 promotion** 时(提交模型的差异要有并发才显形)
- 若 C2 反而更差:先查是不是没走 O_DIRECT(日志里有降级 warning)、
  churn 是否真的把 CPU tier 挤爆(metrics 里看)、盘是不是网络盘

**【punt 诊断后的修正版预期(md + 5.15 宿主)】**:微基准里 uring 的
load 吞吐输 pool ~1.8×、CPU 反而更高(punt 退化成隐形线程池),照搬到
e2e 的天真预期是 C2 ≥ C1 的 TTFT、CPU 也不占优。但 e2e 的变量组合完全
不同:promotion 是一个前缀几百 MB 的突发 load(不是 2 GiB 持续流),
瓶颈可能在 vLLM 调度而非盘;C1 的 Python 线程池在真实 serving 里要和
调度线程抢 GIL(微基准里没有这个竞争);store 是 fire-and-forget 不进
关键路径。所以 C2 vs C1 谁赢是**开放问题**,这正是跑 e2e 的价值 ——
无论哪边赢,配合 iou_wrk 采样都能讲清楚为什么。

**【2026-07-11 实测,开放问题关闭】**:两次独立运行 C2 均优于 C1
(revisit mean -11%、p99 -8%),且 pidstat 显形了 CPU 记账位置的差异
(fs 把 ~15 个点内核态 IO 记在 EngineCore,uring 挪给 io-wq)。四组
数字、CPU 账和同盘微基准复测全在 BENCH_ANALYSIS.md 的**端到端一节**;原始数据
`results_e2e_20260711_0058/`。上面第 3 条预期里"差距要并发才显形"
估计保守了 —— 串行 revisit 就已可测,并发加压见 COMPARE_PLAN 的 E3。

## 5. 结果落盘

每组的 `group_*.json` + `micro.json` + pidstat/iostat 摘录进
`RESULTS.md`,表格三列:A/C1/C2,行是上面的指标。面试叙事按
"微基准归因(syscall/CPU) → 端到端验证(TTFT)"两段讲。

## 6. 跑完

AutoDL 网页上点关机(停计费,数据盘保留)。backing 文件都在
`/root/autodl-tmp`,下次开机接着用;`kv_tier.bin` 是 cache 语义,
删了也无所谓。
