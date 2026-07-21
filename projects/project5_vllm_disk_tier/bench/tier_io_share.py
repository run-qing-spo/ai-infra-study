#!/usr/bin/env python3
"""从调度层账本算 tier 的 IO 服务时间占 TTFT 的比例 —— fs 和 uring 同高度可比。

和 e3_window_audit.py 的分工:
  e3 吃的是 uring C++ 引擎账本(设备下发→完成), **只有 uring 有**, 所以 e3
     只能审 uring, fs 一直是空白(见 e3 脚本第 27 行)。
  本脚本吃的是 sched_ledger.py 写的调度层账本(submit→框架看到完成),
     **fs 和 uring 都有且同一段代码量出来**。所以 fs 终于能算占比, 且 fs 比
     uring 才是苹果比苹果 —— 不会拿 fs 的一种时间去比 uring 的另一种时间。

调度层账本比 uring 的 C++ 窄账本宽:含排队 + 轮询粒度。这是故意的 —— 请求
真正等的就是这段, 它才是能进 TTFT 占比的分子。uring 窄账本留给 e3 当"设备
读本身多快"的交叉验证。

数据来源(都在同一个 results_e2e_ 目录):
  tier_stats_<组名>.sched.records.jsonl  每行 {tier,job_id,is_write,n_blocks,
                                          t_submit,t_done}, 由 KVTIER_SCHED_LEDGER 落
  group_<组名>.json                       bench 的样本, 取 revisit 的 t_wall/total/ttft

窗口、取窗口内单个最慢 read job 比 TTFT —— 算法沿用 e3(取 max 是给引擎最有利的
算法:连最慢那个 job 都只占 TTFT 一点点, "非 IO-bound"才砸得实)。

用法: python3 bench/tier_io_share.py [results_e2e_目录]
"""
import glob
import json
import os
import statistics
import sys

RES = sys.argv[1] if len(sys.argv) > 1 else \
    "/Users/macbook/ai-infra-study/projects/project5_vllm_disk_tier/results_e2e_20260720_1629"

# 和 e3 同一根判据:IO 服务时间占 TTFT 低于此比例, 这批就比不出引擎差异。
IO_BOUND_FRAC = 0.15
# 读 job 落在计时窗口内的下限;低于它说明加载漂出了窗口, 占比数不可信。
WINDOW_FRAC = 0.5

SUFFIX = ".sched.records.jsonl"

paths = sorted(glob.glob(f"{RES}/tier_stats_*{SUFFIX}"))
if not paths:
    print(f"没找到 {SUFFIX} 账本。是不是 serve 时没设 KVTIER_SCHED_LEDGER?")
    sys.exit(1)

for path in paths:
    name = os.path.basename(path)[len("tier_stats_"):-len(SUFFIX)]
    gpath = f"{RES}/group_{name}.json"
    if not os.path.exists(gpath):
        print(f"{name}: 找不到 {os.path.basename(gpath)}, 跳过")
        continue

    rows = [json.loads(ln) for ln in open(path) if ln.strip()]
    reads = [r for r in rows if not r["is_write"]]
    if not reads:
        print(f"{name}: 整组零读 job —— 前缀没从盘上拉回来过")
        continue
    tier_type = rows[0].get("tier", "?")
    t0 = min(r["t_submit"] for r in rows)  # 最早 submit 当原点, 只为打印好看

    s = json.load(open(gpath))["samples"]
    key = next((k for k in s if k.startswith("revisit")), None)
    if key is None:
        print(f"{name}: 没有 revisit 样本, 跳过")
        continue

    # 窗口 = [该轮最早请求发起时刻, 最晚请求结束时刻]。t_wall 是发起时刻。
    win: dict = {}
    for x in s[key]:
        w = win.setdefault(x["round"], [9e18, 0.0])
        w[0] = min(w[0], x["t_wall"])
        w[1] = max(w[1], x["t_wall"] + x["total"])

    print(f"=== {name}  (tier={tier_type}) ===")
    inside, io_share = 0, []
    for r_no in sorted(win):
        lo, hi = win[r_no]
        rd = [r for r in reads if lo <= r["t_submit"] <= hi]
        inside += sum(r["n_blocks"] for r in rd)
        ttft = statistics.mean(
            x["ttft"] * 1000 for x in s[key] if x["round"] == r_no)
        if rd:
            # 各 read job 并发, 单请求只等它自己那一个 job, 所以拿单 job 服务
            # 时间(不是求和)比 TTFT;取 max 给引擎最有利的算法。
            svc = [(r["t_done"] - r["t_submit"]) * 1000 for r in rd]
            frac = max(svc) / ttft
            io_share.append(frac)
            note = (f"IO 服务 mean={statistics.mean(svc):5.1f}ms "
                    f"max={max(svc):6.1f}ms  → 占 TTFT {frac:5.1%}")
        else:
            note = "窗口内零读 —— 先和 iostat 对账, 可能账本截断而非真没读"
        print(f"  r{r_no} 窗口[{lo - t0:6.1f},{hi - t0:6.1f}]s  "
              f"读job={len(rd):3d} blocks={sum(r['n_blocks'] for r in rd):6d}  "
              f"TTFT={ttft:7.1f}ms  {note}")

    total = sum(r["n_blocks"] for r in reads)
    wfrac = inside / total if total else 0.0
    if wfrac < WINDOW_FRAC:
        print(f"  ⚠ 窗口内读块仅 {inside}/{total} = {wfrac:.0%}, 加载漂出计时窗口, "
              f"下面的占比不可信")
        continue
    if not io_share:
        print("  所有轮次零读 —— 这批负载没让 revisit 碰盘")
        continue
    peak = max(io_share)
    verdict = ("IO-bound, 可以比引擎" if peak >= IO_BOUND_FRAC else
               f"非 IO-bound: IO 最多只占 TTFT {peak:.1%}, "
               f"端到端 TTFT 比不出 fs/uring 的引擎差异")
    print(f"  窗口内读块 {inside}/{total} = {wfrac:.0%} | "
          f"各轮 IO 占比峰值 {peak:.1%}  → {verdict}")
