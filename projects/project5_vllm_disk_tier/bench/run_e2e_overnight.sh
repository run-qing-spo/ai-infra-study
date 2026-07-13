#!/usr/bin/env bash
# 无人值守跑一个实验矩阵的端到端 TTFT 对比(COMPARE_PLAN 第 2 步)。
# 设计目标:起脚本 → 断开 ssh → 第二天看 results_e2e_*/SUMMARY.txt。
#
# 用法(远端 GPU 机, 在项目根目录):
#   tmux new -s e2e
#   bash bench/run_e2e_overnight.sh e1        # Ctrl-B D 断开
#   bash bench/run_e2e_overnight.sh e3        # 并发轴: fs/uring 各一次 serve, 扫 c∈{1,4,8,16}×3轮
#   bash bench/run_e2e_overnight.sh e4        # 读写混战: churn 流不停 + revisit 波 @c8 + ITL 探针
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
gil)
    # churn +10ms 悬案的证伪探针(COMPARE_PLAN E3 冻结段): 假说是 fs 的 store
    # 任务在 EngineCore 进程里以 Python 线程跑 open/write/replace, 每块 IO
    # 前后的字节码持 GIL, 和引擎调度热循环抢锁(project4 微基准的"GIL 税≈0"
    # 是独立进程, 不覆盖这种同进程共锁)。把 store 线程 16→2, 抢锁者少 8 倍:
    # churn TTFT 若 -10ms 同向收窄 → GIL/派发线程数坐实; 不动 → 悬案继续。
    # 效应量(~10ms)低于跨 serve 噪声底(15-20ms), 单组对比不算数 ——
    # w16/w2 交替各两个 serve, 判据用批间同向性(26 批全同向的老办法),
    # 不看均值差。w2 的写带宽不构成混淆: 2 线程 O_DIRECT 顺序写 ≫ churn
    # 期实测 ~85MB/s 的 store 流量。phases 沿用 E3 全谱, 顺带观测 store
    # 排空变慢对 revisit 有无反作用
    GIL_PHASES="prime"
    for c in 1 4 8 16; do
        for _ in 1 2 3; do GIL_PHASES+=",churn,revisit@$c"; done
    done
    CFG_FS_W2="${CFG_FS/\"root_dir\"/\"n_write_threads\":2,\"root_dir\"}"
    SPECS=(
        "GIL_w16_a|$CFG_FS|fs|1|PYTHONHASHSEED=0|0|--phases $GIL_PHASES"
        "GIL_w2_a|$CFG_FS_W2|fs|1|PYTHONHASHSEED=0|0|--phases $GIL_PHASES"
        "GIL_w16_b|$CFG_FS|fs|1|PYTHONHASHSEED=0|0|--phases $GIL_PHASES"
        "GIL_w2_b|$CFG_FS_W2|fs|1|PYTHONHASHSEED=0|0|--phases $GIL_PHASES"
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
    echo "未知实验矩阵 '$EXPERIMENT'(可选: e1 / e3 / e4 / gil / legacy)" >&2
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
    [ -n "$SERVE_PID" ] && kill -TERM -- -"$SERVE_PID" 2>/dev/null
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
    timeout "$BENCH_TIMEOUT" python3 bench/long_context_ttft.py \
        --model "$M" --json "$RES/group_$group.json" "${extra[@]}" \
        > "$RES/bench_$group.log" 2>&1
    local rc=$?

    [ "$mon" = 1 ] && stop_monitors
    stop_ballast

    if [ $rc -eq 0 ]; then
        # 把 phase 汇总和比值直接抄进 SUMMARY, 明早一眼能看
        # (revisit@N 带并发后缀, 模式不能再要求 revisit 后面紧跟空格;
        #  mixed 阶段的行是 " wave N" / mixed_revisit@N / mixed_churn / probe ITL)
        grep -E "^(prime|churn|revisit|mixed|probe| wave)|/prime TTFT ratio" \
            "$RES/bench_$group.log" | \
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

    if ! stop_serve; then
        log "  !! 显存清不掉, 中止剩余组, 免得后面全是 OOM 废数据"
        GROUP_STATUS[$group]="${GROUP_STATUS[$group]} GPU_STUCK"
        return 2
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
log "原始数据: $RES/group_*.json + pidstat/iostat/iou_wrk 日志, 明早把整个 $RES/ 拉回来"

if [ "$AUTO_SHUTDOWN" = 1 ] && [ "$all_ok" = 1 ]; then
    log "全部组 OK, AUTO_SHUTDOWN=1, 60 秒后关机停计费"
    sleep 60
    shutdown -h now
fi
