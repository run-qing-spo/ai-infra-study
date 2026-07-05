#!/usr/bin/env python3
# 端到端 TTFT 基准:长上下文多会话 + 前缀复用, 打的是 disk tier 的核心收益 ——
# "被逐出的长前缀第二次来时, 从盘上拉回 vs 整段重算 prefill"。
#
# 负载形状(对应 project4 kv_trace_gen 的 prefix pool 思路, 换成真 vLLM):
#   phase 1  prime   : S 个会话各发一次 (长前缀 + 短问题), 灌满各层 cache
#   phase 2  churn   : F 个一次性长请求把 GPU / CPU tier 冲掉,
#                      迫使 prime 的前缀沉到磁盘(或被丢掉, 取决于配置组)
#   phase 3  revisit : 每个会话再发 (同一长前缀 + 新问题), 量 TTFT
#
# 三组配置对比同一脚本、同一负载:
#   A 无 offload         → revisit TTFT = 全量 prefill 重算(下界基线)
#   B CPU tier only      → 前缀被 churn 挤出 CPU 后仍要重算
#   C CPU + 磁盘 tier    → 前缀从盘上拉回, TTFT 应显著低于 A/B
# C 组内再分 fs / uring 两种 tier, 就是本项目的 A/B 对比。
#
# 输出:每 phase 的 TTFT mean/p50/p99 + 总时长;--json 给原始样本。
#
# 依赖: pip install openai
# 前提: vllm serve 已按 docs/RUN_ON_GPU.md 起好, --port 8000

import argparse
import json
import random
import statistics
import time

from openai import OpenAI

# 用固定词表拼可复现的"伪文档"。词≈token 的换算按 1 词 ≈ 1.3 token 粗估,
# 精确 token 数不重要 —— 三组配置吃到的是同一批 prompt, 对比是公平的。
WORDS = ("system cache tensor kernel stream block prefix decode layer batch "
         "memory transfer schedule request token attention weight buffer "
         "queue submit reap align direct storage evict promote tier").split()


def make_text(rng: random.Random, n_words: int) -> str:
    return " ".join(rng.choice(WORDS) for _ in range(n_words))


def one_request(client, model: str, prompt: str, max_tokens: int) -> dict:
    """流式发一次补全, 掐第一个 token 的表。"""
    t0 = time.perf_counter()
    ttft = None
    n_chunks = 0
    stream = client.completions.create(
        model=model, prompt=prompt, max_tokens=max_tokens,
        temperature=0.0, stream=True,
    )
    for chunk in stream:
        if chunk.choices and chunk.choices[0].text:
            if ttft is None:
                ttft = time.perf_counter() - t0
            n_chunks += 1
    return dict(ttft=ttft, total=time.perf_counter() - t0, chunks=n_chunks)


def pct(xs, p):
    return statistics.quantiles(xs, n=100)[p - 1] if len(xs) > 1 else xs[0]


def report(name: str, samples: list[dict]):
    ttfts = [s["ttft"] for s in samples if s["ttft"] is not None]
    if not ttfts:
        print(f"{name}: no samples")
        return
    print(f"{name:10s}  n={len(ttfts):3d}  "
          f"ttft mean={statistics.mean(ttfts)*1000:8.1f}ms  "
          f"p50={pct(ttfts, 50)*1000:8.1f}ms  "
          f"p99={pct(ttfts, 99)*1000:8.1f}ms")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-url", default="http://localhost:8000/v1")
    ap.add_argument("--model", default="Qwen/Qwen2.5-7B-Instruct")
    ap.add_argument("--sessions", type=int, default=16,
                    help="长前缀会话数。sessions × prefix 要 > CPU tier 容量才踩得到盘")
    ap.add_argument("--prefix-words", type=int, default=6000,
                    help="每会话前缀词数(~8k token, 4090+7B 下单会话 KV ≈ GB 级/8k)")
    ap.add_argument("--churn", type=int, default=24,
                    help="phase2 一次性请求数, 用来把 prime 的前缀挤下层")
    ap.add_argument("--churn-words", type=int, default=6000)
    ap.add_argument("--gen-tokens", type=int, default=32,
                    help="每请求生成 token 数;小值让 TTFT 主导, decode 干扰最小")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--json", default=None, help="原始样本落到这个文件")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    client = OpenAI(base_url=args.base_url, api_key="EMPTY")

    prefixes = [make_text(rng, args.prefix_words) for _ in range(args.sessions)]
    out = {}

    print(f"phase 1: prime {args.sessions} sessions "
          f"({args.prefix_words} words each)")
    out["prime"] = [
        one_request(client, args.model,
                    p + f"\n\nQuestion 1: summarize keyword #{i}. Answer:",
                    args.gen_tokens)
        for i, p in enumerate(prefixes)
    ]
    report("prime", out["prime"])

    print(f"phase 2: churn with {args.churn} one-shot requests")
    out["churn"] = [
        one_request(client, args.model,
                    make_text(rng, args.churn_words) + "\n\nSummarize. Answer:",
                    args.gen_tokens)
        for _ in range(args.churn)
    ]
    report("churn", out["churn"])

    print("phase 3: revisit sessions (this is the number that matters)")
    out["revisit"] = [
        one_request(client, args.model,
                    p + f"\n\nQuestion 2: now count keyword #{i}. Answer:",
                    args.gen_tokens)
        for i, p in enumerate(prefixes)
    ]
    report("revisit", out["revisit"])

    # revisit vs prime 的 TTFT 比值:1.0 = 前缀全部命中(理想),
    # ≈ prime 水平 = 全部重算(offload 没起作用或被逐光)
    prime_m = statistics.mean(s["ttft"] for s in out["prime"])
    rev_m = statistics.mean(s["ttft"] for s in out["revisit"])
    print(f"\nrevisit/prime TTFT ratio: {rev_m / prime_m:.2f} "
          f"(越低越好;1 附近说明前缀基本没保住)")

    if args.json:
        with open(args.json, "w") as f:
            json.dump(dict(config=vars(args), samples=out), f)
        print(f"raw samples -> {args.json}")


if __name__ == "__main__":
    main()
