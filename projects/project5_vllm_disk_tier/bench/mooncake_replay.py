#!/usr/bin/env python3
# E6 收尾:回放真实 Mooncake trace, 标定"真实多轮负载落在 E1~E4 扫出来的哪个区间"。
#
# 为什么用 Mooncake(COMPARE_PLAN §125 定案):它是生产 trace, 本身就是
# KVCache/block 粒度的多轮会话记录, 带真实到达时间 —— 和我们的观测对象(投影到
# block 粒度的 IO 流)同层。ShareGPT 只有文本没到达时间, 得自己合成到达过程,
# 标定意义打折。
#
# ── Mooncake trace 格式(mooncake_trace.jsonl, 每行一条 JSON)────────────────
#   timestamp     : int, 毫秒。相对到达时刻, 0 ~ 3_600_000(一小时窗口)
#   input_length  : int, prompt 的 token 数(trace 脱敏, 无真实文本)
#   output_length : int, 生成的 token 数
#   hash_ids      : list[int]。每个元素 = 一个 512-token block 的哈希 ID。
#                   哈希是"累积"的 —— 第 k 个 id 覆盖 block 0..k 的全部 token,
#                   所以同一个 id 只会出现在同一个位置。
#   样例: {"timestamp": 0, "input_length": 655, "output_length": 52,
#          "hash_ids": [0, 1, 2]}
#
# ── 复用怎么编码 ──────────────────────────────────────────────────────────
#   两条请求"共享前缀" ⟺ 它们的 hash_ids 共享一段前导子数组。因为哈希累积,
#   前导 id 相同就意味着对应的 block 及其之前的所有 token 都相同。多轮会话的
#   第 2 轮 = 复用第 1 轮的前导 block + 尾部几个新 block, 正是我们要打的
#   "被逐出的长前缀第二次来时从盘上拉回 vs 重算"。
#
# ── 到现有 prompt 生成器(long_context_ttft.py)的映射 ─────────────────────
#   现有生成器把"会话"表示成一条固定长前缀 prefixes[i] + 可变问题后缀, 复用靠
#   逐字节重发同一段文本命中 vLLM 的按内容哈希 prefix cache。Mooncake 是它的
#   一般化:
#     block_text(hid)  = 用 hid 播种的确定性词序列, 固定 --block-words 个词
#                        (≈ 512 token)。同一个 id 永远给同一段文本。
#     prompt(request)  = " ".join(block_text(h) for h in hash_ids)
#   于是"共享前导 id" ⟹ "共享前导文本" ⟹ vLLM 在共享区每个 16-token block
#   内容哈希命中 —— trace 的复用结构被无损搬到真 vLLM 上。
#
#   一处必须记账的妥协:每个 block 用**固定**词数, 不按 input_length 的末块
#   余数裁剪。因为同一个 hash_id 在 A 里可能是末块(partial)、在扩展它的 B 里
#   是中块(full), 若按余数裁剪, 两处文本长度不一致 → 命中被破坏。用"固定块"换
#   "命中保真", 代价是实际 token 数被向上取整到 512 的倍数 —— 对 tier 的 KV
#   落盘量是个小的、系统性的高估, 可接受(input_length 只用来事后交叉核对)。
#
# ── 三个标定量(COMPARE_PLAN §123, E6 的交付物)─────────────────────────────
#   (1) revisit 窗口盘读量落在 E1 冷/热哪个区间   —— 要上机跑, 从 iostat 出
#   (2) 并发 load 深度分布对到 E3 哪个档 {1,4,8,16} —— --dry-run 就能从到达
#       过程估, 上机后从实测 in-flight 出
#   (3) 读写比对到 E4 的干扰强度                   —— block 粒度的读写比 --dry-run
#       直接算:首次出现的 block = 写(store), 再次出现 = 读(load)
#   前两个的离线估 + 第三个的精确值都在 --dry-run 里给, 不烧 GPU。
#
# 依赖: pip install openai(上机跑需要; --dry-run 不需要 server 但仍 import)
# 前提: vllm serve 已按 docs/RUN_ON_GPU.md 起好, --shutdown-timeout 30

import argparse
import gzip
import json
import random
import statistics
import threading
import time
from concurrent.futures import ThreadPoolExecutor

# 请求原语 one_request 复用现有 driver(同一套计时/流式掐 TTFT, 保证 E6 的数和
# E1~E4 可比), 但它 import openai —— 只在 replay() 里惰性 import, 好让 --dry-run
# 在没装 openai 的 Mac 上也能跑标定。


def pct(xs, p):
    return statistics.quantiles(xs, n=100)[p - 1] if len(xs) > 1 else xs[0]

# 每个 512-token block 的 KV 落盘上界(E1 摸底: Qwen2.5-7B 每 token KV 56KB,
# vLLM block_size=16 → 每 16-token 块 0.875MiB → 512 token = 32 块 = 28 MiB)。
# 只有被逐出的 block 才真落盘, 所以 distinct_blocks × 28MiB 是磁盘占用的**上界**
# (兜最坏, 沿用 E4 的容量纪律)。
KV_MIB_PER_BLOCK = 28.0

# block 文本的词表:沿用 long_context_ttft 的常见词, 大多 1 词≈1 token, 便于
# --block-words 直接逼近 512 token。词表大小无所谓, 确定性来自 per-id 播种。
_VOCAB = ("system cache tensor kernel stream block prefix decode layer batch "
          "memory transfer schedule request token attention weight buffer queue "
          "submit reap align direct storage evict promote tier context session "
          "prompt replay arrival window offload backing slab extent worker punt "
          "hash reuse suffix prefill throughput latency bandwidth device queue").split()


def load_trace(path: str) -> list[dict]:
    """读 jsonl(支持 .gz), 每行一条记录, 按 timestamp 升序返回。

    只保留四个字段, 缺字段的行报错而不是静默跳过 —— trace 格式对不上要立刻
    暴露, 别跑了半宿标定出一堆垃圾。
    """
    op = gzip.open if path.endswith(".gz") else open
    recs = []
    with op(path, "rt") as f:
        for ln, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
                recs.append(dict(
                    timestamp=int(r["timestamp"]),
                    input_length=int(r["input_length"]),
                    output_length=int(r["output_length"]),
                    hash_ids=[int(h) for h in r["hash_ids"]],
                ))
            except (KeyError, ValueError, TypeError) as e:
                raise SystemExit(f"trace 第 {ln} 行格式不对({e!r}): {line[:120]}")
    recs.sort(key=lambda r: r["timestamp"])
    return recs


class BlockText:
    """hash_id → 确定性文本块。缓存已生成的块, 保证同一 id 每次逐字节相同。"""

    def __init__(self, block_words: int, seed: int):
        self.block_words = block_words
        self.seed = seed
        self._cache: dict[int, str] = {}

    def __call__(self, hid: int) -> str:
        t = self._cache.get(hid)
        if t is None:
            # 用 "seed:hid" 播种:换 --seed 能整体换一批文本(报告纪律里的
            # "关键结论换 seed 重跑一遍确认不翻"), 但同一次运行里 id→文本稳定。
            # 用 str 而非 tuple:Python 3.12 的 random 只收 int/float/str/bytes
            rng = random.Random(f"{self.seed}:{hid}")
            t = " ".join(rng.choice(_VOCAB) for _ in range(self.block_words))
            self._cache[hid] = t
        return t


def build_prompt(bt: BlockText, hash_ids: list[int]) -> str:
    return " ".join(bt(h) for h in hash_ids)


def analyze(recs: list[dict]) -> dict:
    """离线算 E6 的三个标定量里能离线算的部分, 不碰 server。

    读写比(标定量 3)是精确的:block 首次出现 = 写, 再次出现 = 读。
    并发深度(标定量 2)给一个到达过程的估计:用一个名义服务时长把每条请求摊成
    一个占用区间, 数最大重叠 —— 真实值上机后从实测 in-flight 出, 这里只给量级。
    """
    seen: set[int] = set()
    write_blocks = read_blocks = 0
    reuse_depth = []      # 每条请求命中的前导已见 block 数(= 复用前缀深度)
    for r in recs:
        lead = 0
        counting_lead = True
        for h in r["hash_ids"]:
            if h in seen:
                read_blocks += 1
                if counting_lead:
                    lead += 1
            else:
                write_blocks += 1
                counting_lead = False   # 前导一旦断了, 后面的复用不算"前缀"
                seen.add(h)
        reuse_depth.append(lead)

    span_ms = (recs[-1]["timestamp"] - recs[0]["timestamp"]) if recs else 0
    return dict(
        n_req=len(recs),
        span_s=span_ms / 1000.0,
        distinct_blocks=len(seen),
        write_blocks=write_blocks,
        read_blocks=read_blocks,
        rw_ratio=(read_blocks / write_blocks) if write_blocks else float("inf"),
        reuse_depth=reuse_depth,
        disk_upper_mib=len(seen) * KV_MIB_PER_BLOCK,
        in_len=[r["input_length"] for r in recs],
        out_len=[r["output_length"] for r in recs],
    )


def estimate_concurrency(recs: list[dict], service_s: float) -> list[int]:
    """把每条请求摊成 [到达, 到达+service_s] 的占用区间, 扫描线数并发深度。

    service_s 是名义服务时长(默认 0.4s, E3 c=8 一波的量级), 只为把到达过程翻译
    成"同时在飞多少"给出量级估计, 好对到 E3 的档。真值上机测。
    """
    ev = []
    for r in recs:
        t = r["timestamp"] / 1000.0
        ev.append((t, +1))
        ev.append((t + service_s, -1))
    ev.sort()
    depth = 0
    series = []
    for _, d in ev:
        depth += d
        if d > 0:
            series.append(depth)
    return series


def window(recs: list[dict], args) -> list[dict]:
    """按 --time-scale / --max-duration-s / --max-requests 裁一段来跑。

    整条 trace 是一小时、distinct block 可能远超 35G 盘, 不能整跑;裁窗口时保持
    原始相对到达间隔(乘 time-scale), 让并发结构不被破坏。
    """
    if not recs:
        return recs
    t0 = recs[0]["timestamp"]
    out = []
    for r in recs:
        rel = (r["timestamp"] - t0) / 1000.0 * args.time_scale
        if args.max_duration_s and rel > args.max_duration_s:
            break
        r = dict(r, _arrival_s=rel)
        out.append(r)
        if args.max_requests and len(out) >= args.max_requests:
            break
    return out


def report_ttft(name: str, xs: list[float]):
    if not xs:
        print(f"{name}: no samples")
        return
    print(f"{name:20s} n={len(xs):4d}  mean={statistics.mean(xs)*1000:8.1f}ms  "
          f"p50={pct(xs,50)*1000:8.1f}ms  p99={pct(xs,99)*1000:8.1f}ms")


def preflight(recs: list[dict], a: dict, args, n_dropped: int = 0, n_all: int = 0):
    """把标定量和盘占用上界打出来 —— dry-run 的主输出, 也是上机前的安检。"""
    print("═══ Mooncake trace 回放 · 预检 / 标定 ═══")
    if n_dropped:
        print(f"超长丢弃      {n_dropped}/{n_all} "
              f"({100*n_dropped/n_all:.0f}%) input_length > "
              f"{args.max_input_tokens} token(真实 16k serve 也会拒)")
    print(f"请求数        {a['n_req']}")
    print(f"时间跨度      {a['span_s']:.1f}s  (time-scale={args.time_scale}"
          f"{', 裁到 '+str(args.max_duration_s)+'s' if args.max_duration_s else ''})")
    inl, outl = a["in_len"], a["out_len"]
    print(f"input_length  mean={statistics.mean(inl):.0f}  "
          f"p50={pct(inl,50):.0f}  p99={pct(inl,99):.0f} token")
    print(f"output_length mean={statistics.mean(outl):.0f}  "
          f"p50={pct(outl,50):.0f}  p99={pct(outl,99):.0f} token")
    print()
    print("── 标定量 (3) 读写比 · 对到 E4 干扰强度 ──")
    print(f"distinct block {a['distinct_blocks']}  (每 512 token)")
    print(f"写(首现) {a['write_blocks']}   读(复现) {a['read_blocks']}   "
          f"读/写 = {a['rw_ratio']:.2f}")
    rd = a["reuse_depth"]
    hit = [d for d in rd if d > 0]
    print(f"命中前缀的请求 {len(hit)}/{a['n_req']} ({100*len(hit)/a['n_req']:.0f}%)  "
          f"复用深度 mean={statistics.mean(rd):.1f} block  "
          f"p99={pct(rd,99):.0f} block" if a["n_req"] else "")
    print()
    print("── 标定量 (2) 并发 load 深度 · 对到 E3 档 {1,4,8,16} ──")
    series = estimate_concurrency(recs, args.service_s)
    if series:
        print(f"到达过程估(名义服务 {args.service_s}s): "
              f"mean={statistics.mean(series):.1f}  p50={pct(series,50):.0f}  "
              f"p99={pct(series,99):.0f}  max={max(series)}  (真值上机测)")
    print()
    print("── 盘占用上界 · E4 容量纪律(兜最坏, 假设 distinct block 全落盘)──")
    print(f"KV 落盘上界   {a['disk_upper_mib']/1024:.1f} GiB "
          f"= {a['distinct_blocks']} block × {KV_MIB_PER_BLOCK:.0f} MiB")
    budget = args.disk_budget_gib
    verdict = "OK" if a["disk_upper_mib"]/1024 <= budget else "!! 超预算, 缩窗口或加盘"
    print(f"盘预算        {budget} GiB   → {verdict}")
    print(f"（uring 若要零逐出, --disk-bytes 需 ≥ 上界；fs 无上限但盘要装得下）")


def replay(recs: list[dict], args):
    """开环回放:submitter 睡到每条请求的到达时刻再交给线程池, 不闭环等返回,
    这样到达过程(= E3 的并发轴来源)才被如实复现。"""
    from openai import OpenAI
    from long_context_ttft import one_request
    client = OpenAI(base_url=args.base_url, api_key="EMPTY")
    bt = BlockText(args.block_words, args.seed)

    samples: list[dict] = [None] * len(recs)   # type: ignore
    inflight = threading.Semaphore(args.max_inflight)
    saturated = threading.Event()

    def fire(idx: int, r: dict):
        try:
            n_out = min(int(r["output_length"] * args.output_scale),
                        args.output_cap) if args.output_cap else \
                    int(r["output_length"] * args.output_scale)
            n_out = max(1, n_out)
            prompt = build_prompt(bt, r["hash_ids"])
            s = one_request(client, args.model, prompt, n_out)
            s["idx"] = idx
            s["arrival_s"] = r["_arrival_s"]
            s["n_blocks"] = len(r["hash_ids"])
            samples[idx] = s
        except Exception as e:                       # 单条失败不该整轮崩
            samples[idx] = dict(idx=idx, ttft=None, err=repr(e),
                                arrival_s=r["_arrival_s"])
        finally:
            inflight.release()

    print(f"开环回放 {len(recs)} 条, 峰值 in-flight 上限 {args.max_inflight} "
          f"(命中=复现相位被压缩就调大它)")
    t_start = time.monotonic()
    with ThreadPoolExecutor(max_workers=args.max_inflight) as ex:
        for idx, r in enumerate(recs):
            # 睡到该请求的到达时刻(相对 t_start), 保持 trace 的到达间隔
            time.sleep(max(0.0, t_start + r["_arrival_s"] - time.monotonic()))
            if not inflight.acquire(blocking=False):
                saturated.set()          # in-flight 打满 → 后续 submit 被拖慢,
                inflight.acquire()       # 到达过程失真, 事后要在报告里标出来
            ex.submit(fire, idx, r)
    # with 退出即 join 所有在飞请求

    if saturated.is_set():
        print(f"警告: in-flight 触顶 {args.max_inflight}, 到达过程被压缩; "
              f"提高 --max-inflight 重跑, 否则并发深度标定偏低")

    ok = [s for s in samples if s and s.get("ttft") is not None]
    err = [s for s in samples if s and s.get("err")]
    ttfts = [s["ttft"] for s in ok]
    print()
    report_ttft("all revisit TTFT", ttfts)
    # 命中前缀 vs 未命中 分开报:前者打的才是 tier 的 load 路径
    seen: set = set()
    hit_ttft, miss_ttft = [], []
    for idx, r in enumerate(recs):
        s = samples[idx]
        lead = any(h in seen for h in r["hash_ids"][:1])  # 有前导复用
        for h in r["hash_ids"]:
            seen.add(h)
        if s and s.get("ttft") is not None:
            (hit_ttft if lead else miss_ttft).append(s["ttft"])
    report_ttft("  前缀命中(打 tier)", hit_ttft)
    report_ttft("  前缀未命中(新建)", miss_ttft)
    if err:
        print(f"失败请求 {len(err)}/{len(recs)}: {err[0].get('err')!r} ...")

    if args.json:
        with open(args.json, "w") as f:
            json.dump(dict(config=vars(args),
                           samples=[s for s in samples if s]), f)
        print(f"raw samples -> {args.json}")


def main():
    ap = argparse.ArgumentParser(description="回放 Mooncake trace 标定 disk tier")
    ap.add_argument("--trace", required=True,
                    help="Mooncake trace jsonl(支持 .gz)。下载:kvcache-ai/"
                         "Mooncake 的 FAST25-release/traces/{conversation,mooncake}"
                         "_trace.jsonl(conversation 是多轮会话, E6 首选)")
    ap.add_argument("--base-url", default="http://localhost:8000/v1")
    ap.add_argument("--model", default="Qwen/Qwen2.5-7B-Instruct")
    ap.add_argument("--json", default=None, help="原始样本落到这个文件")
    ap.add_argument("--dry-run", action="store_true",
                    help="只解析+标定+安检, 不连 server, 不烧 GPU")
    # 超长请求过滤:trace 的 input_length p99 可达 8w+ token, 远超 serve 的
    # --max-model-len, 真发上去 vLLM 直接拒。丢弃它们反而更贴真实(16k serve
    # 本来也拒), 顺带砍掉盘上界(巨请求贡献大量 block)。设成 max-model-len。
    ap.add_argument("--max-input-tokens", type=int, default=0,
                    help="丢弃 input_length 超过此值的请求, 0 = 不过滤;"
                         "上机时设成 serve 的 --max-model-len(如 16384)")
    # 窗口裁剪:整条 trace 一小时、盘装不下, 得裁一段
    ap.add_argument("--time-scale", type=float, default=1.0,
                    help="到达时刻乘这个系数, <1 压缩加压, >1 拉长(默认 1 原速)")
    ap.add_argument("--max-duration-s", type=float, default=0.0,
                    help="只回放前 N 秒(scale 之后), 0 = 不限")
    ap.add_argument("--max-requests", type=int, default=0,
                    help="只回放前 N 条, 0 = 不限")
    # block→文本 映射
    ap.add_argument("--block-words", type=int, default=400,
                    help="每个 512-token block 生成多少词(≈token, 上机用真 "
                         "tokenizer 校一下, 但 tier 对精确 token 数不敏感)")
    ap.add_argument("--seed", type=int, default=42,
                    help="block 文本的播种;换 seed 整体换文本, 验证结论不翻")
    # 输出长度:decode 贵, 默认封顶(E6 主指标是 TTFT/prefill, 见 METRICS.md)
    ap.add_argument("--output-scale", type=float, default=1.0)
    ap.add_argument("--output-cap", type=int, default=64,
                    help="每请求生成 token 上限, 0 = 完全按 trace 的 output_length")
    # 开环回放
    ap.add_argument("--max-inflight", type=int, default=256,
                    help="峰值同时在飞请求数;触顶会压缩到达过程(报告会警告)")
    # 标定/安检参数
    ap.add_argument("--service-s", type=float, default=0.4,
                    help="dry-run 估并发深度用的名义服务时长(E3 c=8 一波量级)")
    ap.add_argument("--disk-budget-gib", type=float, default=30.0,
                    help="盘预算, 落盘上界超了就报警(E4 用 30G)")
    args = ap.parse_args()

    recs = load_trace(args.trace)
    n_all = len(recs)
    n_dropped = 0
    if args.max_input_tokens:
        # 过滤放在时间窗口之前:先按真实 context 上限筛, 再裁时间段
        recs = [r for r in recs if r["input_length"] <= args.max_input_tokens]
        n_dropped = n_all - len(recs)
    recs = window(recs, args)
    if not recs:
        raise SystemExit("窗口内没有请求, 检查 --max-* / --max-input-tokens 参数")
    a = analyze(recs)
    preflight(recs, a, args, n_dropped, n_all)

    if args.dry_run:
        return
    print()
    replay(recs, args)


if __name__ == "__main__":
    main()
