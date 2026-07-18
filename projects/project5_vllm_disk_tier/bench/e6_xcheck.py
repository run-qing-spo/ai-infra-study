#!/usr/bin/env python3
"""E6 作废条件核验:t_wall × iostat 窗口对齐。

问的是 E3/E4 同一个问题:窗口里前缀**真的从盘上回来了**吗?E6 与它们的不同是
没有分离的 revisit 相位——开环回放整条 trace,读写全程混在一起,所以窗口 = 整条
回放 [min(t_wall), max(t_wall+total)]。

答案是没有(2026-07-17):四档盘读 0.00/0.00/0.01/0.46~0.53 GB 对同期写 27.7GB,
tier 的 load 路径几乎没被激活,TTFT 不能归给读路径。见 COMPARE_PLAN E6 节核验段。

三条记账纪律(E3 的 1089MB/s 假封顶、E4 的"半热基线"、和下面第三条都是栽出来的):
  - iostat 第一份采样是**开机以来**的累计均值,必须丢,否则整轮被它带偏。
  - 1s 粒度对 2s 突发做窗口记账,±1 个采样点就是 ±50%。所以这里不只报窗口积分,
    还把每个"真读的秒"逐条打出来(--timeline),让原始时间线自己说话。
  - **均值会撒谎,回放是确定性的就用配对**。2026-07-17:拿组均值看 c=8, 得出
    "fs 947 vs uring 950, 打平甚至翻向"; 配对(同 idx = 同 prompt)一看, 中位数
    是 uring 快 19~29ms、150 条里赢 86~105 条 —— 均值被少数极端值拽翻了向。
    同 idx 同 prompt 是 trace 回放白送的配对结构, 不用白不用。--pair 报它。

双账交叉:窗口写量应当 ≈ SUMMARY 里 `kv_fs 落盘` 的哨兵值(证明窗口没切偏),
盘读量应当与 serve 日志的 `kv_offload_load_bytes` 同量级(差值 = 命中 CPU tier
不落盘的部分)。两本独立的账都对上,才排除得掉"是我对齐错了"。

写侧也报:2026-07-16 那轮 load 没激活,唯一被激活的 tier 路径反而是 store,于是
它意外成了一个纯 store 对照(E1/E3/E4 里 store 和 load 永远混在一起)。看 w/s 与
w_await:同样带宽下 fs 多打 45% 请求、写延迟高 40~60%,那是 tmp+rename 日志事务
在设备上的实体。

用法:  python3 bench/e6_xcheck.py results_e2e_20260716_0035 [--timeline] [--pair]
"""
import argparse
import datetime
import json
import re
import statistics
from pathlib import Path

TS_RE = re.compile(r"^(\d\d)/(\d\d)/(\d\d) (\d\d):(\d\d):(\d\d)$")
LOAD_RE = re.compile(r"kv_offload_load_bytes=([0-9.e+]+)")
EXT_RE = re.compile(r"External prefix cache hit rate: ([0-9.]+)%")
# 靠大小写区分:GPU 侧是 "Prefix cache hit rate"(大写 P), tier 侧是 "External
# prefix cache hit rate"(小写 p)。前面锚一个 ", " 免得匹配到 External 那句的尾巴。
GPU_RE = re.compile(r", Prefix cache hit rate: ([0-9.]+)%")

READ_FLOOR_KB = 1024        # 低于 1MB/s 的秒算"没在读",只是背景噪声


def parse_iostat(path: Path, dev: str = "md0", tz_offset_h: int = 8):
    """-> [{t(epoch), rkB, wkB, rareq, aqu, ...}],每点代表 (t-1, t] 这一秒。

    iostat 打的是容器本地墙钟(无时区标记),t_wall 是 epoch,所以要减时区偏移。
    偏移不硬猜:main() 里用"窗口必须落在 iostat 覆盖范围内"反查,对不上会喊。
    """
    out, ts = [], None
    for line in open(path):
        line = line.strip()
        m = TS_RE.match(line)
        if m:
            mo, da, yr, hh, mm, ss = map(int, m.groups())
            dt = datetime.datetime(2000 + yr, mo, da, hh, mm, ss)
            ts = (dt - datetime.timedelta(hours=tz_offset_h)).replace(
                tzinfo=datetime.timezone.utc).timestamp()
            continue
        if ts is None or not line.startswith(dev + " "):
            continue
        f = line.split()
        # 列序(错位一格就得出物理上不可能的数, 数完再写):
        #   0 Device   1 r/s   2 rkB/s   3 rrqm/s   4 %rrqm   5 r_await   6 rareq-sz
        #   7 w/s      8 wkB/s 9 wrqm/s 10 %wrqm   11 w_await 12 wareq-sz
        #  13 d/s     14 dkB/s 15 drqm/s 16 %drqm  17 d_await 18 dareq-sz
        #  19 f/s     20 f_await        21 aqu-sz  22 %util
        out.append(dict(t=ts, rs=float(f[1]), rkB=float(f[2]), r_await=float(f[5]),
                        rareq=float(f[6]), ws=float(f[7]), wkB=float(f[8]),
                        w_await=float(f[11]), wareq=float(f[12]),
                        aqu=float(f[21]), util=float(f[22])))
    return out[1:]          # 丢开机累计那一份


def load_group(res: Path, name: str):
    d = json.load(open(res / f"group_E6_{name}.json"))
    ss = [s for s in d["samples"] if s.get("ttft") is not None]
    return ss, min(s["t_wall"] for s in ss), max(s["t_wall"] + s["total"] for s in ss)


def serve_metrics(path: Path):
    """vLLM 自己的账:load 字节、External/GPU prefix cache 命中率。

    三个口径陷阱,都是踩出来的:
      - `kv_offload_load_*` 是 **tier 整体**(CPU primary + 磁盘 secondary)的合账,
        所以它 ≥ 盘读量;两者同量级即互证, 不该要求相等。
      - 日志里的 load/store bytes 是**区间增量**(每次 log 一行), 要 sum 才是全程;
        而 /metrics 端点上的同名指标是累计 counter —— 别把两个口径混着用。
      - prefix cache hit rate 是**累计**的(序列单调爬升), 所以只有末值可比。
        逐行看会掉进相位陷阱:2026-07-17 曾见 fs 首行 1.9% vs uring 29.2%, 像是
        天大的差异, 其实只是两组 serve 的 10s 采样节拍相位不同、首桶覆盖的请求
        数不同(fs 跨 9 个桶、uring 跨 8 个) —— 末值收敛到 45~47% 对 47.6%。
    """
    txt = open(path).read()
    ld = [float(x) for x in LOAD_RE.findall(txt)]
    ext = [float(x) for x in EXT_RE.findall(txt)]
    gpu = [float(x) for x in GPU_RE.findall(txt)]
    return dict(load_GB=sum(ld) / 2**30,          # 区间增量求和 = 全程
                ext_peak=max(ext) if ext else 0.0,
                gpu_last=gpu[-1] if gpu else 0.0)  # 累计口径, 只有末值可比


def pct(xs, p):
    xs = sorted(xs)
    if not xs:
        return 0.0
    k = (len(xs) - 1) * p / 100
    f = int(k)
    return xs[f] if f + 1 >= len(xs) else xs[f] + (xs[f + 1] - xs[f]) * (k - f)


def sign_test_p(k: int, n: int) -> float:
    """双尾符号检验的精确 p 值(二项 n,0.5)。标准库够用,不引 scipy。

    k = uring 更快的条数。H0:两 tier 无系统性差异 ⟹ k ~ B(n, 0.5)。
    """
    if n == 0:
        return 1.0
    from math import comb
    k = min(k, n - k)
    tail = sum(comb(n, i) for i in range(0, k + 1)) / 2**n
    return min(1.0, 2 * tail)


def paired_report(res: Path, c: int, reps=("a", "b", "c")):
    """配对比较:同 idx = 同 prompt(回放确定性), 是 trace 白送的配对结构。

    为什么不用组均值:见文件头第三条纪律。均值对极端值敏感, 而 TTFT 分布长尾
    (p99 是 p50 的 3~10 倍), 少数几条撞 GPU 排队的请求就能把均值拽翻向。
    配对的中位数 + 符号检验对长尾免疫, 且直接回答"是不是系统性的"。
    """
    def load(name):
        p = res / f"group_E6_{name}.json"
        if not p.exists():
            return None
        d = json.load(open(p))
        return {s["idx"]: s["ttft"] for s in d["samples"]
                if s.get("ttft") is not None}

    out = []
    for rf in reps:
        for ru in reps:
            f, u = load(f"c{c}_fs_{rf}"), load(f"c{c}_uring_{ru}")
            if not f or not u:
                continue
            common = sorted(set(f) & set(u))
            diff = [(u[i] - f[i]) * 1000 for i in common]      # <0 = uring 更快
            if not diff:
                continue
            k = sum(1 for d in diff if d < 0)
            out.append((f"fs_{rf} vs uring_{ru}", len(diff), k,
                        statistics.median(diff), statistics.mean(diff),
                        sign_test_p(k, len(diff))))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("results", type=Path)
    ap.add_argument("--timeline", action="store_true",
                    help="逐条打出每个真读的秒(原始时间线,别只信积分)")
    ap.add_argument("--pair", action="store_true",
                    help="配对比较 + 符号检验(同 idx 同 prompt;均值会撒谎,见文件头)")
    ap.add_argument("--tz-offset-h", type=int, default=8)
    ap.add_argument("--concurrency", type=int, nargs="+", default=[1, 4, 8, 16])
    args = ap.parse_args()

    rows = []
    for c in args.concurrency:
        for tier in ("fs", "uring"):
            for rep in ("a", "b", "c"):
                name = f"c{c}_{tier}_{rep}"
                if not (args.results / f"group_E6_{name}.json").exists():
                    continue
                ss, lo, hi = load_group(args.results, name)
                pts = parse_iostat(args.results / f"iostat_E6_{name}.log",
                                   tz_offset_h=args.tz_offset_h)
                if not (pts[0]["t"] <= lo and hi <= pts[-1]["t"] + 2):
                    print(f"!! {name}: 回放窗口没落在 iostat 覆盖内 —— 时区偏移 "
                          f"{args.tz_offset_h}h 可能不对,先修这个再看数")
                sel = [p for p in pts if lo - 1 < p["t"] <= hi + 1]
                busy = [p for p in sel if p["rkB"] > READ_FLOOR_KB]
                tt = [s["ttft"] for s in ss]
                m = serve_metrics(args.results / f"serve_E6_{name}.log")
                # 写侧在"写活跃的秒"上取均值:store 路径是 2026-07-16 那轮唯一
                # 被激活的 tier 路径,它的设备形态是那轮唯一站得住的机制证据。
                wbusy = [p for p in sel if p["wkB"] > READ_FLOOR_KB]
                wavg = (lambda k: statistics.mean([p[k] for p in wbusy])
                        if wbusy else 0.0)
                rows.append(dict(
                    c=c, tier=tier, rep=rep, n=len(ss), dur=hi - lo,
                    rGB=sum(p["rkB"] for p in sel) / 2**20,
                    wGB=sum(p["wkB"] for p in sel) / 2**20,
                    busy_s=len(busy),
                    rareq=statistics.mean([p["rareq"] for p in busy]) if busy else 0,
                    aqu=statistics.mean([p["aqu"] for p in busy]) if busy else 0,
                    ws=wavg("ws"), w_await=wavg("w_await"), waqu=wavg("aqu"),
                    mean=statistics.mean(tt) * 1000, p50=pct(tt, 50) * 1000,
                    p99=pct(tt, 99) * 1000, **m))
                if args.timeline and busy:
                    print(f"-- {name} 真读的秒(窗口 {hi-lo:.0f}s):")
                    for p in busy:
                        print(f"     t+{p['t']-lo:5.1f}s  {p['rkB']/1024:6.0f}MB/s"
                              f"  rareq {p['rareq']:6.1f}KB  aqu {p['aqu']:5.1f}")

    print(f"\n{'组':<16}{'n':>4}{'窗口s':>7}{'盘读GB':>8}{'盘写GB':>8}{'读秒':>5}"
          f"{'rareq':>7}{'aqu':>6}{'vLLM读GB':>9}{'Ext%':>6}{'GPU%':>6}"
          f"{'mean':>8}{'p99':>8}")
    for r in rows:
        print(f"c{r['c']}_{r['tier']}_{r['rep']:<10}{r['n']:>4}{r['dur']:>7.1f}"
              f"{r['rGB']:>8.2f}{r['wGB']:>8.2f}{r['busy_s']:>5}{r['rareq']:>7.1f}"
              f"{r['aqu']:>6.1f}{r['load_GB']:>9.2f}{r['ext_peak']:>6.1f}"
              f"{r['gpu_last']:>6.1f}{r['mean']:>8.1f}{r['p99']:>8.1f}")

    print("\n=== store 路径的设备形态(写活跃的秒上取均值)===")
    print("load 未激活时,这是唯一被激活的 tier 路径 —— 同带宽下比 w/s 和 w_await")
    print(f"{'档':<12}{'w/s':>7}{'wkB/s':>9}{'w_await(ms)':>13}{'aqu-sz':>8}")
    for c in args.concurrency:
        for tier in ("fs", "uring"):
            rs = [r for r in rows if r["c"] == c and r["tier"] == tier]
            if not rs:
                continue
            print(f"c={c:<3}{tier:<8}{statistics.mean([r['ws'] for r in rs]):7.0f}"
                  f"{statistics.mean([r['wGB']*1024/max(r['dur'],1) for r in rs]):9.0f}"
                  f"{statistics.mean([r['w_await'] for r in rs]):13.2f}"
                  f"{statistics.mean([r['waqu'] for r in rs]):8.2f}")

    print("\n=== 合并同档 ===")
    for c in args.concurrency:
        for tier in ("fs", "uring"):
            rs = [r for r in rows if r["c"] == c and r["tier"] == tier]
            if not rs:
                continue
            print(f"c={c:<3}{tier:<6} 盘读 {statistics.mean([r['rGB'] for r in rs]):5.2f}GB"
                  f"  盘写 {statistics.mean([r['wGB'] for r in rs]):6.2f}GB"
                  f"  vLLM load {statistics.mean([r['load_GB'] for r in rs]):5.2f}GB"
                  f"  mean {statistics.mean([r['mean'] for r in rs]):7.1f}ms"
                  f"  p50 {statistics.mean([r['p50'] for r in rs]):7.1f}ms"
                  f"  p99 {statistics.mean([r['p99'] for r in rs]):7.1f}ms")

    if args.pair:
        print("\n=== 配对比较 + 符号检验(同 idx = 同 prompt)===")
        print("均值对长尾敏感会撒谎(见文件头纪律三);配对中位数与符号检验才是口径。")
        for c in args.concurrency:
            rep = paired_report(args.results, c)
            if not rep:
                continue
            print(f"-- c={c}")
            for name, n, k, med, mean, p in rep:
                flag = "系统性" if p < 0.01 else ("弱" if p < 0.05 else "不显著")
                print(f"   {name:22} uring 更快 {k:3}/{n:<3} ({k/n*100:3.0f}%)"
                      f"  中位差 {med:8.1f}ms  均值差 {mean:8.1f}ms"
                      f"  p={p:.2g} {flag}")

    print("\n作废条件:盘读量对不上工作集的档位作废(E3 §51 立)。E6 实测四档全废——"
          "盘读 0~0.5GB 对盘写 27.7GB,load 路径未激活,TTFT 不归读路径。")
    print("但差异本身是系统性的(--pair 看):它只可能来自 store 路径,机制未查明,"
          "记悬案。见 COMPARE_PLAN E6 节。")


if __name__ == "__main__":
    main()
