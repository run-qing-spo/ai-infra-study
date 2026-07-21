#!/usr/bin/env python3
"""E3 窗口审计: revisit 的 TTFT 里, 真正花在盘上的有多少。

2026-07-20 的 e3dentry 六组用这个脚本翻了两次案, 过程本身是教训:

  第一次(错的)  把样本的 t_wall 当成请求**结束**时刻, 拿 t_wall - total 当窗口
                 起点, 窗口整体前移了一个请求时长, 于是看到"读 job 全落在窗口
                 之外", 得出"加载滞后、没测到引擎"的结论 —— 纯属对齐错误。
                 t_wall 取值在请求发出**之前**(long_context_ttft.py one_request),
                 窗口是 [t_wall, t_wall + total]。
  第二次           读 job 100% 落在 revisit 窗口内。r1 读 5832 块 IO 只花 40ms 而
                 TTFT 1121ms, r2 读 6221 块 45ms TTFT 531ms —— IO 占 TTFT 不到
                 10%, 引擎再快也没地方兑现。这条站得住。
  第三次           但当时把 r3 的"零读"读成了"数据全在内存、没碰盘", 那是错的。
                 引擎 bytes_read 10.30 GiB, 设备实际读 15.62 GiB, 差 5.32 GiB
                 正好一轮; 账本 t_wall 停在 r3 开始前 0.20s(uring_b 是 0.47s)。
                 真相是账本被截断:10s 的 dump 周期撞 21s 一轮的 revisit, r3 必然
                 落在最后一次 dump 之后, 而 shutdown 的终写在 SIGINT 关停下没跑。
                 修法见 manager.py 的 _flush_records —— 记录改成收割即 append。
                 教训: 账本里的"没有", 要和设备侧对账过才是"没发生"。

所以判据不是"加载在不在窗口内"(那题已答完), 而是 **IO 服务时间占 TTFT 的比例**。
占比低于 IO_BOUND_FRAC 时, 这个负载就不是 IO-bound 的, 拿它比 fs/uring 的 TTFT
等于在噪声里找信号; 要么换更重的 IO 负载, 要么承认端到端这层兑现不了引擎差异。

只有 uring 组能审(fs 走 vLLM 原生路径, 没有账本)。fs 与 uring 同负载同参数,
uring 侧的占比可当整批的量级参考, 但 fs 自己的 IO 时间仍需 fs 侧埋点才能证。

用法: python3 bench/e3_window_audit.py [results_e2e_目录]
"""
import json, glob, os, sys, statistics

RES = sys.argv[1] if len(sys.argv) > 1 else \
    "/Users/macbook/ai-infra-study/projects/project5_vllm_disk_tier/results_e2e_20260720_1629"

# IO 服务时间占 TTFT 低于此比例, 就判这批数据比不出引擎差异。0.15 是拍的:
# 就算 uring 把 IO 全部砍到 0, 端到端也只动 15% 以内, 而组间 serve 漂移
# (E1/E6 反复吃过的亏)本身就有百分之几, 信噪比不够看
IO_BOUND_FRAC = 0.15
# 读块落在窗口外的容忍上限。这一项现在只是前置卫生检查, 防止改了负载形状
# 之后加载又漂出计时窗口 —— 真漂了, 下面的占比数就没有意义了
WINDOW_FRAC = 0.5

for path in sorted(glob.glob(f"{RES}/tier_stats_*.json")):
    name = os.path.basename(path)[len("tier_stats_"):-len(".json")]
    gpath = f"{RES}/group_{name}.json"
    if not os.path.exists(gpath):
        print(f"{name}: 找不到 {os.path.basename(gpath)}, 跳过")
        continue
    st = json.load(open(path))
    jr = st["job_records"]
    if "rows" in jr:
        rows = jr["rows"]          # 旧格式:记录内嵌在 stats.json(会丢尾巴)
    else:
        # 新格式:记录在同名 .records.jsonl 里, stats 只留条数供对账
        rpath = f"{RES}/{os.path.basename(path)[:-len('.json')]}.records.jsonl"
        if not os.path.exists(rpath):
            print(f"{name}: stats 说有 {jr['n_rows']} 条记录, 但 "
                  f"{os.path.basename(rpath)} 没收到 —— 别用这组")
            continue
        rows = [json.loads(ln) for ln in open(rpath) if ln.strip()]
        if len(rows) < jr["n_rows"]:
            print(f"{name}: 记录只有 {len(rows)} 条, stats 说应有 "
                  f"{jr['n_rows']} 条 —— 账本残缺, 别用这组")
            continue
    s = json.load(open(gpath))["samples"]
    reads = [r for r in rows if not r[1]]          # is_write == False
    if not reads:
        print(f"{name}: 整组零读 job —— 前缀根本没从盘上拉回来过")
        continue
    t0 = min(r[3] for r in rows)                   # t_submit 最早的 job 当原点

    key = next((k for k in s if k.startswith("revisit")), None)
    if key is None:
        print(f"{name}: 没有 revisit 样本, 跳过")
        continue
    # 窗口 = [最早请求的发起时刻, 最晚请求的结束时刻]。t_wall 是发起时刻!
    win = {}
    for x in s[key]:
        w = win.setdefault(x["round"], [9e18, 0.0])
        w[0] = min(w[0], x["t_wall"])
        w[1] = max(w[1], x["t_wall"] + x["total"])

    print(f"=== {name} ===")
    inside, io_share = 0, []
    for r_no in sorted(win):
        lo, hi = win[r_no]
        rd = [r for r in reads if lo <= r[3] <= hi]
        inside += sum(r[2] for r in rd)
        ttft = statistics.mean(x["ttft"] * 1000 for x in s[key] if x["round"] == r_no)
        if rd:
            # 读 job 之间是并发的, 单个请求等的是自己那一个 job, 所以拿单 job
            # 的服务时间(不是求和)去比 TTFT; 取 max 是给引擎最有利的算法 ——
            # 连最慢的那个 job 都只占 TTFT 一点点, 结论才站得住
            svc = [(r[5] - r[4]) * 1000 for r in rd]
            frac = max(svc) / ttft
            io_share.append(frac)
            note = (f"IO 服务 mean={statistics.mean(svc):5.1f}ms max={max(svc):6.1f}ms"
                    f"  → 占 TTFT {frac:5.1%}")
        else:
            # 别急着读成"没碰盘"。E3D 两组的 r3 都是零读记录, 查下来是账本被
            # 截断(dump 周期撞实验节奏 + shutdown 终写没跑), 设备侧实打实读了
            # 5.32 GiB。真没读还是没记下来, 要拿设备侧读量对账才能分辨
            note = "零读记录 —— 先与 iostat 对账, 可能是账本截断而非真没读"
        print(f"  r{r_no} 窗口[{lo - t0:6.1f},{hi - t0:6.1f}]s  "
              f"读job={len(rd):3d} blocks={sum(r[2] for r in rd):6d}  "
              f"TTFT={ttft:7.1f}ms  {note}")

    total = sum(r[2] for r in reads)
    wfrac = inside / total if total else 0.0
    if wfrac < WINDOW_FRAC:
        out = [r for r in reads if not any(lo <= r[3] <= hi for lo, hi in win.values())]
        print(f"  ⚠ 窗口内读块仅 {inside}/{total} = {wfrac:.0%}, 加载漂出计时窗口, "
              f"下面的占比不可信")
        print("    窗口外读 job 时刻: " +
              " ".join(f"{r[3] - t0:.1f}s" for r in out[:12]) +
              (" ..." if len(out) > 12 else ""))
        continue
    if not io_share:
        print("  所有轮次零读 —— 这批负载压根没让 revisit 碰盘")
        continue
    peak = max(io_share)
    verdict = ("IO-bound, 可以比引擎" if peak >= IO_BOUND_FRAC else
               f"非 IO-bound: IO 最多只占 TTFT {peak:.1%}, "
               f"端到端 TTFT 比不出 fs/uring 的引擎差异")
    print(f"  窗口内读块 {inside}/{total} = {wfrac:.0%} | "
          f"各轮 IO 占比峰值 {peak:.1%}  → {verdict}")
