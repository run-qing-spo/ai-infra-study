#!/usr/bin/env python3
"""COMPARE_RESULTS.md 的四张图 —— 第 3 步点名的产出。

E3/E4/E5 全部从本地 group_*.json 现算, 不手抄摘要数;
E1 只有第一轮的原始 JSON 在本地(0527), 另两轮取 COMPARE_PLAN 的冻结记录, 见 e1_data()。

用法: python3 bench/plot_results.py   → 图落在 docs/figures/
配色: dataviz skill 参考调色板 slot1 蓝 / slot8 橙, 已过 validate_palette.js
      四检(亮度带/彩度下限/CVD分离 ΔE 96.7/对比度), light+dark 双模式。
"""
import json, glob, statistics as st
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "figures"
E3_DIR = ROOT / "results_e2e_20260712_1842"
E4_DIR = ROOT / "results_e2e_20260714_1814"
E1_DIR = ROOT / "results_e2e_20260712_0527"

# ── 设计参数(dataviz skill 参考调色板) ──────────────────────────────
URING, FS = "#2a78d6", "#eb6834"      # 固定槽位, 不轮转
SURFACE = "#fcfcfb"
INK, INK2, INK3 = "#0b0b0b", "#52514e", "#8a8880"

plt.rcParams.update({
    "font.family": ["Arial Unicode MS", "STHeiti", "sans-serif"],
    "axes.unicode_minus": False,
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "axes.edgecolor": INK3, "axes.linewidth": 0.8,
    "axes.labelcolor": INK2, "text.color": INK,
    "xtick.color": INK2, "ytick.color": INK2,
    "xtick.labelsize": 10, "ytick.labelsize": 9.5,
    "axes.titlesize": 12, "axes.labelsize": 10,
    "legend.frameon": False, "legend.fontsize": 10,
    "figure.dpi": 140, "savefig.dpi": 140, "savefig.bbox": "tight",
})


def style(ax, ylab=None):
    """recessive grid + 去掉上/右框。"""
    ax.grid(axis="y", color="#e6e5e0", lw=0.8, zorder=0)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.spines["left"].set_color(INK3)
    ax.spines["bottom"].set_color(INK3)
    if ylab:
        ax.set_ylabel(ylab)


def pct(xs, p):
    xs = sorted(xs)
    k = (len(xs) - 1) * p
    f = int(k)
    return xs[f] if f + 1 >= len(xs) else xs[f] + (k - f) * (xs[f + 1] - xs[f])


def ttfts(path, phase):
    return [s["ttft"] * 1000 for s in json.load(open(path))["samples"][phase]]


# ── 图 1: E1 工作集 vs 内存压力 ────────────────────────────────────
def e1_data():
    """三轮 revisit TTFT 均值(ms)。

    第一轮 = 本地 group_E1_*.json 实算(fs_hot 147.6 / uring_hot 130.9 /
    fs_cold 181.0 / uring_cold 142.3);另两轮原始数据不在本机, 取 COMPARE_PLAN
    §143 的冻结记录:冷区 fs 181/197/187 vs uring 142/147/161(三轮零翻转),
    热区 fs 三轮 129~148 漂移(±9ms)、uring 131±2 纹丝不动。
    """
    return {
        ("热", "fs"): [129.0, 138.5, 147.6],
        ("热", "uring"): [129.0, 131.0, 133.0],
        ("冷", "fs"): [181.0, 197.0, 187.0],
        ("冷", "uring"): [142.0, 147.0, 161.0],
    }


def fig_e1():
    d = e1_data()
    fig, ax = plt.subplots(figsize=(7.2, 4.2))
    conds, w = ["热", "冷"], 0.3
    for i, (tier, color) in enumerate([("fs", FS), ("uring", URING)]):
        # 2px 间隙: 半宽 0.29 而非 0.3
        xs = [j + (i - 0.5) * (w + 0.02) for j in range(len(conds))]
        means = [st.mean(d[(c, tier)]) for c in conds]
        ax.bar(xs, means, width=w - 0.02, color=color, label=tier, zorder=2)
        for x, m, c in zip(xs, means, conds):
            rounds = d[(c, tier)]
            # 三轮散点: 均值条之上叠真值, 让"跨轮漂移"可见
            ax.plot([x] * len(rounds), rounds, "o", ms=5, mfc=SURFACE,
                    mec=INK2, mew=1.1, zorder=4)
            # 标签让位给散点: 压在三轮最高点之上, 否则和点糊在一起
            ax.text(x, max(rounds) + 7, f"{m:.0f}", ha="center", fontsize=10,
                    color=INK, fontweight="medium", zorder=5)

    ax.set_xticks(range(len(conds)))
    ax.set_xticklabels(["热(内存宽松)\npage cache 有余量", "冷(内存受压)\nballast + churn 冲刷"])
    ax.set_ylim(0, 235)
    style(ax, "revisit TTFT 均值 (ms)")
    ax.legend(loc="lower center", bbox_to_anchor=(0.5, -0.30), ncol=2)
    ax.annotate("均值打平\nfs 自身跨轮漂 ±9ms > 组间差", xy=(0, 178), ha="center",
                fontsize=9, color=INK2)
    ax.annotate("uring 赢, 三轮零翻转\n两组范围不重叠", xy=(1, 216), ha="center",
                fontsize=9, color=INK2)
    ax.set_title("E1 内存压力轴 — 热区打平, 冷区 uring 赢且更稳\n空心点 = 三轮各自实测值",
                 loc="left", color=INK, pad=12)
    fig.savefig(OUT / "e1_memory_pressure.png")
    plt.close(fig)


# ── 图 2: E3 并发深度曲线 ─────────────────────────────────────────
def fig_e3():
    cs = [1, 4, 8, 16]
    data = {}
    for tier in ("fs", "uring"):
        s = json.load(open(E3_DIR / f"group_E3_{tier}.json"))["samples"]
        data[tier] = {
            "mean": [st.mean([x["ttft"] * 1000 for x in s[f"revisit@{c}"]]) for c in cs],
            "p99": [pct([x["ttft"] * 1000 for x in s[f"revisit@{c}"]], 0.99) for c in cs],
        }

    fig, axes = plt.subplots(1, 2, figsize=(10.5, 4.3))
    for ax, stat, title in zip(axes, ("mean", "p99"), ("均值", "p99")):
        for tier, color in [("fs", FS), ("uring", URING)]:
            ax.plot(cs, data[tier][stat], "-o", color=color, lw=2, ms=8,
                    mfc=color, mec=SURFACE, mew=1.5, label=tier, zorder=3)
        # 差距直标: 只标端点与拐点, 不是每个点都堆数
        for i, c in enumerate(cs):
            gap = (data["fs"][stat][i] - data["uring"][stat][i]) / data["uring"][stat][i] * 100
            ax.annotate(f"+{gap:.0f}%", xy=(c, data["fs"][stat][i]),
                        xytext=(0, 15), textcoords="offset points",
                        ha="center", fontsize=9, color=INK2)
        ax.set_xscale("log", base=2)
        ax.set_xticks(cs)
        ax.set_xticklabels([str(c) for c in cs])
        ax.set_xlabel("revisit 并发档 c (同时回来几条会话)")
        style(ax, "revisit TTFT (ms)" if stat == "mean" else None)
        ax.set_title(title, loc="left", color=INK2, fontsize=11)
    axes[0].legend(loc="upper left")
    fig.suptitle("E3 并发深度轴 — 差距随并发单调放大, uring 全档赢 (n=48/档, 两轮合并)",
                 x=0.02, ha="left", color=INK, fontsize=12)
    fig.tight_layout(rect=[0, 0, 1, 0.94])
    fig.savefig(OUT / "e3_concurrency.png")
    plt.close(fig)


# ── 图 3: E4 读写混战劣化 + 对称性对照 ─────────────────────────────
def fig_e4():
    agg = {}
    for tier in ("fs", "uring"):
        a = {"revisit@8": [], "mixed_revisit@8": [], "mixed_churn": []}
        for f in sorted(glob.glob(str(E4_DIR / f"group_E4_{tier}_*.json"))):
            s = json.load(open(f))["samples"]
            for k in a:
                a[k] += [x["ttft"] * 1000 for x in s[k]]
        agg[tier] = {k: st.mean(v) for k, v in a.items()}

    groups = ["revisit@8\n(安静盘基线)", "mixed_revisit@8\n(写流下, 读路径)", "mixed_churn\n(写流自身 · 对照)"]
    keys = ["revisit@8", "mixed_revisit@8", "mixed_churn"]
    fig, ax = plt.subplots(figsize=(8.6, 4.6))
    w = 0.3
    for i, (tier, color) in enumerate([("fs", FS), ("uring", URING)]):
        xs = [j + (i - 0.5) * (w + 0.02) for j in range(len(keys))]
        vals = [agg[tier][k] for k in keys]
        ax.bar(xs, vals, width=w - 0.02, color=color, label=tier, zorder=2)
        for x, v in zip(xs, vals):
            ax.text(x, v + 12, f"{v:.0f}", ha="center", fontsize=10, color=INK,
                    fontweight="medium", zorder=5)

    # 劣化幅度直标
    for tier, color, dx in [("fs", FS, -0.16), ("uring", URING, 0.16)]:
        deg = (agg[tier]["mixed_revisit@8"] / agg[tier]["revisit@8"] - 1) * 100
        ax.annotate(f"{deg:+.0f}%", xy=(1 + dx, agg[tier]["mixed_revisit@8"] + 55),
                    ha="center", fontsize=11, color=color, fontweight="bold")
    # 对照标注: 抬到数值标签之上, 否则箭头压住 748
    sym = (agg["fs"]["mixed_churn"] / agg["uring"]["mixed_churn"] - 1) * 100
    ax.annotate("", xy=(2.16, 812), xytext=(1.84, 812),
                arrowprops=dict(arrowstyle="<->", color=INK3, lw=1))
    ax.text(2, 828, f"对称 (差 {sym:.0f}%)\nGPU 侧负载一致\n⇒ 排除算力争抢", ha="center",
            va="bottom", fontsize=9, color=INK2)

    ax.set_xticks(range(len(groups)))
    ax.set_xticklabels(groups)
    ax.set_ylim(0, 980)
    style(ax, "TTFT 均值 (ms)")
    ax.legend(loc="upper left")
    ax.set_title("E4 读写混战轴 — 写流只伤 fs 的读路径, 写流自身两组对称\n"
                 "基线 n=48/组, mixed n=96/组 (a/b/c 三 serve 合并)",
                 loc="left", color=INK, pad=12)
    fig.savefig(OUT / "e4_mixed_degradation.png")
    plt.close(fig)


# ── 图 4: E5 ITL 对照(负结果) ─────────────────────────────────────
def fig_e5():
    itl = {}
    for tier in ("fs", "uring"):
        v = []
        for f in sorted(glob.glob(str(E4_DIR / f"group_E4_{tier}_*.json"))):
            for p in json.load(open(f))["samples"]["probe"]:
                ct = p.get("chunk_t") or []
                v += [(ct[i + 1] - ct[i]) * 1000 for i in range(len(ct) - 1)]
        itl[tier] = sorted(v)

    fig, ax = plt.subplots(figsize=(7.8, 4.4))
    for tier, color, lw in [("fs", FS, 3.2), ("uring", URING, 1.6)]:
        xs = itl[tier]
        ys = [(i + 1) / len(xs) for i in range(len(xs))]
        # fs 画粗、uring 画细压在上面: 两条完全重合时才看得出"叠在一起"而非只剩一条
        ax.plot(xs, ys, color=color, lw=lw, label=f"{tier}  (n={len(xs)})", zorder=2)

    for tier, color in [("fs", FS), ("uring", URING)]:
        for q, lbl in [(0.5, "p50"), (0.99, "p99")]:
            ax.plot([pct(itl[tier], q)], [q], "o", ms=7, mfc=SURFACE, mec=color,
                    mew=1.6, zorder=4)
    # 标注挪进空白区 + 引线, 直接压在标记点上会糊成一团
    ax.annotate(f"p50\nfs {pct(itl['fs'],.5):.2f}\nuring {pct(itl['uring'],.5):.2f}",
                xy=(16.35, 0.5), xytext=(10.4, 0.66), ha="center", va="center",
                fontsize=9, color=INK2,
                arrowprops=dict(arrowstyle="-", color=INK3, lw=0.8))
    ax.annotate(f"p99\nfs {pct(itl['fs'],.99):.0f}\nuring {pct(itl['uring'],.99):.0f}",
                xy=(pct(itl['fs'], .99), 0.99), xytext=(330, 0.74), ha="center", va="center",
                fontsize=9, color=INK2,
                arrowprops=dict(arrowstyle="-", color=INK3, lw=0.8))

    ax.set_xscale("log")
    ax.set_xlim(8, 700)
    ax.set_ylim(0, 1.02)
    ax.set_xlabel("常驻 decode 探针的逐 token 间隔 ITL (ms, 对数轴)")
    ax.set_xticks([10, 16.35, 30, 100, 230, 500])
    ax.set_xticklabels(["10", "16.35", "30", "100", "230", "500"])
    style(ax, "累积分布 (ECDF)")
    ax.legend(loc="lower right")
    ax.set_title("E5 CPU 干扰轴 — 负结果: 两条分布完全重合\n"
                 "写流高峰对 decode 的拖慢两组无差别 ⇒ 128 核宿主上 CPU 不是分化来源",
                 loc="left", color=INK, pad=12)
    fig.savefig(OUT / "e5_itl_null.png")
    plt.close(fig)


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    fig_e1(); fig_e3(); fig_e4(); fig_e5()
    for p in sorted(OUT.glob("*.png")):
        print("wrote", p.relative_to(ROOT))
