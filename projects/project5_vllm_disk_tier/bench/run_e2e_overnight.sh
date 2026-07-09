#!/usr/bin/env bash
# 无人值守跑完 A / B / C1 / C2 四组端到端 TTFT 对比(docs/RUN_ON_GPU.md 阶段 2)。
# 设计目标:起脚本 → 断开 ssh → 第二天看 results_e2e_*/SUMMARY.txt。
#
# 用法(远端 GPU 机, 在项目根目录):
#   tmux new -s e2e
#   bash bench/run_e2e_overnight.sh          # Ctrl-B D 断开
#   # 或者不用 tmux: nohup bash bench/run_e2e_overnight.sh > overnight.log 2>&1 &
#
# 产物(全部落在 results_e2e_<时间戳>/ 下):
#   SUMMARY.txt        每组的关键事件 + tier 日志行 + revisit/prime 比值, 先看这个
#   group_{A,B,C1,C2}.json   bench 原始样本
#   serve_*.log bench_*.log  serve / 负载的完整输出
#   pidstat_* iostat_* iou_wrk_*  C1/C2 期间的 CPU 论点监控
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
AUTO_SHUTDOWN="${AUTO_SHUTDOWN:-0}"

CFG_B='{"kv_connector":"OffloadingConnector","kv_role":"kv_both","kv_connector_extra_config":{"spec_name":"CPUOffloadingSpec","cpu_bytes_to_use":4294967296}}'
CFG_C1='{"kv_connector":"OffloadingConnector","kv_role":"kv_both","kv_connector_extra_config":{"spec_name":"TieringOffloadingSpec","cpu_bytes_to_use":4294967296,"secondary_tiers":[{"type":"fs","root_dir":"'$DATA'/kv_fs"}]}}'
CFG_C2='{"kv_connector":"OffloadingConnector","kv_role":"kv_both","kv_connector_extra_config":{"spec_name":"TieringOffloadingSpec","cpu_bytes_to_use":4294967296,"secondary_tiers":[{"type":"uring","path":"'$DATA'/kv_tier.bin","disk_bytes_to_use":21474836480,"queue_depth":32}]}}'

cd "$(dirname "$0")/.."
RES="results_e2e_$(date +%Y%m%d_%H%M)"
mkdir -p "$RES"
SUMMARY="$RES/SUMMARY.txt"

log() { echo "[$(date '+%F %T')] $*" | tee -a "$SUMMARY"; }

SERVE_PID=""
MON_PIDS=()

cleanup() {
    # 脚本被杀 / 出错退出时别留孤儿: 掐掉监控和 serve 进程组
    ((${#MON_PIDS[@]})) && kill "${MON_PIDS[@]}" 2>/dev/null
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
    # 兜底: 引擎在 EngineCore 子进程里, INT 偶尔收不干净
    pkill -9 -f "vllm serve" 2>/dev/null
    pkill -9 -f "EngineCore" 2>/dev/null
    SERVE_PID=""
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

# ── 监控(C1/C2): serve 全家的 CPU + 盘 + iou-wrk 线程数 ────────────────
start_monitors() {
    local group=$1 pids
    # 注意别用裸 "vllm" 匹配: 项目路径里就带 vllm 字样, 会把本脚本自己算进去
    pids=$(pgrep -d, -f "vllm serve|EngineCore") || pids=""
    if command -v pidstat >/dev/null && [ -n "$pids" ]; then
        pidstat -u -p "$pids" 5 > "$RES/pidstat_$group.log" 2>&1 &
        MON_PIDS+=($!)
    fi
    if command -v iostat >/dev/null; then
        iostat -x 5 > "$RES/iostat_$group.log" 2>&1 &
        MON_PIDS+=($!)
    fi
    # iou-wrk 是内核线程, 全系统数一遍即可(独占机器, 没别的 uring 用户)
    ( while sleep 1; do
          echo "$(date +%T) $(ps -eLo comm= 2>/dev/null | grep -c '^iou-wrk')"
      done > "$RES/iou_wrk_$group.log" ) &
    MON_PIDS+=($!)
}

stop_monitors() {
    ((${#MON_PIDS[@]})) && kill "${MON_PIDS[@]}" 2>/dev/null
    MON_PIDS=()
}

# ── 一组的完整节奏: 起 serve → 等就绪 → 验 tier → 负载 → 收尾 ──────────
# run_group <组名> <kv-transfer-config|-> <期望tier日志|-> <monitor:0/1> <env|->
run_group() {
    local group=$1 cfg=$2 expect=$3 mon=$4 env0=$5
    local slog="$RES/serve_$group.log"

    log "════ 组 $group 开始 (显存基线 $(gpu_used_mb) MiB) ════"

    local cmd=()
    [ "$env0" != "-" ] && cmd+=(env "$env0")
    cmd+=(vllm serve "$M" --max-model-len "$MAXLEN" --port "$PORT")
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

    [ "$mon" = 1 ] && start_monitors "$group"
    sleep 5

    log "  跑负载..."
    timeout "$BENCH_TIMEOUT" python3 bench/long_context_ttft.py \
        --model "$M" --json "$RES/group_$group.json" \
        > "$RES/bench_$group.log" 2>&1
    local rc=$?

    [ "$mon" = 1 ] && stop_monitors

    if [ $rc -eq 0 ]; then
        # 把三行 phase 汇总和比值直接抄进 SUMMARY, 明早一眼能看
        grep -E "^(prime|churn|revisit) |revisit/prime" "$RES/bench_$group.log" | \
            while IFS= read -r l; do log "  $l"; done
        [ -z "${GROUP_STATUS[$group]:-}" ] && GROUP_STATUS[$group]="OK"
    else
        log "  组 $group 负载失败 rc=$rc, bench 日志尾部:"
        tail -5 "$RES/bench_$group.log" | tee -a "$SUMMARY"
        GROUP_STATUS[$group]="FAIL(bench rc=$rc)"
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
[ -f bench/long_context_ttft.py ]    || { log "找不到 bench/long_context_ttft.py (要在项目根跑)"; fail=1; }
[ -d "$DATA" ]                       || { log "数据盘 $DATA 不存在"; fail=1; }
pgrep -f "vllm serve" >/dev/null && { log "已有 vllm serve 在跑, 先清掉再来"; fail=1; }
command -v pidstat >/dev/null || log "警告: 没有 pidstat(sysstat), CPU 监控会缺失, 不阻塞"
avail_gb=$(df -BG --output=avail "$DATA" 2>/dev/null | tail -1 | tr -dc 0-9)
[ -n "$avail_gb" ] && (( avail_gb < 40 )) && log "警告: $DATA 只剩 ${avail_gb}G, C 组要 20G+ 备份文件, 可能不够"
(( fail )) && { log "预检失败, 退出"; exit 1; }
log "预检通过, 结果目录 $RES/, 开始四组"

t0=$(date +%s)

# 组名|kv-transfer-config|期望tier日志|要不要监控|额外env ("-" = 无)
for spec in \
    "A|-|-|0|-" \
    "B|$CFG_B|-|0|-" \
    "C1|$CFG_C1|fs|1|PYTHONHASHSEED=0" \
    "C2|$CFG_C2|uring|1|-"
do
    IFS='|' read -r g cfg expect mon env0 <<< "$spec"
    run_group "$g" "$cfg" "$expect" "$mon" "$env0"
    if [ $? -eq 2 ]; then
        log "!! GPU 状态没恢复, 中止剩余组"
        break
    fi
    [ "$g" = C1 ] && rm -rf "$DATA/kv_fs"    # 给 C2 腾磁盘
done

# ── 收尾 ───────────────────────────────────────────────────────────────
log "════ 全部结束, 总耗时 $(( ($(date +%s) - t0) / 60 )) 分钟 ════"
all_ok=1
for g in A B C1 C2; do
    s="${GROUP_STATUS[$g]:-未跑}"
    log "  组 $g: $s"
    [ "$s" = "OK" ] || all_ok=0
done
log "原始数据: $RES/group_*.json + pidstat/iostat/iou_wrk 日志, 明早把整个 $RES/ 拉回来"

if [ "$AUTO_SHUTDOWN" = 1 ] && [ "$all_ok" = 1 ]; then
    log "四组全 OK, AUTO_SHUTDOWN=1, 60 秒后关机停计费"
    sleep 60
    shutdown -h now
fi
