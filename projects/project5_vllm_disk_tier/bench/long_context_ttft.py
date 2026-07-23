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
import os
import random
import statistics
import threading
import time
import uuid
from concurrent.futures import ThreadPoolExecutor

from openai import OpenAI
from kv_uring_tier.request_ids import make_req_id

# 用固定词表拼可复现的"伪文档"。词≈token 的换算按 1 词 ≈ 1.3 token 粗估,
# 精确 token 数不重要 —— 三组配置吃到的是同一批 prompt, 对比是公平的。
WORDS = ("system cache tensor kernel stream block prefix decode layer batch "
         "memory transfer schedule request token attention weight buffer "
         "queue submit reap align direct storage evict promote tier").split()


def make_text(rng: random.Random, n_words: int) -> str:
    return " ".join(rng.choice(WORDS) for _ in range(n_words))


def one_request(client, model: str, prompt: str, max_tokens: int,
                keep_chunk_times: bool = False,
                request_id: str = None) -> dict:
    """流式发一次补全, 掐第一个 token 的表。"""
    # formal runner 总会传确定性 ID;随机 fallback 只给手工调用保底。
    request_id = request_id or ("kvt-" + uuid.uuid4().hex)
    t_wall = time.time()      # 绝对时间戳:事后把样本对到 iostat/meminfo 的时间轴上
    t_start_ns = time.monotonic_ns()
    t0 = time.perf_counter()
    ttft = None
    t_first_ns = None
    n_chunks = 0
    response_id = None
    chunk_t = [] if keep_chunk_times else None   # 逐 token 绝对到达时刻(ITL 探针用)
    stream = client.completions.create(
        model=model, prompt=prompt, max_tokens=max_tokens,
        temperature=0.0, stream=True,
        # vLLM 0.24.0 的 _base_request_id() 明确优先读取 X-Request-Id。
        # 走 header 不依赖 OpenAI SDK 是否把 vLLM 扩展字段列进请求模型。
        extra_headers={"X-Request-Id": request_id},
    )
    for chunk in stream:
        chunk_response_id = getattr(chunk, "id", None)
        if chunk_response_id is not None:
            if response_id is None:
                response_id = chunk_response_id
            elif chunk_response_id != response_id:
                raise RuntimeError(
                    f"one request returned multiple response ids: "
                    f"{response_id!r} vs {chunk_response_id!r}"
                )
        if chunk.choices and chunk.choices[0].text:
            if ttft is None:
                ttft = time.perf_counter() - t0
                t_first_ns = time.monotonic_ns()
            n_chunks += 1
            if chunk_t is not None:
                chunk_t.append(time.time())
    t_done_ns = time.monotonic_ns()
    d = dict(req_id=request_id, response_id=response_id,
             ttft=ttft, total=time.perf_counter() - t0, chunks=n_chunks,
             t_wall=t_wall, client_start_mono_ns=t_start_ns,
             client_first_chunk_mono_ns=t_first_ns,
             client_done_mono_ns=t_done_ns)
    if chunk_t is not None:
        d["chunk_t"] = chunk_t
    return d


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
    ap.add_argument("--run-id", default=os.environ.get("KVTIER_RUN_ID", ""),
                    help="本次 serve lifecycle 的唯一 ID;进入每条客户端样本和"
                         "服务端账本,正式 runner 会自动设置")
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
    ap.add_argument("--revisit-concurrency", type=int, default=1,
                    help="revisit 阶段的并发请求数(E3 的并发轴), 1 = 现有串行行为")
    ap.add_argument("--phases", default="prime,churn,revisit",
                    help="逗号分隔的 phase 序列, 可重复出现构成循环, 例如 E3 的 "
                         "prime,churn,revisit@1,churn,revisit@4,...;"
                         "revisit@N 覆盖该轮的并发数。默认与旧行为一致。"
                         "mixed@N 是 E4 的读写混战阶段:churn 流不停, 按固定节拍"
                         "插 revisit 波(见 --mixed-*), 期间可选常驻 decode 探针量 ITL")
    ap.add_argument("--mixed-interval", type=float, default=20.0,
                    help="mixed 阶段两个 revisit 波的起点间隔秒数。间隔内 churn 流"
                         "持续把上一波拉回来的前缀重新挤下盘;默认 20s ≈ E3 一轮"
                         "完整 churn(24 条)的挤出量, 短了下一波会命中 GPU/CPU tier")
    ap.add_argument("--mixed-waves", type=int, default=6,
                    help="mixed 阶段插入的 revisit 波数, 每波 = 全部会话 @ 并发 c"
                         "(与 E3 的 revisit@c 同形状, 劣化对比才同基线)")
    ap.add_argument("--probe-tokens", type=int, default=256,
                    help="mixed 阶段常驻 decode 探针每请求生成的 token 数, 0 关探针。"
                         "探针量的是 ITL 在 IO 高峰被压高多少(E5 的端到端证据)")
    ap.add_argument("--pre-revisit-stat-dir", default=None,
                    help="每次 revisit 前对该目录全量 stat(dentry/inode 预热)。"
                         "E3 dentry 预热对照专用, 只动元数据缓存温度这一个变量;"
                         "对照组不带本参数(COMPARE_PLAN §55 封顶实验)")
    args = ap.parse_args()
    if not args.run_id:
        args.run_id = f"manual-{time.time_ns()}"

    # 提前把 --phases 校验完, 别跑了半宿在中途炸
    phases = [t.strip() for t in args.phases.split(",") if t.strip()]
    for t in phases:
        base, _, conc = t.partition("@")
        if base not in ("prime", "churn", "revisit", "mixed") \
                or (conc and base not in ("revisit", "mixed")):
            ap.error(f"--phases 里不认识的 token: {t}")
        if conc and not conc.isdigit():
            ap.error(f"--phases 并发数不是整数: {t}")

    rng = random.Random(args.seed)
    client = OpenAI(base_url=args.base_url, api_key="EMPTY")

    prefixes = [make_text(rng, args.prefix_words) for _ in range(args.sessions)]
    # 同名 phase 多次出现时样本合并到同一个 key(E3 每档 3 轮取合并样本),
    # 逐样本的 round 字段保留轮次信息, t_wall 保留绝对时间
    out = {}

    def run_and_report(key: str, round_no: int, samples: list[dict]):
        for s in samples:
            s["round"] = round_no
            s["phase"] = key
            s["run_id"] = args.run_id
        out.setdefault(key, []).extend(samples)
        report(key, samples)

    rounds = {}       # 每个 token 出现到第几轮
    q_idx = 1         # revisit 每轮换新问题编号:前缀相同、后缀不同,
                      # 否则整条 prompt(含问题)与上一轮完全一致,
                      # prefix cache 直接整条命中, 量不到 tier 的 load
    for step, tok in enumerate(phases, 1):
        rounds[tok] = rnd = rounds.get(tok, 0) + 1
        base, _, conc = tok.partition("@")

        if base == "prime":
            print(f"phase {step}: prime {args.sessions} sessions "
                  f"({args.prefix_words} words each)")
            run_and_report(tok, rnd, [
                one_request(client, args.model,
                            p + f"\n\nQuestion 1: summarize keyword #{i}. Answer:",
                            args.gen_tokens,
                            request_id=make_req_id(
                                args.run_id, f"prime:{rnd}:session:{i}"))
                for i, p in enumerate(prefixes)
            ])

        elif base == "churn":
            # 每轮都用新生成的文本:重复文本会命中 prefix cache, 挤不动下层
            print(f"phase {step}: churn with {args.churn} one-shot requests "
                  f"(round {rnd})")
            run_and_report(tok, rnd, [
                one_request(client, args.model,
                            make_text(rng, args.churn_words) + "\n\nSummarize. Answer:",
                            args.gen_tokens,
                            request_id=make_req_id(
                                args.run_id, f"churn:{rnd}:request:{i}"))
                for i in range(args.churn)
            ])

        elif base == "revisit":
            c = int(conc) if conc else args.revisit_concurrency
            q_idx += 1
            # dentry 预热对照(COMPARE_PLAN §55): revisit 计时窗口之外把目标
            # 目录全量 stat 扫热。O_DIRECT 下数据页不进 page cache, 这一扫只动
            # 元数据温度这一个变量;每轮 churn 都新增几千文件冲刷缓存, 所以
            # 每次 revisit 前都要重扫。行首前缀凑 sumpat 的 ^revisit, 进 SUMMARY。
            if args.pre_revisit_stat_dir:
                t0 = time.time()
                n_f = 0
                for dirpath, _dirs, files in os.walk(args.pre_revisit_stat_dir):
                    for fn in files:
                        try:
                            os.stat(os.path.join(dirpath, fn))
                            n_f += 1
                        except OSError:
                            pass  # 正撞上 tmp+rename 替换窗口, 跳过
                print(f"revisit-prewarm: stat {n_f} files in "
                      f"{time.time() - t0:.1f}s ({args.pre_revisit_stat_dir})")
            print(f"phase {step}: revisit sessions @ concurrency {c} "
                  f"(round {rnd}, this is the number that matters)")

            def go(i_p, q=q_idx):
                i, p = i_p
                return one_request(client, args.model,
                                   p + f"\n\nQuestion {q}: now count keyword #{i}. Answer:",
                                   args.gen_tokens,
                                   request_id=make_req_id(
                                       args.run_id,
                                       f"revisit:{tok}:{rnd}:session:{i}"))

            if c <= 1:
                samples = [go(ip) for ip in enumerate(prefixes)]
            else:
                # 线程池只管并发发请求;openai 客户端线程安全, 计时在各自线程里掐
                with ThreadPoolExecutor(max_workers=c) as ex:
                    samples = list(ex.map(go, enumerate(prefixes)))
            run_and_report(tok, rnd, samples)

        else:  # mixed(E4): churn 流 + 按节拍插 revisit 波 + 常驻 decode 探针
            c = int(conc) if conc else args.revisit_concurrency
            print(f"phase {step}: mixed — churn flow + {args.mixed_waves} revisit "
                  f"waves @ concurrency {c}, interval {args.mixed_interval:.0f}s"
                  + (f", probe {args.probe_tokens} tok/req"
                     if args.probe_tokens > 0 else ""))
            stop = threading.Event()
            dead = []   # 后台流的死因;干扰源中途消失的话整个阶段作废, 必须显式暴露

            # churn 流和探针各用独立 rng:random.Random 不是线程安全的,
            # 且独立播种保证三个流的文本互不重复(重复文本命中 prefix cache)
            churn_rng = random.Random(args.seed + 1000 * rnd)
            churn_samples = []
            churn_seq = 0
            churn_seq_lock = threading.Lock()

            def next_churn_req_id():
                nonlocal churn_seq
                with churn_seq_lock:
                    seq = churn_seq
                    churn_seq += 1
                return make_req_id(
                    args.run_id, f"mixed:{tok}:{rnd}:churn:{seq}")

            def churn_flow():
                # 闭环发压, 节奏与独立 churn phase 相同, 唯一区别是不停
                try:
                    while not stop.is_set():
                        churn_samples.append(one_request(
                            client, args.model,
                            make_text(churn_rng, args.churn_words)
                            + "\n\nSummarize. Answer:",
                            args.gen_tokens,
                            request_id=next_churn_req_id()))
                except Exception as e:
                    dead.append(f"churn 流死于: {e!r}")

            probe_rng = random.Random(args.seed + 2000 * rnd)
            probe_samples = []
            probe_seq = 0
            probe_seq_lock = threading.Lock()

            def next_probe_req_id():
                nonlocal probe_seq
                with probe_seq_lock:
                    seq = probe_seq
                    probe_seq += 1
                return make_req_id(
                    args.run_id, f"mixed:{tok}:{rnd}:probe:{seq}")

            def probe_flow():
                # 常驻长 decode:逐 token 记绝对时刻, 事后对 iostat 看 IO 高峰
                # 把 ITL 压高多少。前缀短(200 词)且每请求全新 —— prefill 干扰
                # 小, 也不碰任何缓存层
                try:
                    while not stop.is_set():
                        probe_samples.append(one_request(
                            client, args.model,
                            make_text(probe_rng, 200)
                            + "\n\nWrite a long story. Answer:",
                            args.probe_tokens, keep_chunk_times=True,
                            request_id=next_probe_req_id()))
                except Exception as e:
                    dead.append(f"decode 探针死于: {e!r}")

            flows = [threading.Thread(target=churn_flow, daemon=True)]
            if args.probe_tokens > 0:
                flows.append(threading.Thread(target=probe_flow, daemon=True))
            for th in flows:
                th.start()

            wave_samples = []
            t_phase = time.monotonic()
            for wave in range(1, args.mixed_waves + 1):
                # 波按固定节拍开环插入:睡到时间点而不是睡固定时长, 波本身的
                # 耗时不挤占下一波前的 churn 窗口;第一波前也先让 churn 跑满
                # 一个间隔, 保证 prime/上一波拉回来的前缀已被挤下盘
                time.sleep(max(0.0,
                               t_phase + wave * args.mixed_interval
                               - time.monotonic()))
                q_idx += 1

                def go_wave(i_p, q=q_idx):
                    i, p = i_p
                    return one_request(
                        client, args.model,
                        p + f"\n\nQuestion {q}: now count keyword #{i}. Answer:",
                        args.gen_tokens,
                        request_id=make_req_id(
                            args.run_id,
                            f"mixed:{tok}:{rnd}:wave:{wave}:session:{i}"))

                with ThreadPoolExecutor(max_workers=c) as ex:
                    ws = list(ex.map(go_wave, enumerate(prefixes)))
                # 波编号跨多个 mixed token 连续, 语义与 revisit 的 round 一致
                for s in ws:
                    s["round"] = (rnd - 1) * args.mixed_waves + wave
                    s["phase"] = f"mixed_revisit@{c}"
                    s["run_id"] = args.run_id
                wave_samples.extend(ws)
                report(f" wave {wave}", ws)

            stop.set()
            for th in flows:
                th.join(timeout=60)   # 只等在飞的最后一条请求收尾
            for msg in dead:
                print(f"警告: {msg} —— 干扰源不完整, 本 mixed 阶段作废")

            out.setdefault(f"mixed_revisit@{c}", []).extend(wave_samples)
            report(f"mixed_revisit@{c}", wave_samples)
            for s in churn_samples:
                s["round"] = rnd
                s["phase"] = "mixed_churn"
                s["run_id"] = args.run_id
            out.setdefault("mixed_churn", []).extend(churn_samples)
            report("mixed_churn", churn_samples)
            if probe_samples:
                for s in probe_samples:
                    s["round"] = rnd
                    s["phase"] = "probe"
                    s["run_id"] = args.run_id
                out.setdefault("probe", []).extend(probe_samples)
                # ITL = 相邻 chunk 到达时刻之差, 各请求内部取差后合并
                itls = [b - a for s in probe_samples
                        for a, b in zip(s["chunk_t"], s["chunk_t"][1:])]
                if itls:
                    print(f"probe ITL   n={len(itls)}  "
                          f"mean={statistics.mean(itls)*1000:6.2f}ms  "
                          f"p50={pct(itls, 50)*1000:6.2f}ms  "
                          f"p99={pct(itls, 99)*1000:6.2f}ms")

    # 多轮合并后的汇总(单轮的 key 逐轮打印时已完整报过, 不重复打印)
    merged = [k for k, v in out.items() if len({s["round"] for s in v}) > 1]
    if merged:
        print("\n=== 合并样本汇总(跨轮) ===")
        for k in merged:
            report(k, out[k])

    # revisit vs prime 的 TTFT 比值:1.0 = 前缀全部命中(理想),
    # ≈ prime 水平 = 全部重算(offload 没起作用或被逐光)
    if "prime" in out:
        prime_m = statistics.mean(s["ttft"] for s in out["prime"])
        for k in out:
            if "revisit" in k:
                rev_m = statistics.mean(s["ttft"] for s in out[k])
                print(f"\n{k}/prime TTFT ratio: {rev_m / prime_m:.2f} "
                      f"(越低越好;1 附近说明前缀基本没保住)")

    if args.json:
        with open(args.json, "w") as f:
            json.dump(dict(config=vars(args), samples=out), f)
        print(f"raw samples -> {args.json}")


if __name__ == "__main__":
    main()
