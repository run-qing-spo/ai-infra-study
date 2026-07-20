#!/usr/bin/env python3
# E2 store 剖析的读数脚本(COMPARE_PLAN E2 重启版)。
#
# 一次回答 E2 要收的三件事, 全部从 results_e2e_*/ 现算:
#   [1] churn TTFT 税  —— fs/uring 的 churn 均值相对无 offload 基线(none 组)
#       差多少。效应量 ~10ms 低于跨 serve 噪声底 15~20ms, 所以判据是**同向性**
#       (a/b/c 三对全同向才算数), 不是均值差 —— 沿用 E3 "26 批全同向"的老办法。
#   [2] 吸收率与漏斗 —— uring 的唯一块账本(manager 的 _ever_stored 计数)对
#       fs 的文件账, 同口径比。漏斗逐级列出来: 上层发下来多少 → filter 挡掉
#       多少 → 容量/环满拒多少 → 首写多少 → 重写多少。"上层静默丢块"就看
#       offered 对不对得上负载上界: 对得上说明丢在我们这层, 对不上说明 vLLM
#       根本没往下发。
#   [3] per-job 时间分解 —— store/load 各自的 queue(排队: SPSC + worker 忙 +
#       gather 等)与 service(ring 里飞)分位数。+10ms 的通路定位靠它, 不用
#       再从 iostat 外面猜。
#
# 用法(在 GPU 机上, 项目根):
#   python3 bench/e2_store_audit.py results_e2e_20260720_1546

import argparse
import json
import re
import statistics
import sys
from pathlib import Path

# manager._RECORD_SCHEMA 的字段序, 改那边必须同步改这里
REC = ["job_id", "is_write", "n_blocks", "t_submit", "t_first_issue",
       "t_done", "q_jobs_at_submit", "dev_inflight_at_issue"]


def pct(xs, p):
    if not xs:
        return float("nan")
    return statistics.quantiles(xs, n=100)[p - 1] if len(xs) > 1 else xs[0]


def churn_mean(group_json: Path) -> tuple[float, int]:
    """合并该组所有 churn 轮的 ttft 均值, 毫秒(与 bench 的 n=48 合并行同口径)。

    JSON 里的 ttft 是秒(perf_counter 差值), report() 打印时才 ×1000。
    """
    data = json.loads(group_json.read_text())
    samples = data.get("samples", data)   # 老格式没有 samples 包一层
    xs = [s["ttft"] * 1000 for k, v in samples.items()
          if k.split("@")[0] == "churn"
          for s in v if s.get("ttft") is not None]
    return (statistics.fmean(xs) if xs else float("nan")), len(xs)


def fs_disk_from_summary(res: Path) -> dict[str, tuple[int, int]]:
    """从 SUMMARY.txt 抓每个 fs 组的 'kv_fs 落盘: N 个文件, M MiB'。"""
    out = {}
    summary = res / "SUMMARY.txt"
    if not summary.exists():
        return out
    cur = None
    for line in summary.read_text(errors="replace").splitlines():
        m = re.search(r"════ 组 (\S+) 开始", line)
        if m:
            cur = m.group(1)
        m = re.search(r"kv_fs 落盘: (\d+) 个文件, (\d+) MiB", line)
        if m and cur:
            out[cur] = (int(m.group(1)), int(m.group(2)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results_dir")
    ap.add_argument("--churn-blocks", type=int, default=488,
                    help="一条 churn 请求产出的 KV 块数(6000 词 ≈ 488 块)")
    ap.add_argument("--prime-blocks", type=int, default=8000,
                    help="prime 相位产出的块数")
    args = ap.parse_args()

    res = Path(args.results_dir)
    if not res.is_dir():
        sys.exit(f"结果目录不存在: {res}")

    groups = sorted(p.stem[len("group_"):] for p in res.glob("group_*.json"))
    if not groups:
        sys.exit(f"{res} 下没有 group_*.json")

    def tier_of(g):
        for t in ("none", "fs", "uring"):
            if f"_{t}_" in g or g.endswith(f"_{t}"):
                return t
        return "?"

    # ---- [1] churn TTFT 税 ----------------------------------------------
    print("=" * 66)
    print("[1] churn TTFT 税(判据是同向性, 不是均值差 —— 效应量在噪声底以下)")
    print("=" * 66)
    means = {}
    for g in groups:
        m, n = churn_mean(res / f"group_{g}.json")
        means[g] = m
        print(f"  {g:<16} churn n={n:<4} 均值 {m:8.1f}ms")

    # 按 a/b/c 后缀配对: 同一时段的相邻 serve, 环境最接近
    reps = sorted({g.rsplit("_", 1)[-1] for g in groups
                   if g.rsplit("_", 1)[-1] in "abc" and len(g.rsplit("_", 1)[-1]) == 1})
    if reps:
        print(f"\n  {'轮':<4}{'none':>10}{'fs':>10}{'uring':>10}"
              f"{'fs−none':>10}{'uring−none':>12}{'fs−uring':>10}")
        wins = {"fs>uring": 0, "fs>none": 0, "n": 0}
        for r in reps:
            row = {tier_of(g): means[g] for g in groups if g.endswith(f"_{r}")}
            if not {"none", "fs", "uring"} <= row.keys():
                continue
            wins["n"] += 1
            wins["fs>uring"] += row["fs"] > row["uring"]
            wins["fs>none"] += row["fs"] > row["none"]
            print(f"  {r:<4}{row['none']:>10.1f}{row['fs']:>10.1f}"
                  f"{row['uring']:>10.1f}{row['fs'] - row['none']:>+10.1f}"
                  f"{row['uring'] - row['none']:>+12.1f}"
                  f"{row['fs'] - row['uring']:>+10.1f}")
        if wins["n"]:
            print(f"\n  同向性: fs>uring {wins['fs>uring']}/{wins['n']}, "
                  f"fs>none {wins['fs>none']}/{wins['n']}"
                  "   (全同向才下结论)")

    # ---- [2] 吸收率与漏斗 ------------------------------------------------
    print()
    print("=" * 66)
    print("[2] 吸收率与漏斗(uring 唯一块账本 vs fs 文件账, 同口径)")
    print("=" * 66)
    fs_disk = fs_disk_from_summary(res)
    for g, (nf, mib) in sorted(fs_disk.items()):
        print(f"  {g:<16} fs 文件账: {nf} 文件 = {mib} MiB (= 唯一块, fs 无逐出)")

    stats_files = sorted(res.glob("tier_stats_*.json"))
    if not stats_files:
        print("  !! 没有 tier_stats_*.json —— uring 账本没收到, [2][3] 无数据")
        return

    for sf in stats_files:
        d = json.loads(sf.read_text())
        c = d.get("counters", {})
        gg = d.get("gauges", {})
        bs = d.get("block_size", 0)
        mib = lambda blocks: blocks * bs / 1024 / 1024   # noqa: E731
        first = c.get("store_blocks_first", 0)
        offered = c.get("store_blocks_offered", 0)
        ub = args.prime_blocks + args.churn_blocks * 48   # 负载上界(见 e2 spec)

        print(f"\n  {sf.stem[len('tier_stats_'):]}  (block={bs}B)")
        print(f"    上层发下来 offered      {offered:>8}  = {mib(offered):8.0f} MiB")
        print(f"      ├ filter 挡掉(已在盘) {c.get('store_blocks_filtered', 0):>8}")
        print(f"      ├ 容量拒              {c.get('store_blocks_rejected_capacity', 0):>8}"
              f"   (job {c.get('store_jobs_rejected_capacity', 0)})")
        print(f"      ├ 引擎环满拒          {c.get('store_blocks_rejected_ring', 0):>8}"
              f"   (job {c.get('store_jobs_rejected_ring', 0)})")
        print(f"      ├ IO 失败             {c.get('store_blocks_io_failed', 0):>8}")
        print(f"      ├ 重写(逐出后回来)    {c.get('store_blocks_rewrite', 0):>8}"
              f"   逐出块 {c.get('evicted_blocks', 0)}")
        print(f"      └ 首写=唯一块         {first:>8}  = {mib(first):8.0f} MiB")
        print(f"    lookup  hit {c.get('lookup_hit', 0)} / "
              f"retry {c.get('lookup_retry', 0)} / miss {c.get('lookup_miss', 0)}")
        print(f"    load    job {c.get('load_jobs', 0)} (promotion "
              f"{c.get('load_jobs_promotion', 0)}) / block {c.get('load_blocks', 0)}")
        # promotion 对账: lookup 答过 hit 的块数 − 实际来的 load 块数
        # = 上层放弃的 promotion(CPU tier 满时 vLLM 直接判 MISS, 不调到这层)
        gap = c.get("lookup_hit", 0) - c.get("load_blocks", 0)
        print(f"    上层放弃的 promotion    {gap:>8}   (= lookup_hit − load_blocks)")
        print(f"    吸收率  首写/offered {first / offered * 100:5.1f}%   "
              f"首写/负载上界({ub}) {first / ub * 100:5.1f}%")
        print(f"    收尾在盘 {gg.get('present_blocks', 0)} 块 / "
              f"曾落盘 {gg.get('ever_stored_blocks', 0)} 块 / "
              f"空闲 slot {gg.get('free_slots', 0)}")
        eng = d.get("engine", {})
        if eng:
            print(f"    引擎物理口径: 写 {eng.get('bytes_written', 0)/2**30:.2f} GiB / "
                  f"读 {eng.get('bytes_read', 0)/2**30:.2f} GiB / "
                  f"submit 调用 {eng.get('submit_calls', 0)} / "
                  f"job 失败 {eng.get('jobs_failed', 0)}")

    # ---- [3] per-job 时间分解 --------------------------------------------
    print()
    print("=" * 66)
    print("[3] per-job 时间分解(queue = 排队等 worker, service = ring 里飞)")
    print("=" * 66)
    for sf in stats_files:
        d = json.loads(sf.read_text())
        rows = d.get("job_records", {}).get("rows", [])
        if not rows:
            print(f"  {sf.stem}: 无 job 记录")
            continue
        print(f"\n  {sf.stem[len('tier_stats_'):]}   共 {len(rows)} 个 job")
        for is_write, label in ((True, "store"), (False, "load ")):
            sel = [dict(zip(REC, r)) for r in rows if bool(r[1]) is is_write]
            if not sel:
                print(f"    {label}: 无")
                continue
            q = [(r["t_first_issue"] - r["t_submit"]) * 1000 for r in sel]
            s = [(r["t_done"] - r["t_first_issue"]) * 1000 for r in sel]
            blk = [r["n_blocks"] for r in sel]
            qd = [r["q_jobs_at_submit"] for r in sel]
            print(f"    {label} n={len(sel):<5} 块/job 均{statistics.fmean(blk):5.1f} "
                  f"最大{max(blk)}   提交时在途 job 均{statistics.fmean(qd):4.1f} "
                  f"最大{max(qd)}")
            print(f"          queue   p50 {pct(q, 50):8.2f}ms  p99 {pct(q, 99):8.2f}ms  "
                  f"max {max(q):8.2f}ms")
            print(f"          service p50 {pct(s, 50):8.2f}ms  p99 {pct(s, 99):8.2f}ms  "
                  f"max {max(s):8.2f}ms")


if __name__ == "__main__":
    main()
