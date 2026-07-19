#!/usr/bin/env python3
"""E4 相位重建: 每个 mixed_revisit 批到达时, 在飞 churn 处于什么相位、剩多少 prefill,
与批均值 TTFT 摆在一起 —— 检验"排队外显"是线性加法还是被 load 重叠掩盖的阈值形状。"""
import json, glob, os

RES = "/Users/macbook/ai-infra-study/projects/project5_vllm_disk_tier/results_e2e_20260714_1814"

rows = []
for path in sorted(glob.glob(f"{RES}/group_E4_*.json")):
    name = os.path.basename(path)[len("group_E4_"):-len(".json")]  # fs_a, uring_b, ...
    d = json.load(open(path))["samples"]
    churn = sorted(d["mixed_churn"], key=lambda s: s["t_wall"])
    for wave in sorted({s["round"] for s in d["mixed_revisit@8"]}):
        ss = sorted((s for s in d["mixed_revisit@8"] if s["round"] == wave),
                    key=lambda s: s["t_wall"])
        for bi, batch in enumerate((ss[:8], ss[8:]), 1):
            t_arr = min(s["t_wall"] for s in batch)
            phase, remain = "idle", 0.0
            for c in churn:
                if c["t_wall"] <= t_arr < c["t_wall"] + c["total"]:
                    end_prefill = c["t_wall"] + c["ttft"]
                    if t_arr < end_prefill:
                        phase, remain = "prefill", (end_prefill - t_arr) * 1000
                    else:
                        phase = "decode"
                    break
            mean = sum(s["ttft"] for s in batch) / len(batch) * 1000
            rows.append((name, wave, bi, phase, remain, mean))

print(f"{'group':<9}{'wave':>5}{'batch':>6}  {'churn相位':<8}{'剩余prefill':>11}  {'批均值TTFT':>10}")
for name, wave, bi, phase, remain, mean in rows:
    r = f"{remain:8.0f}ms" if phase == "prefill" else f"{'—':>9}"
    print(f"{name:<9}{wave:>5}{bi:>6}  {phase:<8}{r:>11}  {mean:>8.1f}ms")

print("\n-- decode/idle 相位批的均值(本征), 按 tier --")
for tier in ("fs", "uring"):
    vals = [m for n, w, b, p, r, m in rows if n.startswith(tier) and p != "prefill"]
    pre = [(r, m) for n, w, b, p, r, m in rows if n.startswith(tier) and p == "prefill"]
    print(f"{tier}: decode/idle 批 n={len(vals)} 均值 {sum(vals)/len(vals):.0f}ms | "
          f"prefill 批 n={len(pre)}: " +
          ", ".join(f"(剩{r:.0f}→{m:.0f})" for r, m in sorted(pre)))
