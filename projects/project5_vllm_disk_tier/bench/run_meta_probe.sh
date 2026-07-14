#!/usr/bin/env bash
# 元数据面假说的因子分离实验(需要 root:drop_caches)。
#
# 要证的假说见 bench/meta_probe.cpp 头注释。这里负责构造自变量并跑满矩阵:
#
#   布局轴   files(N 个小文件) / slab(单文件复用 fd) / slab-reopen(单文件每次重开)
#   缓存轴   hot(全热) / drop2(只丢 dentry+inode slab) / drop3(slab + page cache 全丢)
#
# 读判据的顺序(先证自变量动了, 再看因变量):
#
#   0. 前置    每次 probe 前后打 /proc/slabinfo 的 dentry/inode active_objs。
#              drop 之后没掉 = 自变量根本没动, 后面的数一概不算数。
#   1. 主证据  files 的 open_us:hot → drop2 → drop3 是否阶梯上涨?
#              而 slab-reopen 的 open_us 三档应该基本平 —— 它 open 次数一样多,
#              只是 dentry 只有一条。两者的差 = "N 条 dentry"的代价。
#   2. 分账    drop2−hot   = 纯 CPU 的路径重建成本(块还在 page cache, 不读盘)
#              drop3−drop2 = 盘上元数据 IO 的成本
#              这一刀正好回答"回收本身不也占 CPU 吗"—— 两笔账分开量。
#   3. 旁证    iostat 的 rareq-sz:drop3 + files 应该混进 4KiB 量级的小读
#              (数据块 0.875MiB, 尺寸上完全可分); 其余组合不该有。
#   4. 对照    read_us 三档应该基本不变 —— 数据走 O_DIRECT, 本来就不吃 page cache。
#              它要是也在动, 说明有别的东西在干扰, 整轮作废。
#
# 用法:
#   sudo bash bench/run_meta_probe.sh /mnt/nvme/metaprobe [blocks] [reads]
#   blocks 默认 15000(对齐 E1 实测的 15574 个 kv_fs 文件), reads 默认 1000。
#   磁盘占用 = blocks × 0.875MiB × 2 份(files + slab), 15000 块约需 26G。
#
# 注意 reads 必须 ≤ blocks:probe 读的是 shuffle 后不重复的块序列, 这样 files
# 布局全程都是新 dentry(不会自己把缓存预热掉)。而 slab-reopen 反复开同一个
# 文件, 只有第一次 miss —— 这不是 bug, 这正是判据要看的东西。

set -uo pipefail

DIR=${1:?用法: sudo bash bench/run_meta_probe.sh <数据目录> [blocks] [reads]}
BLOCKS=${2:-15000}
READS=${3:-1000}
BIN=./meta_probe
RES="results_meta_$(date +%Y%m%d_%H%M)"

[ "$(id -u)" -eq 0 ] || { echo "需要 root:drop_caches 要写 /proc/sys/vm/"; exit 1; }
[ -x "$BIN" ] || { echo "先编译: make meta_probe"; exit 1; }
[ "$READS" -le "$BLOCKS" ] || { echo "reads($READS) 必须 ≤ blocks($BLOCKS), 理由见头注释"; exit 1; }

mkdir -p "$RES" "$DIR"
DEV=$(df --output=source "$DIR" | tail -1 | sed 's|/dev/||')
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$RES/run.log"; }

log "数据目录 $DIR (设备 $DEV, 文件系统 $(df --output=fstype "$DIR" | tail -1))"
log "blocks=$BLOCKS reads=$READS, 预计占盘 $(( BLOCKS * 875 * 2 / 1000 / 1024 ))G"

# ── 造数据 ──────────────────────────────────────────────────────────────
# files 和 slab 各一份; slab-reopen 复用 slab 的数据。
for layout in files slab; do
    if [ -e "$DIR/$layout.done" ]; then
        log "跳过 build($layout): 已存在"
    else
        log "build $layout ..."
        "$BIN" build --dir "$DIR/$layout" --layout "$layout" --blocks "$BLOCKS" \
            2>&1 | tee -a "$RES/build_$layout.log"
        touch "$DIR/$layout.done"
    fi
done
sync

# ── 缓存条件 ────────────────────────────────────────────────────────────
# 四档, 前三档用 drop_caches 直接清, 第四档用 ballast 制造持续内存压力。
#
# drop 和 ballast 的目标状态相同(缓存空了), 手段完全不同, 两个都要:
#   drop_caches  一次性命令内核丢掉缓存。优点是能精确切分(drop2 只丢 slab,
#                ballast 做不到); 缺点是压力一撤缓存就开始长回来 —— 冷条件
#                会在 probe 过程中自我衰减(见 meta_probe.cpp 的 mean_slice)。
#   ballast      占住回收不掉的匿名内存, 逼内核持续回收。这才是 e2e 里 fs tier
#                真正待着的条件, 微基准要接回 E1 那 40ms 就少不了它。
# 换句话说:drop2/drop3 用来证机制, ballast 用来和 e2e 对齐。
#
# drop_caches 前必须 sync:脏页丢不掉, 不 sync 的话 drop 是部分失效的。
# (本实验全程 O_DIRECT, 脏页主要来自 build 阶段的元数据)
BALLAST_PID=""
BALLAST_AVAIL_GIB=${BALLAST_AVAIL_GIB:-4}   # 压到 MemAvailable ≤ 这个值; 须 < 工作集

start_ballast() {
    # 裸机没有 cgroup 上限, 用 --avail-gib 判据(容器里才用 --cache-room-gib)
    python3 bench/ram_ballast.py --avail-gib "$BALLAST_AVAIL_GIB" \
        > "$RES/ballast_$1.log" 2>&1 &
    BALLAST_PID=$!
    for _ in $(seq 120); do
        grep -q "BALLAST READY" "$RES/ballast_$1.log" 2>/dev/null && {
            log "  $(grep BALLAST\ READY "$RES/ballast_$1.log")"; return 0; }
        kill -0 "$BALLAST_PID" 2>/dev/null || { log "  ballast 进程死了"; return 1; }
        sleep 1
    done
    log "  ballast 120s 没就绪"; return 1
}

stop_ballast() {
    [ -n "$BALLAST_PID" ] && kill "$BALLAST_PID" 2>/dev/null
    wait "$BALLAST_PID" 2>/dev/null
    BALLAST_PID=""
}

trap 'stop_ballast; kill $(jobs -p) 2>/dev/null' EXIT INT TERM

set_cache() {
    case "$1" in
        hot)   # 先跑一遍完整 probe 把 dcache/inode 拉热, 结果丢弃
               log "  预热(结果丢弃) ..."
               "$BIN" probe --dir "$DIR/$2" --layout "$3" --blocks "$BLOCKS" \
                   --reads "$READS" >/dev/null 2>&1 ;;
        drop2) sync; echo 2 > /proc/sys/vm/drop_caches ;;   # 只丢 dentry + inode slab
        drop3) sync; echo 3 > /proc/sys/vm/drop_caches ;;   # slab + page cache 一起丢
        ballast)
               # 先清干净再压:光有压力不清缓存的话, 内核未必回收得动已经热着的
               # dentry(它有引用计数和 LRU 保护), 起点不确定。清完再压 = 起点
               # 确定 + 全程不许长回来。
               sync; echo 3 > /proc/sys/vm/drop_caches
               start_ballast "$4" || return 1 ;;
    esac
    sleep 2   # 让 shrinker / kswapd 跑完再开表
}

# ── 矩阵 ────────────────────────────────────────────────────────────────
for layout in files slab slab-reopen; do
    data=$layout
    [ "$layout" = "slab-reopen" ] && data=slab   # 复用 slab 的数据

    for cache in hot drop2 drop3 ballast; do
        tag="${layout}_${cache}"
        log "── $tag ──"
        if ! set_cache "$cache" "$data" "$layout" "$tag"; then
            log "  缓存条件没构造出来, 跳过"
            stop_ballast
            continue
        fi

        iostat -xmt 1 "$DEV" > "$RES/iostat_$tag.log" 2>&1 &
        iopid=$!

        "$BIN" probe --dir "$DIR/$data" --layout "$layout" --blocks "$BLOCKS" \
            --reads "$READS" > "$RES/probe_$tag.json" 2> "$RES/slab_$tag.log"
        rc=$?

        kill $iopid 2>/dev/null; wait $iopid 2>/dev/null
        stop_ballast

        if [ $rc -ne 0 ]; then
            log "  失败 rc=$rc, 见 $RES/slab_$tag.log"
            continue
        fi
        # 一行摘要:open/read 的 mean 和 p99, 明眼看阶梯; 外加冷条件衰减检查
        python3 - "$RES/probe_$tag.json" <<'PY' | tee -a "$RES/run.log"
import json, sys
d = json.load(open(sys.argv[1]))
o, r = d["open_us"], d["read_us"]
print(f"  open  mean={o['mean']:8.1f}us p99={o['p99']:9.1f}us  总计 {o['total_ms']:7.1f}ms")
print(f"  read  mean={r['mean']:8.1f}us p99={r['p99']:9.1f}us  总计 {r['total_ms']:7.1f}ms")
print(f"  吞吐  {d['mibps']:.0f} MiB/s  (墙钟 {d['wall_s']:.1f}s)")
h1, h2 = o["first_half"], o["second_half"]
if h1 > 0:
    drift = (h2 - h1) / h1 * 100
    flag = "  <<< 冷条件在自我衰减, 这一档的均值被稀释了" if drift < -25 else ""
    print(f"  衰减  open 前半 {h1:.1f}us → 后半 {h2:.1f}us ({drift:+.0f}%){flag}")
PY
        # 旁证:这一档的设备平均读请求尺寸。files+drop3 若混进元数据小读, 这里会掉。
        awk '/^ *'"$DEV"'/ {n++; sz+=$8} END {if(n) printf "  设备平均读请求尺寸 %.0f KiB (%d 个采样点)\n", sz/n*1024, n}' \
            "$RES/iostat_$tag.log" | tee -a "$RES/run.log"
    done
done

# ── 汇总 ────────────────────────────────────────────────────────────────
log "── 汇总(open 延迟, 单位 us)──"
python3 - "$RES" <<'PY' | tee -a "$RES/run.log"
import json, os, sys
res = sys.argv[1]
CACHES = ("hot", "drop2", "drop3", "ballast")
rows = {}
for layout in ("files", "slab", "slab-reopen"):
    for cache in CACHES:
        p = os.path.join(res, f"probe_{layout}_{cache}.json")
        if os.path.exists(p):
            rows[(layout, cache)] = json.load(open(p))

hdr = f"{'layout':<14}" + "".join(f"{c:>11}" for c in CACHES)
print(hdr + f"   {'drop2-hot':>11}{'drop3-drop2':>13}")
for layout in ("files", "slab", "slab-reopen"):
    got = [rows.get((layout, c)) for c in CACHES]
    if not any(got):
        continue
    cells = "".join(f"{d['open_us']['mean']:>11.1f}" if d else f"{'-':>11}" for d in got)
    line = f"{layout:<14}{cells}"
    hot, d2, d3 = (g["open_us"]["mean"] if g else None for g in got[:3])
    if hot is not None and d2 is not None and d3 is not None:
        line += f"   {d2-hot:>11.1f}{d3-d2:>13.1f}"
    print(line)
print()
print("判读:")
print("  files 的 drop2-hot   = 纯 CPU 路径重建成本(dentry 没了, 但块还在 page cache)")
print("  files 的 drop3-drop2 = 盘上元数据 IO 成本(块也没了, 必须真读盘)")
print("  slab-reopen 四档应基本平 —— 它 open 次数与 files 相同, 只是 dentry 只有一条。")
print("  若 files 涨而 slab-reopen 不涨 → 贵的不是 open() 本身, 是 N 条 dentry。假说成立。")
print("  若两者一起涨 → 假说被证伪, 成本在 open() 系统调用本身, 与文件数无关。")
print()
print("  ballast 档是和 e2e 对齐用的:它是 fs tier 在 E1 冷组里真正待着的条件")
print("  (持续内存压力, 而非一次性 drop)。判据是它应当复现 drop3 的量级 ——")
print("  若 ballast 明显轻于 drop3, 说明内存压力并没有把 dentry 回收干净,")
print("  那么 E1 冷组那 40ms 就不能全记在元数据面头上, 得另找一部分解释。")
print("  各档的'衰减'行(前半 vs 后半)先过一眼:后半明显更快 = 冷条件没维持住。")
PY

log "完成, 结果在 $RES/"
