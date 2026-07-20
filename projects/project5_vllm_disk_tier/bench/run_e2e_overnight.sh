#!/usr/bin/env bash
# 无人值守跑一个实验矩阵的端到端 TTFT 对比(COMPARE_PLAN 第 2 步)。
# 设计目标:起脚本 → 断开 ssh → 第二天看 results_e2e_*/SUMMARY.txt。
#
# 用法(远端 GPU 机, 在项目根目录):
#   tmux new -s e2e
#   bash bench/run_e2e_overnight.sh e1        # Ctrl-B D 断开
#   bash bench/run_e2e_overnight.sh e2        # store 剖析: 纯 churn 循环, none/fs/uring × a/b/c
#   bash bench/run_e2e_overnight.sh e3        # 并发轴: fs/uring 各一次 serve, 扫 c∈{1,4,8,16}×3轮
#   bash bench/run_e2e_overnight.sh e3dentry  # dentry 预热对照: fs_warm/fs_ctrl 成对 + uring 锚点
#   bash bench/run_e2e_overnight.sh e4        # 读写混战: churn 流不停 + revisit 波 @c8 + ITL 探针
#   bash bench/run_e2e_overnight.sh e4phase   # 相位共振干预: uring 波间隔 19.3s×3 + 20s 参考点
#   bash bench/run_e2e_overnight.sh legacy    # 旧 A/B/C1/C2 四组
#   # 或者不用 tmux: nohup bash bench/run_e2e_overnight.sh e1 > overnight.log 2>&1 &
#
# 第一个参数选实验矩阵(默认 e1)。"冷"组的构造是自适应的:serve 就绪后
# 起 ram_ballast 把 cgroup 配额里留给 page cache 的余量压到
# CACHE_ROOM_GIB(默认 4G, 小于 16 会话约 7G 的工作集)以下, 不需要预估
# serve 占多少内存(2026-07-12 摸底: 容器配额 120G, 宿主 free 显示 1TB
# 不可信, 判据必须对着 cgroup 记账算)。
#
# 产物(全部落在 results_e2e_<时间戳>/ 下):
#   SUMMARY.txt        每组的关键事件 + tier 日志行 + revisit/prime 比值, 先看这个
#   group_<组名>.json  bench 原始样本
#   serve_*.log bench_*.log  serve / 负载的完整输出
#   pidstat_* iostat_* iou_wrk_*  CPU 论点监控
#   meminfo_*          Cached/Dirty/MemAvailable 采样(E1 的 Cached 曲线、E2 的 Dirty 水位)
#   ballast_*.log      冷组压舱进程的输出
#
# AUTO_SHUTDOWN=1 bash bench/... 可在全部跑完后自动关机停计费(默认不关,
# 任何一组失败也不关, 留现场诊断)。

set -u

M="Qwen/Qwen2.5-7B-Instruct"
MAXLEN=16384
DATA=/root/autodl-tmp
PORT=8000
READY_TIMEOUT=3600      # 等 serve 就绪的上限(首组可能含模型下载)
BENCH_TIMEOUT=5400      # 单组负载上限
GPU_CLEAR_MB=1024       # 显存低于此值视为已清空
SHUTDOWN_TIMEOUT=30     # vLLM 自己的 shutdown_timeout, 默认 0 = 零等待 SIGKILL
                        # EngineCore(见 stop_serve 的泄漏归因), 非 0 才走 drain
                        # 模式给它时间跑完 shutdown() -> SharedOffloadRegion.cleanup()
AUTO_SHUTDOWN="${AUTO_SHUTDOWN:-0}"
EXPERIMENT="${1:-e1}"                   # 实验矩阵: e1 / legacy
CACHE_ROOM_GIB="${CACHE_ROOM_GIB:-4}"   # 冷组给 page cache 留的余量上限(GiB)

# HF 下载三件套(2026-07-10 组 A 踩坑: 模型没缓存, xet 后端直连 CAS 服务 401):
# 端点走国内镜像; 关 xet 退回普通 HTTP(镜像不支持 xet); 缓存放数据盘保系统盘
export HF_ENDPOINT="${HF_ENDPOINT:-https://hf-mirror.com}"
export HF_HUB_DISABLE_XET=1
export HF_HOME="${HF_HOME:-$DATA/hf}"

# uring 的磁盘容量上限。fs tier 没有对应配置项(无上限), 所以这个数同时是
# "对照是否干净"的判据: 负载的盘上唯一块总量一旦超过它, uring 就得 LRU 逐出
# 重写而 fs 不用, 两边不再是同一场比赛(E3 实测 2.9× 写放大的根因)。
#
# 2026-07-14 上调 20G→30G: 第一次跑 e4 时 20G 不够, 唯一块实测 28357MiB 撞上限,
# 三个 uring 组全被判 WARN(capacity)。教训是"每条 churn 往盘上留多少唯一块"
# 不是负载常数 —— 一条 churn 产出 ~488 块 KV, 能落盘多少取决于 store 路径吸收
# 得过来多少, E3 只吸收了 23%、E4 吸收了 75%(同样的请求节奏, 机制待查, 见
# COMPARE_PLAN E4 节的悬案)。所以容量不按"预期落盘量"给, 按**最坏情况上界**给:
# 上界 = prime 8000 块 + churn 条数 × 488 块, 全按 100% 落盘算, 与吸收率无关,
# 只由 prompt 大小和请求条数决定 —— 算得准, 也就兜得住。
URING_CAP_MIB=30720
URING_CAP_BYTES=$(( URING_CAP_MIB * 1024 * 1024 ))

CFG_B='{"kv_connector":"OffloadingConnector","kv_role":"kv_both","kv_connector_extra_config":{"spec_name":"CPUOffloadingSpec","cpu_bytes_to_use":4294967296}}'
CFG_FS='{"kv_connector":"OffloadingConnector","kv_role":"kv_both","kv_connector_extra_config":{"spec_name":"TieringOffloadingSpec","cpu_bytes_to_use":4294967296,"secondary_tiers":[{"type":"fs","root_dir":"'$DATA'/kv_fs"}]}}'
CFG_URING='{"kv_connector":"OffloadingConnector","kv_role":"kv_both","kv_connector_extra_config":{"spec_name":"TieringOffloadingSpec","cpu_bytes_to_use":4294967296,"secondary_tiers":[{"type":"uring","path":"'$DATA'/kv_tier.bin","disk_bytes_to_use":'$URING_CAP_BYTES',"queue_depth":32}]}}'

# ── 实验矩阵: 每行一个实验 ──────────────────────────────────────────────
# 字段: 组名|kv-transfer-config|期望tier日志|监控:0/1|额外env|冷组cache余量GiB|bench额外参数
# ("-" = 无;余量 0 = 热组不压舱)。加实验 = 加一行, run_group 骨架和监控全复用。
case "$EXPERIMENT" in
e1)
    # E1 工作集 vs page cache(COMPARE_PLAN E1): fs/uring × 热/冷 四次 serve。
    # 负载沿用默认串行 prime→churn→revisit;判据三层: revisit TTFT 对比 +
    # iostat 的 revisit 期间 r/s(fs 热组应约等于零)+ cgroup 的 cache 曲线
    # (fs 组上涨 = KV 在 CPU tier 和 page cache 存两份, uring 组应是平的)。
    SPECS=(
        "E1_fs_hot|$CFG_FS|fs|1|PYTHONHASHSEED=0|0|-"
        "E1_fs_cold|$CFG_FS|fs|1|PYTHONHASHSEED=0|$CACHE_ROOM_GIB|-"
        "E1_uring_hot|$CFG_URING|uring|1|-|0|-"
        "E1_uring_cold|$CFG_URING|uring|1|-|$CACHE_ROOM_GIB|-"
    )
    ;;
e2)
    # E2 store 剖析(COMPARE_PLAN E2 重启版): 纯 churn 循环, 把 load 从环境里
    # 整个拿掉。一次收三件: churn TTFT 税(fs/uring 的 churn 均值 − A 组无
    # offload 基线;E3 里 fs 系统性 +10ms 的悬案)、吸收率之谜(纯 churn 若
    # 落盘 ≈8 成 → 坐实"revisit 波打断下沉"; 掉回 2 成 → 文件数/时长衰减)、
    # store 侧 fs/uring 对比空白。观测三路: fs 的 kv_fs 文件账(收尾统计,
    # 既有), uring 的 <backing>.stats.json 账本(manager 10s 周期 dump:
    # 唯一块漏斗 + per-job queue/service 时戳, 2026-07-20 实装), iostat 写带宽。
    #
    # churn 压容量(E4 纪律, 零逐出才能写量对称): 上界 = prime 8000 +
    # 48×488 = 31424 块 ≈ 26.9G < 30G 上限(余 10%)。8 轮 × 6 条: 轮数多、
    # 每轮小, "落盘增量 vs 文件数"曲线的粒度才够判衰减形态; 文件数推到
    # ~3.1 万(超 E4 复跑无衰减的 2.6 万, 逼近 E3 低吸收的 4 万)。时长轴
    # (E3 的 30+ 分钟)在零逐出约束下盖不满, 如实记为本设计的边界。
    # A 组也跑同一负载: churn 不读盘、文本全新不吃 prefix cache, TTFT 由
    # GPU prefill 主导, 三组可比;它的 iostat 顺带证"无 offload 时零盘写"。
    E2_PHASES="prime"
    for _ in 1 2 3 4 5 6 7 8; do E2_PHASES+=",churn"; done
    E2_ARGS="--phases $E2_PHASES --churn 6"
    SPECS=(
        "E2_none_a|-|-|1|-|0|$E2_ARGS"
        "E2_fs_a|$CFG_FS|fs|1|PYTHONHASHSEED=0|0|$E2_ARGS"
        "E2_uring_a|$CFG_URING|uring|1|-|0|$E2_ARGS"
        "E2_none_b|-|-|1|-|0|$E2_ARGS"
        "E2_fs_b|$CFG_FS|fs|1|PYTHONHASHSEED=0|0|$E2_ARGS"
        "E2_uring_b|$CFG_URING|uring|1|-|0|$E2_ARGS"
        "E2_none_c|-|-|1|-|0|$E2_ARGS"
        "E2_fs_c|$CFG_FS|fs|1|PYTHONHASHSEED=0|0|$E2_ARGS"
        "E2_uring_c|$CFG_URING|uring|1|-|0|$E2_ARGS"
    )
    ;;
e3)
    # E3 并发 load 深度(COMPARE_PLAN E3): 同一 serve 内 prime 一次, 每个并发
    # 档 c 前重新 churn 把前缀挤回盘, 每档 3 轮取合并样本。不压舱 ——
    # 并发轴不和内存压力轴混跑。判据: revisit@c 的 p50/p99 随 c 的曲线;
    # 作废条件靠样本 t_wall 对 iostat 读流量事后核对
    E3_PHASES="prime"
    for c in 1 4 8 16; do
        for _ in 1 2 3; do E3_PHASES+=",churn,revisit@$c"; done
    done
    SPECS=(
        "E3_fs|$CFG_FS|fs|1|PYTHONHASHSEED=0|0|--phases $E3_PHASES"
        "E3_uring|$CFG_URING|uring|1|-|0|--phases $E3_PHASES"
    )
    ;;
e3dentry)
    # E3 归因封顶: dentry 预热对照(COMPARE_PLAN §55)。§14 微基准已证冷元数据
    # **足以**致碎(充分性), 这里补 e2e 内的干预(排他性): 同 E3 负载, warm 组
    # 每次 revisit 前把 kv_fs 全量 stat 扫热 —— O_DIRECT 下数据页不进 page
    # cache, 只动元数据温度一个变量。预测: warm 组 fs 设备请求尺寸回 ~773KiB、
    # 请求数骤降、对 uring 差距显著收窄; 不中则归因降级回"充分性+内插一致"。
    #
    # 不重跑完整 E3 矩阵: 档位压到 c=16 单档(分化最大, +39%)× 3 轮;
    # fs_warm/fs_ctrl 同机成对消跨 serve 漂移(E1 纪律), uring 各一组当同机
    # 差距锚点; b 轮把 warm/ctrl 顺序对调, 抵消单调 serve 漂移(E6 学的)。
    # churn 16 条/轮 × 3 轮 = 48 条: 上界 8000 + 48×488 = 31424 块 ≈ 26.9G
    # < 30G, uring 组零逐出; 挤出压力 16 条已由 E4 自验证够(满量 5443MB)。
    E3D_PHASES="prime,churn,revisit@16,churn,revisit@16,churn,revisit@16"
    E3D_CTRL="--phases $E3D_PHASES --churn 16"
    E3D_WARM="$E3D_CTRL --pre-revisit-stat-dir $DATA/kv_fs"
    SPECS=(
        "E3D_fs_warm_a|$CFG_FS|fs|1|PYTHONHASHSEED=0|0|$E3D_WARM"
        "E3D_fs_ctrl_a|$CFG_FS|fs|1|PYTHONHASHSEED=0|0|$E3D_CTRL"
        "E3D_uring_a|$CFG_URING|uring|1|-|0|$E3D_CTRL"
        "E3D_fs_ctrl_b|$CFG_FS|fs|1|PYTHONHASHSEED=0|0|$E3D_CTRL"
        "E3D_fs_warm_b|$CFG_FS|fs|1|PYTHONHASHSEED=0|0|$E3D_WARM"
        "E3D_uring_b|$CFG_URING|uring|1|-|0|$E3D_CTRL"
    )
    ;;
e4)
    # E4 读写混战(COMPARE_PLAN E4): mixed@8 —— churn 流持续闭环发压,
    # 每 25s 插一波 revisit(全部 16 会话 @ c=8, 与 E3 的 revisit@8 同形状),
    # 常驻 decode 探针逐 token 记时间戳(E5 的 ITL 证据)。判据: mixed_revisit@8
    # 相对 E3 revisit@8 "安静盘"基线(fs 369.3ms 均值 / uring 303.5ms)劣化多少。
    # 定档 c=8 的理由见 COMPARE_PLAN E4 节(保守留一档余量)。不压舱。
    #
    # 盘上唯一块 ≤ 容量上限是硬约束(见 COMPARE_PLAN E4 "干净对照"段): 唯一块
    # 一旦超过 uring 的 disk_bytes_to_use 就触发 LRU 逐出→重写, E3 那 2.9× 写放大
    # 会把"写流干扰读路径"的测量污染成"uring 自伤"。
    #
    # 预算按**最坏情况上界**给, 不按预期落盘量(2026-07-14 第一次跑 e4 的教训:
    # 按"每条 churn 留 ~98MiB"的预期算得 17.2G, 实测 28.4G, 三个 uring 组全废 ——
    # 落盘量取决于 store 路径的吸收率, 而吸收率不是常数, E3 是 23%、E4 是 75%)。
    # 上界只由 prompt 大小和请求条数决定, 与吸收率无关:
    #   上界(块) = prime 8000 + churn 条数 × 488     (一条 6000 词 churn ≈ 488 块)
    # 本配置: churn 16(独立) + mixed 约 43s÷1.28s ≈ 34 条 = 50 条
    #   ⇒ 上界 = 8000 + 50×488 = 32400 块 × 0.875MiB ≈ 28.4G < 30G 上限, 留 8% 余量
    #   ⇒ 盘占用峰值 = max(kv_fs 上界 28.4G, uring backing 30G) = 30G < 35G 可用 ✓
    # 上界兜得住"uring 比 fs 吸收得更多"的情况 —— 收尾的 kv_fs 落盘哨兵只量得到
    # fs 那一侧, 真正的保险是这个上界。
    #
    # churn 16 / interval 20s 是为了压住上界砍出来的, 代价是每轮的挤出压力从 E3 的
    # 24 条降到 ~16 条。够不够挤是自验的: 每波 revisit 的盘读量(t_wall × iostat)
    # 若两 tier 相当且量级可观(E3 同档是 5.32GB), 前缀就是真从盘上回来的;若明显
    # 偏小, 说明挤出不足、波打在了 GPU/CPU tier 上, 该轮作废。
    # 不要为了多拿样本加波数 —— 加样本走加 serve(下面 a/b/c), 不是延长 mixed。
    #
    # 安静盘基线放在同一个 serve 里(prime,churn,revisit@8 —— 与 E3 revisit@8
    # 同形状), 劣化 = mixed_revisit@8 − 本 serve 自己的 revisit@8。不去对 E3 那次
    # serve 的数(fs 369.3ms / uring 303.5ms): E1 立下的纪律是跨 serve 差异小于
    # 15~20ms 的邻居噪声底不许下结论, 而劣化幅度事先并不知道有没有那么大。
    # 已知残留混淆: mixed 期盘上文件数比基线期多(churn 流一直在写), fs 的元数据
    # 面随文件数增长, 所以 fs 的劣化里掺了一点"文件更多"而非纯"写流干扰"。E3 的
    # 每档 3 轮(轮间文件数递增)读数稳定, 说明这一项经验上很小, 记为已知缺口。
    #
    # a/b/c 交替三次 serve: 每 serve 基线 16 样本 + mixed 2 波 × 16 = 32 样本,
    # 合并后 48 基线 / 96 mixed 每 tier(≥ E3 每档 48);交替是为了跨 serve 环境
    # 漂移不偏向任一 tier(E1 的教训)
    E4_MIXED="--phases prime,churn,revisit@8,mixed@8 --churn 16 --mixed-waves 2 --mixed-interval 20"
    SPECS=(
        "E4_fs_a|$CFG_FS|fs|1|PYTHONHASHSEED=0|0|$E4_MIXED"
        "E4_uring_a|$CFG_URING|uring|1|-|0|$E4_MIXED"
        "E4_fs_b|$CFG_FS|fs|1|PYTHONHASHSEED=0|0|$E4_MIXED"
        "E4_uring_b|$CFG_URING|uring|1|-|0|$E4_MIXED"
        "E4_fs_c|$CFG_FS|fs|1|PYTHONHASHSEED=0|0|$E4_MIXED"
        "E4_uring_c|$CFG_URING|uring|1|-|0|$E4_MIXED"
    )
    ;;
e4phase)
    # E4 相位共振干预(COMPARE_PLAN §100): 波间隔 20s ≈ 16.0 × churn 周期
    # 1.24~1.25s ⇒ 相位在 serve 内被冻住, 抽中坏相位整个 serve 全坏(uring_a
    # 那次 +78%)。互质间隔 19.3s(≈15.44 周期)让相位逐波游走 —— 预测:
    # "全坏 serve"消失, 各 serve 的批间形态趋同(逐批散点核, e4_phase_audit.py);
    # 不中则相位冻结归因要回炉。20s 参考点同机再锚一次(用户定: 一个即可)。
    # fs 不陪跑: 其 +117% 劣化与 GPU 相位无关, 散点已证(§109)。
    # 容量与 e4 同一套预算: 上界 ≈ 28.4G < 30G, 零逐出。
    E4P_BASE="--phases prime,churn,revisit@8,mixed@8 --churn 16 --mixed-waves 2"
    SPECS=(
        "E4P_uring_193_a|$CFG_URING|uring|1|-|0|$E4P_BASE --mixed-interval 19.3"
        "E4P_uring_200_ref|$CFG_URING|uring|1|-|0|$E4P_BASE --mixed-interval 20"
        "E4P_uring_193_b|$CFG_URING|uring|1|-|0|$E4P_BASE --mixed-interval 19.3"
        "E4P_uring_193_c|$CFG_URING|uring|1|-|0|$E4P_BASE --mixed-interval 19.3"
    )
    ;;
e6)
    # E6 真实 trace 锚点(COMPARE_PLAN E6): 回放 Mooncake 生产 trace, 标定真实
    # 多轮负载落在 E1~E4 扫出来的哪个区间。跑 mooncake_replay.py 而非
    # long_context_ttft.py(run_group 按 $EXPERIMENT 选脚本)。
    #
    # 窗口 = toolagent_trace 前 150 条, 过滤 input_length>16384(真实 16k serve
    # 也会拒)。选 toolagent 而非 conversation 的理由(dry-run 标定, 2026-07-15):
    # conversation 装得下盘的窗口(~100 条)读/写只有 0.09 —— trace 开头全是冷首写,
    # load 路径被饿死;toolagent 每请求 block 密度低一半(4.06 vs 7.5), 且工具/
    # system 前缀复用前置, N=150 同时满足: 盘上界 27.6G < 36G 可用(留 8.6G)、
    # 读/写 0.91(读写均衡, load 路径真被打到)、并发估计 p99 16/max 17(正好落在
    # E3 的 c=16 档 —— uring 优势最大的 +39% 那一档)。conversation 降级为纯标定点
    # (全 trace 坐标 读/写 0.57、并发 p99 13), 它的"窗口读饿死"本身写进报告。
    #
    # 盘上界 27.6G < uring disk_bytes 30G ⇒ 零逐出, 干净对照(E4 纪律)。fs 无上限
    # 但 27.6G < 36G 可用, 装得下。block-words 480: 真 Qwen tokenizer 校准(400 词
    # =426 token), 480 词 ≈511 token/块 ≈ Mooncake 真实 512-token 块, 盘上界估算
    # 对齐。CPU tier 4G ≪ 27.6G 工作集 ⇒ 盘 tier 必被打到。output-cap 64: 主指标
    # 是 TTFT/prefill(METRICS.md), decode 封顶省时且 E5 已证 decode 不分化。
    #
    # 并发扫描(2026-07-16 定): 开环按 trace 原始到达率回放会把单张 4090 灌爆 ——
    # 150 条 ~5800-token 请求在 33s 内涌入, GPU 一步嚼不下, 攒成 150 深的调度队列,
    # 实测 TTFT 19s 全是排队, tier 的 load 路径被彻底淹没, fs≈uring(还被 serve
    # 单调漂移伪装成"fs 恒赢", 详见 git 历史那次废数据的归因)。根子是真实到达率
    # 是一个集群的, 单卡跑不动 —— 这本身是 E6 的一条边界结论。
    #
    # 修法: 用 --max-inflight 把在飞并发压到 GPU 追得上的水位, 让 TTFT 量到的是
    # tier 而非过载队列。但"哪个 c 才在饱和悬崖以下"不拍脑袋 —— 直接扫 c ∈
    # {1,4,8,16}(= E3 的档), 画 TTFT-vs-c 曲线自己暴露拐点, 且能直接叠在 E3 的
    # 合成曲线上, 顺带把"真实 toolagent 请求内容能否复现 E3 的 tier 分化"验掉。
    # E3 已在同机测过 c≤16 稳定(revisit@8 749/559ms、@16 ~750ms, 非雪崩), 所以
    # 悬崖预期在 16 之上; 扫描 + driver 的实测并发/ttft 爬升自检当场证实。
    #
    # 每个 (c) 一组新 serve: 同一 serve 内重放同一 trace 会命中已驻留的块(cache
    # 污染, E3 那次靠 churn 挤回盘解决), 而 E6 没有 churn 相, 只能靠冷 serve 保证
    # 每档都是冷→load。窗口/映射沿用前案(toolagent 150 条、过滤 16384、block-words
    # 480、盘上界 27.6G < 36G 可用、CPU tier 4G ≪ 工作集 ⇒ 盘 tier 必被打到)。
    # 每档两复现, 且第二复现把 tier 顺序对调(fs先→uring先), 抵消单调 serve 漂移
    # (上一次 fs 恒排每对之首 + 上漂 = 假"fs 赢", 这次结构上消掉)。
    E6_TRACE="traces/toolagent_trace.jsonl"
    E6_BASE="--trace $E6_TRACE --max-input-tokens 16384 --max-requests 150 --block-words 480 --output-cap 64"
    SPECS=()
    for c in 1 4 8 16; do
        a="$E6_BASE --max-inflight $c"
        # 复现 a: fs 先; 复现 b: uring 先 —— 每档内 fs/uring 各当一次"队首"
        SPECS+=("E6_c${c}_fs_a|$CFG_FS|fs|1|PYTHONHASHSEED=0|0|$a")
        SPECS+=("E6_c${c}_uring_a|$CFG_URING|uring|1|-|0|$a")
        SPECS+=("E6_c${c}_uring_b|$CFG_URING|uring|1|-|0|$a")
        SPECS+=("E6_c${c}_fs_b|$CFG_FS|fs|1|PYTHONHASHSEED=0|0|$a")
    done
    ;;
gil)
    # churn +10ms 悬案的证伪探针(COMPARE_PLAN E3 冻结段): 假说是 fs 的 store
    # 任务在 EngineCore 进程里以 Python 线程跑 open/write/replace, 每块 IO
    # 前后的字节码持 GIL, 和引擎调度热循环抢锁(project4 微基准的"GIL 税≈0"
    # 是独立进程, 不覆盖这种同进程共锁)。把 store 线程 16→2, 抢锁者少 8 倍:
    # churn TTFT 若 -10ms 同向收窄 → GIL/派发线程数坐实; 不动 → 悬案继续。
    # 效应量(~10ms)低于跨 serve 噪声底(15-20ms), 单组对比不算数 ——
    # w16/w2 交替各两个 serve, 判据用批间同向性(26 批全同向的老办法),
    # 不看均值差。w2 的写带宽不构成混淆: 2 线程 O_DIRECT 顺序写 ≫ churn
    # 期实测 ~85MB/s 的 store 流量。
    #
    # 2026-07-20 改负载: 原稿沿用 E3 全谱(churn 24 × 12 轮), 而那正是把盘
    # 写满、撞 77884 次 ENOSPC 的那套负载 —— 照跑数据全废。换成 E2 的纯 churn
    # (48 条, 实测落盘 21.3G < 35G 盘), 同时消掉 revisit 这个无关变量: 要测的
    # 是 store 路径对 prefill 的干扰, load 在场只添噪。基线组 none 必须同批跑,
    # 因为"税"的定义就是相对无 offload 的差(E2 已标定: fs +7.7ms、uring +1.3ms)。
    #
    # 预期与判读: GIL 争抢的代价随线程数超线性(convoy), 16→2 若把 +7.7ms
    # 明显压下去 ⇒ GIL 坐实; 若纹丝不动, 说明代价正比于 Python 总工作量而非
    # 并发争抢, 换候选(元数据事务 / 派发路径本身)。
    # 自检: w2 的 store 更慢, kv_fs 落盘量应当低于 w16 的 24889 文件;
    # 两组若一字不差, 说明 n_write_threads 这个配置键没生效, 本轮作废。
    GIL_PHASES="prime"
    for _ in 1 2 3 4 5 6 7 8; do GIL_PHASES+=",churn"; done
    GIL_ARGS="--phases $GIL_PHASES --churn 6"
    CFG_FS_W2="${CFG_FS/\"root_dir\"/\"n_write_threads\":2,\"root_dir\"}"
    SPECS=(
        "GIL_none_a|-|-|1|-|0|$GIL_ARGS"
        "GIL_w16_a|$CFG_FS|fs|1|PYTHONHASHSEED=0|0|$GIL_ARGS"
        "GIL_w2_a|$CFG_FS_W2|fs|1|PYTHONHASHSEED=0|0|$GIL_ARGS"
        "GIL_none_b|-|-|1|-|0|$GIL_ARGS"
        "GIL_w2_b|$CFG_FS_W2|fs|1|PYTHONHASHSEED=0|0|$GIL_ARGS"
        "GIL_w16_b|$CFG_FS|fs|1|PYTHONHASHSEED=0|0|$GIL_ARGS"
        "GIL_none_c|-|-|1|-|0|$GIL_ARGS"
        "GIL_w16_c|$CFG_FS|fs|1|PYTHONHASHSEED=0|0|$GIL_ARGS"
        "GIL_w2_c|$CFG_FS_W2|fs|1|PYTHONHASHSEED=0|0|$GIL_ARGS"
    )
    ;;
legacy)
    # 旧 A/B/C1/C2 四组(docs/RUN_ON_GPU.md 阶段 2), 保留可复跑
    SPECS=(
        "A|-|-|0|-|0|-"
        "B|$CFG_B|-|0|-|0|-"
        "C1|$CFG_FS|fs|1|PYTHONHASHSEED=0|0|-"
        "C2|$CFG_URING|uring|1|-|0|-"
    )
    ;;
*)
    echo "未知实验矩阵 '$EXPERIMENT'(可选: e1 / e2 / e3 / e3dentry / e4 / e4phase / e6 / gil / legacy)" >&2
    exit 1
    ;;
esac

cd "$(dirname "$0")/.."
RES="results_e2e_$(date +%Y%m%d_%H%M)"
mkdir -p "$RES"
SUMMARY="$RES/SUMMARY.txt"

log() { echo "[$(date '+%F %T')] $*" | tee -a "$SUMMARY"; }

SERVE_PID=""
MON_PIDS=()
BALLAST_PID=""

cleanup() {
    # 脚本被杀 / 出错退出时别留孤儿: 掐掉监控、压舱和 serve 进程组
    ((${#MON_PIDS[@]})) && kill "${MON_PIDS[@]}" 2>/dev/null
    [ -n "$BALLAST_PID" ] && kill "$BALLAST_PID" 2>/dev/null
    # 2026-07-20: 原版只发一个 TERM 就退出, 两个后果都踩到了 ——
    # (a) setsid 起的 serve 脱离进程组, TERM 之后还要走完 30s drain, 脚本
    #     早退出了它还活着, 下次预检直接被"已有 vllm serve 在跑"挡回来;
    # (b) 删 kv_fs / backing 写在 run_group 的组收尾里, 组没跑完根本轮不到,
    #     中断一次就把 35G 留在盘上(本次实测: 盘只剩 1G)。
    # 所以中断路径必须自己接管清理义务 —— 同 /dev/shm 泄漏那次的教训:
    # 用信号兜底的脚本, 得替被杀的进程把手尾做完。
    if [ -n "$SERVE_PID" ]; then
        kill -TERM -- -"$SERVE_PID" 2>/dev/null
        for _ in $(seq 1 "${SHUTDOWN_TIMEOUT:-30}"); do
            kill -0 -- -"$SERVE_PID" 2>/dev/null || break
            sleep 1
        done
        kill -9 -- -"$SERVE_PID" 2>/dev/null
    fi
    # 盘和 tmpfs 的残留一律清掉。数据本身在 $RES 里, 这些只是 tier 的工作区,
    # 留着既占盘又污染下一轮的缓存状态。
    rm -rf "$DATA/kv_fs" 2>/dev/null
    rm -f "$DATA/kv_tier.bin" "$DATA/kv_tier.bin.stats.json.tmp" \
          "$DATA/kv_tier.bin.records.jsonl" 2>/dev/null
    rm -f /dev/shm/vllm_offload_*.mmap 2>/dev/null
}
trap cleanup EXIT

gpu_used_mb() {
    nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits \
        2>/dev/null | head -1 | tr -d ' '
}

# ── 就绪判定: /v1/models 通了就算, 日志行只做兜底 ──────────────────────
wait_ready() {
    local logf=$1 deadline=$(( $(date +%s) + READY_TIMEOUT ))
    while (( $(date +%s) < deadline )); do
        curl -sf "http://localhost:$PORT/v1/models" >/dev/null 2>&1 && return 0
        grep -q "Application startup complete" "$logf" 2>/dev/null && return 0
        kill -0 "$SERVE_PID" 2>/dev/null || {
            log "  serve 进程提前退出, 尾部日志:"
            tail -5 "$logf" | tee -a "$SUMMARY"
            return 1
        }
        sleep 10
    done
    log "  等就绪超时 ${READY_TIMEOUT}s"
    return 1
}

stop_serve() {
    kill -INT -- -"$SERVE_PID" 2>/dev/null
    local i
    for i in $(seq 1 30); do
        kill -0 "$SERVE_PID" 2>/dev/null || break
        sleep 2
    done
    kill -0 "$SERVE_PID" 2>/dev/null && { kill -TERM -- -"$SERVE_PID" 2>/dev/null; sleep 10; }
    # 兜底 SIGKILL 只在优雅关停真的失败时开枪, 并且留痕。原来这两行是无守卫无日志
    # 地照杀, 于是"优雅关停成功"和"失败"混成一团, 下面那桩泄漏因此归因错了两天。
    if pgrep -f "vllm serve|EngineCore" >/dev/null 2>&1; then
        log "  !! 优雅关停失败, SIGKILL 兜底(shm 会残留, 靠下面的 rm 清)"
        pkill -9 -f "vllm serve" 2>/dev/null
        pkill -9 -f "EngineCore" 2>/dev/null
    fi
    SERVE_PID=""
    # /dev/shm 泄漏归因(2026-07-14 查清, 修正旧注释): 肇事者不是上面的 pkill。
    # vLLM 的 shutdown_timeout 默认 0(config/vllm.py:380) → process manager 对
    # EngineCore 先 terminate 再零等待 SIGKILL(v1/utils.py:621 那个 join 循环
    # deadline=now, 第一轮就 break) → TieringOffloadingManager.shutdown() →
    # SharedOffloadRegion.cleanup() 里的 os.unlink 永远跑不到 → 每次 serve 在
    # tmpfs 上留一具 4.3G 尸体(= cpu_bytes_to_use 的 CPU tier 实体), 攒满 /dev/shm
    # 后下一组的 MADV_POPULATE_WRITE 分不到页, 吃 EFAULT 死在引擎初始化
    # (2026-07-12 E3_uring 踩坑, 攒到第 5 次 serve 才炸)。
    # 根治 = serve 时传 --shutdown-timeout(见 SHUTDOWN_TIMEOUT); 这行 rm 保留当
    # 兜底: OOM killer / 真挂死之下 SIGKILL 路径永远可能发生, 用 SIGKILL 兜底的
    # 脚本必须接管被杀进程的清理义务。
    rm -f /dev/shm/vllm_offload_*.mmap
    for i in $(seq 1 60); do
        local used; used=$(gpu_used_mb)
        [ -n "$used" ] && (( used < GPU_CLEAR_MB )) && {
            log "  显存已清空 (${used} MiB)"
            return 0
        }
        sleep 5
    done
    log "  !! 显存 5 分钟没降到 ${GPU_CLEAR_MB} MiB 以下 (当前 ${used:-?} MiB)"
    return 1
}

# ── 监控(tier 组): serve 全家的 CPU + 盘 + iou-wrk 线程数 + 内存 ───────
start_monitors() {
    local group=$1 pids
    # 注意别用裸 "vllm" 匹配: 项目路径里就带 vllm 字样, 会把本脚本自己算进去
    pids=$(pgrep -d, -f "vllm serve|EngineCore") || pids=""
    if command -v pidstat >/dev/null && [ -n "$pids" ]; then
        # -t 拆到线程级: E5 要对比 fs 的 32 条池线程 vs uring 的单提交线程
        # + io-wq worker(PF_IO_WORKER 是进程的线程, -t 才看得见)
        pidstat -u -t -p "$pids" 5 > "$RES/pidstat_$group.log" 2>&1 &
        MON_PIDS+=($!)
    fi
    if command -v iostat >/dev/null; then
        # -t 给每次采样打时间戳, 要拿它和 bench 的 phase 时刻对齐。
        # 1 秒粒度(原 5 秒): E3 复盘两次栽在 5 秒稀释上——高并发 revisit
        # 窗口只有 1~3 秒, 5 秒均值把突发峰值稀释一半, 差点把 1089MB/s
        # 误判成设备封顶;fs 慢的那截在每请求流水线延迟里, 5 秒粒度看不见
        iostat -x -t 1 > "$RES/iostat_$group.log" 2>&1 &
        MON_PIDS+=($!)
    fi
    # iou-wrk 是内核线程, 全系统数一遍即可(独占机器, 没别的 uring 用户)
    ( while sleep 1; do
          echo "$(date +%T) $(ps -eLo comm= 2>/dev/null | grep -c '^iou-wrk')"
      done > "$RES/iou_wrk_$group.log" ) &
    MON_PIDS+=($!)
    # 内存采样, 5 秒一采: cache 是 E1 的第三层判据(fs 组一路涨 = 同一份
    # KV 在 CPU tier 和 page cache 存两份), dirty 是 E2 的 writeback 归因,
    # anon 验证冷组压舱全程没漏气。共享宿主上 /proc/meminfo 混着别的租户,
    # 主数据看本容器 cgroup 的记账(page cache 谁读记谁账), meminfo 只留
    # 一列宿主侧参考
    # 解读要点(E1 复盘): 真正的 page cache = file − shmem。ballast 的
    # mmap(-1) 是 MAP_SHARED, 记进 shmem;file 把 shmem 也包含在内,
    # 所以单看 file 会把压舱误读成缓存暴涨。这里直接算好 pagecache 列
    ( CG=/sys/fs/cgroup/memory/memory.stat
      PAT='^total_(cache|rss|shmem|dirty|writeback)$'; FKEY=total_cache; SKEY=total_shmem
      if [ ! -f "$CG" ]; then
          CG=/sys/fs/cgroup/memory.stat
          PAT='^(anon|file|shmem|file_dirty|file_writeback)$'; FKEY=file; SKEY=shmem
      fi
      while sleep 5; do
          echo "$(date +%T) $(awk -v pat="$PAT" -v fk="$FKEY" -v sk="$SKEY" \
              '$1 ~ pat {printf "%s=%dMB ", $1, $2/1048576}
               $1 == fk {f=$2} $1 == sk {s=$2}
               END {printf "pagecache=%dMB ", (f-s)/1048576}' "$CG") | host: $(awk \
              '$1 ~ /^(MemAvailable|Cached|Dirty):$/ {printf "%s%dMB ", $1, $2/1024}' /proc/meminfo)"
      done > "$RES/meminfo_$group.log" ) &
    MON_PIDS+=($!)
}

stop_monitors() {
    ((${#MON_PIDS[@]})) && kill "${MON_PIDS[@]}" 2>/dev/null
    MON_PIDS=()
}

# ── 冷组压舱: 起 ballast 占住内存, 等到 READY 才算生效 ─────────────────
# 自适应模式: 填到 cgroup 配额里留给 page cache 的余量 ≤ room_gib。
# 要填 ~100G 且逼着内核一路回收 cache, 慢是预期的, 上限给到 10 分钟
start_ballast() {
    local room_gib=$1 group=$2 blog="$RES/ballast_$group.log"
    python3 bench/ram_ballast.py --cache-room-gib "$room_gib" > "$blog" 2>&1 &
    BALLAST_PID=$!
    local i
    for i in $(seq 1 600); do
        if grep -q "BALLAST READY" "$blog" 2>/dev/null; then
            log "  $(grep 'BALLAST READY' "$blog")"
            return 0
        fi
        kill -0 "$BALLAST_PID" 2>/dev/null || break
        sleep 1
    done
    log "  !! ballast 没就绪(死了或 600s 超时), 日志尾部:"
    tail -3 "$blog" 2>/dev/null | tee -a "$SUMMARY"
    return 1
}

stop_ballast() {
    [ -n "$BALLAST_PID" ] && kill "$BALLAST_PID" 2>/dev/null
    BALLAST_PID=""
}

# ── 一组的完整节奏: 起 serve → 等就绪 → 验 tier → 压舱 → 负载 → 收尾 ──
# run_group <组名> <kv-transfer-config|-> <期望tier日志|-> <monitor:0/1> <env|-> <冷组cache余量GiB> <bench额外参数|->
run_group() {
    local group=$1 cfg=$2 expect=$3 mon=$4 env0=$5 ballast=$6 bench_args=$7
    local slog="$RES/serve_$group.log"

    log "════ 组 $group 开始 (显存基线 $(gpu_used_mb) MiB) ════"

    local cmd=()
    [ "$env0" != "-" ] && cmd+=(env "$env0")
    cmd+=(vllm serve "$M" --max-model-len "$MAXLEN" --port "$PORT"
          --shutdown-timeout "$SHUTDOWN_TIMEOUT")
    [ "$cfg" != "-" ] && cmd+=(--kv-transfer-config "$cfg")

    # 上一组 uring 账本的残留(fail 路径没收走的)先挪走, 别被本组 serve 覆盖
    if [ -f "$DATA/kv_tier.bin.stats.json" ]; then
        mv "$DATA/kv_tier.bin.stats.json" "$RES/tier_stats_${group}_stale.json"
        log "  !! 发现上一组未收走的 uring 账本, 存为 tier_stats_${group}_stale.json"
    fi
    # 流水是 append 打开的, 残留不挪走就会被本组续写, 两组数据混成一份且
    # 无从分辨 —— 覆盖写没有这个问题, 是换成 append 之后新欠下的债, 必须还
    if [ -f "$DATA/kv_tier.bin.records.jsonl" ]; then
        mv "$DATA/kv_tier.bin.records.jsonl" \
           "$RES/tier_stats_${group}_stale.records.jsonl"
        log "  !! 发现上一组未收走的 uring 流水, 存为 tier_stats_${group}_stale.records.jsonl"
    fi

    setsid "${cmd[@]}" > "$slog" 2>&1 &
    SERVE_PID=$!
    log "  serve 已启动 pid=$SERVE_PID, 等就绪..."

    if ! wait_ready "$slog"; then
        log "  组 $group 失败: serve 没起来, 跳过"
        GROUP_STATUS[$group]="FAIL(serve)"
        stop_serve || return 2
        return 1
    fi
    log "  serve 就绪"

    # tier 日志验证: C 组没建出期望的 tier 就没有对比意义, 记警报但照跑
    if [ "$expect" != "-" ]; then
        local tline
        tline=$(grep -h "Created secondary tier" "$slog" | head -3)
        if echo "$tline" | grep -q "($expect)"; then
            log "  tier 确认: $tline"
        else
            log "  !! 没看到 'Created secondary tier ... ($expect)', 实际: ${tline:-<无>}"
            GROUP_STATUS[$group]="WARN(tier)"
        fi
        grep -in "O_DIRECT\|fall.*back\|degrad" "$slog" | head -5 | \
            while IFS= read -r l; do log "  serve 日志警报: $l"; done
    fi

    # 压舱放在 serve 就绪之后、负载之前: 自适应填充是拿 cgroup 配额减去
    # 已就位的匿名内存(serve 的 CPU tier pinned 等)算余量, serve 没起来
    # 之前算不准;也避免和模型加载抢内存。压不住就跳过这组 ——
    # "假冷"数据混进结论比缺一组更糟
    if [ "$ballast" != 0 ]; then
        if ! start_ballast "$ballast" "$group"; then
            GROUP_STATUS[$group]="FAIL(ballast)"
            stop_ballast
            stop_serve || return 2
            return 1
        fi
    fi

    [ "$mon" = 1 ] && start_monitors "$group"
    sleep 5

    log "  跑负载..."
    local extra=()
    [ "$bench_args" != "-" ] && read -ra extra <<< "$bench_args"
    # E6 回放真实 trace, 换驱动脚本; 其余矩阵都是合成负载走 long_context_ttft
    local bench_script="bench/long_context_ttft.py"
    [ "$EXPERIMENT" = e6 ] && bench_script="bench/mooncake_replay.py"
    timeout "$BENCH_TIMEOUT" python3 "$bench_script" \
        --model "$M" --json "$RES/group_$group.json" "${extra[@]}" \
        > "$RES/bench_$group.log" 2>&1
    local rc=$?

    [ "$mon" = 1 ] && stop_monitors
    stop_ballast

    if [ $rc -eq 0 ]; then
        # 把 phase 汇总和比值直接抄进 SUMMARY, 明早一眼能看
        # (revisit@N 带并发后缀, 模式不能再要求 revisit 后面紧跟空格;
        #  mixed 阶段的行是 " wave N" / mixed_revisit@N / mixed_churn / probe ITL)
        local sumpat='^(prime|churn|revisit|mixed|probe| wave)|/prime TTFT ratio'
        # E6 是 mooncake_replay 的输出: 预检标定行(请求数/读写比/并发/盘上界) +
        # TTFT 结果(all revisit / 前缀命中·未命中) + 超长丢弃/失败/警告
        [ "$EXPERIMENT" = e6 ] && sumpat='超长丢弃|请求数|读/写|到达过程|KV 落盘|all revisit|前缀(命中|未命中)|失败请求|实测在飞并发|ttft 前半|^注:'
        grep -E "$sumpat" "$RES/bench_$group.log" | \
            while IFS= read -r l; do log "  $l"; done
        [ -z "${GROUP_STATUS[$group]:-}" ] && GROUP_STATUS[$group]="OK"
    else
        log "  组 $group 负载失败 rc=$rc, bench 日志尾部:"
        tail -5 "$RES/bench_$group.log" | tee -a "$SUMMARY"
        GROUP_STATUS[$group]="FAIL(bench rc=$rc)"
    fi

    # tier 落盘统计 + 清理(原"C1 跑完删 kv_fs"的一般化): fs tier 没有容量
    # 上限配置, 35G 的盘必须盯着, 删之前先把占用记进 SUMMARY 当数据;
    # 删文件顺带把它的 page cache 一起释放, 组间缓存状态互相隔离
    if [ "$expect" = fs ] && [ -d "$DATA/kv_fs" ]; then
        local nfiles mib
        nfiles=$(find "$DATA/kv_fs" -type f | wc -l | tr -d ' ')
        mib=$(du -sm "$DATA/kv_fs" | cut -f1)
        log "  kv_fs 落盘: ${nfiles} 个文件, ${mib} MiB, 删掉腾磁盘"
        # fs 没有容量上限, 它的落盘量 = 本轮负载的盘上唯一块总量(按内容哈希天然
        # 去重), 而 uring 组跑的是同一批 prompt(--seed 固定) ⇒ 唯一块总量相同。
        # 所以这个数就是"uring 会不会撞上限"的先行指标: 超过 URING_CAP_MIB 就说明
        # uring 组必然触发 LRU 逐出→重写, 对照不干净(E3 那 2.9× 写放大的根因)。
        # E4 靠"唯一块 ≤ 20G"换干净对照, 这条越界即作废, 必须缩短 mixed 后重跑。
        if (( mib > URING_CAP_MIB )); then
            log "  !! 唯一块 ${mib} MiB > uring 容量 ${URING_CAP_MIB} MiB —— uring 组会逐出重写, 对照不干净"
            GROUP_STATUS[$group]="WARN(capacity)"
        fi
        rm -rf "$DATA/kv_fs"
    elif [ "$expect" = uring ]; then
        rm -f "$DATA/kv_tier.bin"   # 下一组 serve 会重新 fallocate + prewarm
    fi

    # 盘满哨兵(2026-07-20 补, E3 复盘教训): fs tier 没有容量上限, 负载一超盘
    # 就 ENOSPC, 而它**不报警也不停** —— 每块 store 失败一次、日志刷一行, 实验
    # 照跑完、状态照 OK。E3 两轮的 fs 组各撞了 77884 次, 从 c=4 档起全程满盘,
    # 直到今天才发现(吸收率 23% 的真身就是盘的物理上限, 不是 store 路径衰减)。
    # 满盘不只是丢块: XFS 接近满时分配器劣化, 且失败路径本身在刷元数据操作,
    # 两者都直接砸在被测的 IO 路径上 ⇒ 见到就作废, 不许当"轻微异常"。
    local nospc
    nospc=$(grep -icE "No space left on device" "$slog" 2>/dev/null || echo 0)
    if [ "${nospc:-0}" != 0 ]; then
        log "  !! 盘满: serve 日志 ${nospc} 次 ENOSPC —— 本组数据作废, 缩小负载重跑"
        GROUP_STATUS[$group]="FAIL(ENOSPC×${nospc})"
    fi

    if ! stop_serve; then
        log "  !! 显存清不掉, 中止剩余组, 免得后面全是 OOM 废数据"
        GROUP_STATUS[$group]="${GROUP_STATUS[$group]} GPU_STUCK"
        return 2
    fi

    # uring 账本收进结果目录, 两个文件:
    #   .stats.json        counters/gauges 快照 + 记录条数(10s 周期覆盖写)
    #   .records.jsonl     per-job 时戳流水(每次收割即 append)
    # 分成两个文件是 2026-07-20 E3D 丢 r3 之后改的:原来记录内嵌在 stats.json
    # 里全量覆盖写, dump 的 10s 周期和 revisit 的 21s 一轮撞相位, 第三轮必然
    # 落在最后一次 dump 之后, 而 shutdown 的终写在 SIGINT 关停下并没有跑 ——
    # 一整轮实验数据就这么没了, 两组一模一样。现在记录不依赖任何"最后的
    # 机会", 条数对不上时下面会报, 分析脚本也会自己拦。
    if [ "$expect" = uring ]; then
        if [ -f "$DATA/kv_tier.bin.stats.json" ]; then
            mv "$DATA/kv_tier.bin.stats.json" "$RES/tier_stats_$group.json"
            log "  uring 账本已收: tier_stats_$group.json"
        else
            log "  !! uring 账本缺失(.stats.json 没出现) —— manager dump 没跑?"
            GROUP_STATUS[$group]="${GROUP_STATUS[$group]:-OK} WARN(stats)"
        fi
        if [ -f "$DATA/kv_tier.bin.records.jsonl" ]; then
            mv "$DATA/kv_tier.bin.records.jsonl" \
               "$RES/tier_stats_$group.records.jsonl"
            log "  uring 流水已收: $(wc -l < "$RES/tier_stats_$group.records.jsonl") 条"
        else
            log "  !! uring 流水缺失(.records.jsonl 没出现) —— 窗口审计做不了"
            GROUP_STATUS[$group]="${GROUP_STATUS[$group]:-OK} WARN(records)"
        fi
        rm -f "$DATA/kv_tier.bin.stats.json.tmp"
    fi
    return 0
}

# ── 起跑前体检: 缺东西现在就死, 别浪费一晚 ─────────────────────────────
declare -A GROUP_STATUS
log "预检..."
fail=0
command -v nvidia-smi >/dev/null || { log "缺 nvidia-smi"; fail=1; }
command -v vllm       >/dev/null || { log "缺 vllm"; fail=1; }
python3 -c "import openai" 2>/dev/null || { log "缺 python openai 包"; fail=1; }
# uring 组的两个启动期死因都在这里提前暴露(2026-07-10 各踩一次):
#   .so 没编(新机器忘了 make) / manager 的 vLLM 接口合同变了(import 即炸)
python3 -c "from kv_uring_tier import _kvtier; import kv_uring_tier.manager" \
    || { log "uring 依赖检查失败: 先 make 编 .so;还不行就看上面 import 报错(vLLM 接口合同可能变了)"; fail=1; }
[ -f bench/long_context_ttft.py ]    || { log "找不到 bench/long_context_ttft.py (要在项目根跑)"; fail=1; }
[ -f bench/ram_ballast.py ]          || { log "找不到 bench/ram_ballast.py"; fail=1; }
# E6 额外要 mooncake_replay.py 和 trace 文件 —— trace 没下会跑到一半才炸
if [ "$EXPERIMENT" = e6 ]; then
    [ -f bench/mooncake_replay.py ] || { log "找不到 bench/mooncake_replay.py (E6 要它回放)"; fail=1; }
    [ -f "$E6_TRACE" ] || { log "找不到 E6 trace 文件 '$E6_TRACE' (先下 toolagent_trace.jsonl 到 traces/)"; fail=1; }
fi
[ -d "$DATA" ]                       || { log "数据盘 $DATA 不存在"; fail=1; }
pgrep -f "vllm serve" >/dev/null && { log "已有 vllm serve 在跑, 先清掉再来"; fail=1; }
# 上一轮(或上一次脚本)被 -9 杀掉留下的 shm 存货, 起跑前清一次
# (上面已确认没有 vllm serve 在跑, 删了不影响活进程)
(( fail )) || rm -f /dev/shm/vllm_offload_*.mmap
command -v pidstat >/dev/null || log "警告: 没有 pidstat(sysstat), CPU 监控会缺失, 不阻塞"
# 盘够不够: uring 的 backing 文件按 URING_CAP_MIB 全量 fallocate + prewarm, fs 目录
# 无上限但组间即删, 所以峰值 ≈ 上限本身。留 3G 余量给日志和文件系统元数据。
avail_gb=$(df -BG --output=avail "$DATA" 2>/dev/null | tail -1 | tr -dc 0-9)
cap_gb=$(( URING_CAP_MIB / 1024 ))
if [ -n "$avail_gb" ] && (( avail_gb < cap_gb + 3 )); then
    log "警告: $DATA 只剩 ${avail_gb}G, uring backing 要 ${cap_gb}G(+fs 目录同量级), 可能不够"
fi
(( fail )) && { log "预检失败, 退出"; exit 1; }
# 内存基线看 cgroup 不看 free: 共享宿主上 free 显示的是宿主 1TB, 没意义
if [ -f /sys/fs/cgroup/memory/memory.limit_in_bytes ]; then
    log "cgroup 内存基线: limit=$(( $(cat /sys/fs/cgroup/memory/memory.limit_in_bytes) >> 30 ))G current=$(( $(cat /sys/fs/cgroup/memory/memory.usage_in_bytes) >> 30 ))G"
elif [ -f /sys/fs/cgroup/memory.max ]; then
    log "cgroup 内存基线: limit=$(cat /sys/fs/cgroup/memory.max) current=$(( $(cat /sys/fs/cgroup/memory.current) >> 30 ))G"
fi
# CPU 基线同理, 也不能信 nproc/lscpu: 容器里它们穿透看宿主(实测宿主 128 核),
# 真正能用的是 cgroup 配额(实测 16 核)。差 8 倍, 解读任何 cpu_util 都要先有
# 正确的分母 —— "顶满配额"和"随便用用"在绝对核数上长得一模一样, 只有对上
# 配额才分得开。此前七轮 e2e 没有一份 RESULTS 记过这个数(2026-07-20 补)。
cpu_host=$(grep -c ^processor /proc/cpuinfo)
cpu_quota=未知
if [ -f /sys/fs/cgroup/cpu.max ]; then                          # cgroup v2
    read -r quota_us period_us < /sys/fs/cgroup/cpu.max
    if [ "$quota_us" = max ]; then cpu_quota=无限
    else cpu_quota=$(awk "BEGIN{printf \"%.1f\", $quota_us/$period_us}"); fi
elif [ -f /sys/fs/cgroup/cpu/cpu.cfs_quota_us ]; then           # cgroup v1
    quota_us=$(cat /sys/fs/cgroup/cpu/cpu.cfs_quota_us)
    period_us=$(cat /sys/fs/cgroup/cpu/cpu.cfs_period_us)
    if (( quota_us < 0 )); then cpu_quota=无限
    else cpu_quota=$(awk "BEGIN{printf \"%.1f\", $quota_us/$period_us}"); fi
fi
# affinity 要单独记: 配额是 CFS 时间片掐出来的, 不是 cpuset 圈出来的 ——
# 实测 allowed 是 0-127 而配额只有 16 核, 线程能在 128 个核上飘, 跨 socket
# 访存是 IO 抖动的一个来源, 和限流是两回事, 别混着归因。
log "CPU 基线: 宿主 ${cpu_host} 核, cgroup 配额 ${cpu_quota} 核, affinity=$(awk '/Cpus_allowed_list/{print $2}' /proc/self/status), OMP_NUM_THREADS=${OMP_NUM_THREADS:-未设}"
if [ "$cpu_quota" != 无限 ] && [ "$cpu_quota" != 未知 ]; then
    # 并发组的客户端线程 + vLLM 调度 + io-wq worker 全挤在这点配额里。并发数
    # 逼近配额时 CPU 先饱和, 测出来的组间差可能是抢 CPU 而不是 IO 路径的差。
    log "  提醒: 并发数接近 ${cpu_quota} 时 CPU 先于盘饱和, 该组 TTFT 归因要先排除 CPU"
fi
# 限流快照: cpu.stat 是容器本次开机以来的累计值, 关机重开清零, 所以只有
# 起跑前/收尾各采一次做差才有意义。注意"顶满配额"和"被 CFS 掐住"是两件事,
# throttled_usec 是后者唯一的直接读数, nr_throttled 只数次数不给时长。
CPUSTAT=/sys/fs/cgroup/cpu.stat
[ -f "$CPUSTAT" ] || CPUSTAT=/sys/fs/cgroup/cpu/cpu.stat
if [ -f "$CPUSTAT" ]; then
    cp "$CPUSTAT" "$RES/cpu_stat_before.txt"
    log "限流基线: $(awk '/nr_throttled|throttled_usec/{printf "%s=%s ", $1, $2}' "$CPUSTAT")"
fi

# 模型预下载: 已缓存则秒过; 没缓存就先下完再开组, 免得下载吃掉 serve 就绪超时
log "预下载模型 $M (进度见 $RES/download.log)..."
if python3 -c "from huggingface_hub import snapshot_download; snapshot_download('$M')" \
        > "$RES/download.log" 2>&1; then
    log "模型就绪"
else
    log "模型下载失败, 退出; download.log 尾部:"
    tail -5 "$RES/download.log" | tee -a "$SUMMARY"
    exit 1
fi

log "预检通过, 结果目录 $RES/, 实验矩阵 $EXPERIMENT 共 ${#SPECS[@]} 组"

t0=$(date +%s)

# 先把组名收齐, 收尾报告要把没跑到的组也列成"未跑"
# (不能叫 GROUPS: 那是 bash 内建只读数组——当前用户的属组 gid, 赋值被静默忽略)
GROUP_ORDER=()
for spec in "${SPECS[@]}"; do
    IFS='|' read -r g _ <<< "$spec"
    GROUP_ORDER+=("$g")
done

for spec in "${SPECS[@]}"; do
    IFS='|' read -r g cfg expect mon env0 ballast bargs <<< "$spec"
    run_group "$g" "$cfg" "$expect" "$mon" "$env0" "$ballast" "$bargs"
    if [ $? -eq 2 ]; then
        log "!! GPU 状态没恢复, 中止剩余组"
        break
    fi
done

# ── 收尾 ───────────────────────────────────────────────────────────────
log "════ 全部结束, 总耗时 $(( ($(date +%s) - t0) / 60 )) 分钟 ════"
all_ok=1
for g in "${GROUP_ORDER[@]}"; do
    s="${GROUP_STATUS[$g]:-未跑}"
    log "  组 $g: $s"
    [ "$s" = "OK" ] || all_ok=0
done
# 这一晚被 CFS 掐了多少: 和起跑前的快照做差(cpu.stat 是累计值, 绝对值没用)。
# throttled 明显非零 = 这批数字是在 CPU 配额封顶下测的, 组间差先归因 CPU 再谈 IO。
if [ -f "$RES/cpu_stat_before.txt" ] && [ -f "$CPUSTAT" ]; then
    cp "$CPUSTAT" "$RES/cpu_stat_after.txt"
    log "CPU 限流账(本轮增量): $(awk '
        FNR==NR { before[$1]=$2; next }
        /nr_periods|nr_throttled|throttled_usec/ { printf "%s+%d ", $1, $2-before[$1] }
    ' "$RES/cpu_stat_before.txt" "$CPUSTAT")"
fi
log "原始数据: $RES/group_*.json + pidstat/iostat/iou_wrk 日志, 明早把整个 $RES/ 拉回来"

if [ "$AUTO_SHUTDOWN" = 1 ] && [ "$all_ok" = 1 ]; then
    log "全部组 OK, AUTO_SHUTDOWN=1, 60 秒后关机停计费"
    sleep 60
    shutdown -h now
fi
