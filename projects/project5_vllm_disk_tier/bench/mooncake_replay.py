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
import re
import statistics
import threading
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from typing import Optional      # 不用 `dict | None`:那是 3.10+ 语法, 而 dry-run
                                 # 要在 Mac(python3 = 3.9)上跑; GPU 机是 3.12

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
BLOCK_TOKENS = 512               # Mooncake 的 hash_id 粒度

# E6 实测的 GPU KV cache 容量(serve 日志 kv_cache_utils.py:"GPU KV cache size:
# 114,240 tokens", Qwen2.5-7B + 4090)。复用距离要跟它比才有意义。
DEFAULT_GPU_CACHE_TOKENS = 114240

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


def reuse_distances(recs: list[dict]) -> list[int]:
    """LRU 栈距离:同一个 block 相邻两次访问之间, 隔了多少个**不同**的 block。

    为什么是这个量(2026-07-17 加, E6 miss 归因的可证伪点):
    读 vLLM 源码发现, offload tier 的 lookup 起点被钉在 GPU prefix cache 的命中
    边界上 —— `start_block_idx = num_computed_tokens // offloaded_block_size`,
    然后 `_maximal_prefix_lookup` 从那里往后扫, 第一个 MISS 就 break。而 Mooncake
    的复用结构是"共享前缀 + 独有新后缀":GPU 只要吃掉了**全部**共享前缀, 边界之后
    就永远是从没算过的新块, tier 必然查空、submit_load 一次都不会被调用。所以
    tier 能否被激活, 取决于"共享前缀有没有被逐出 GPU", 而这正是栈距离在回答:
        栈距离 < GPU 容量  ⟺  该次复用在 LRU 下命中 GPU  ⟹  tier 永远轮不到
    这解释了 E6 实测的 c=1 盘读 0.00GB / load_bytes 精确 0 字节。

    口径与近似(别当精确模型用):
      - 距离单位是 512-token block(Mooncake 的 hash_id 粒度), ×512 得 token 数;
        vLLM 真实的逐出粒度是 16-token block, 这里只做量级判断。
      - vLLM v1 的 GPU prefix cache 不是纯 LRU(free block queue 近似 FIFO,
        cached 块可被重新分配), 所以这是**近似**判据: 栈距离远小于容量 ⟹ 几乎
        必然命中; 远大于 ⟹ 几乎必然逐出; 骑在容量附近的那部分说不准。
      - 只算 prompt 侧的 block 复用, 不含 decode 产生的 KV(trace 无文本, 且
        E6 的 output 被 --output-cap 封到 64 token, 对容量的影响是二阶的)。

    经典 Fenwick tree(BIT)算法, O(n log n):BIT 里只在"每个 block 的最后一次
    访问位置"记 1, 于是 (上次位置, 当前位置) 开区间的和 = 中间的 distinct 数。
    """
    accesses = [h for r in recs for h in r["hash_ids"]]
    n = len(accesses)
    if n == 0:
        return []
    tree = [0] * (n + 1)

    def upd(i: int, v: int) -> None:
        i += 1
        while i <= n:
            tree[i] += v
            i += i & -i

    def qry(i: int) -> int:            # 前缀和 [0, i]
        i += 1
        s = 0
        while i > 0:
            s += tree[i]
            i -= i & -i
        return s

    last: dict[int, int] = {}
    out: list[int] = []
    for t, b in enumerate(accesses):
        p = last.get(b)
        if p is not None:
            out.append(qry(t - 1) - qry(p))   # (p, t) 开区间的 distinct 数
            upd(p, -1)                        # 旧位置退休, 只保留最后一次
        upd(t, 1)
        last[b] = t
    return out


# 窗口要能标定 load 路径, 至少得有这个比例的复用距离超出 GPU 容量。5% 是工程判断
# 不是理论值:E6 窗口是 0.0%(测出来 tier 零激活), 全 trace 是 17.9%。低于 5% 时
# 即便偶有激活也是零星几次, 撑不起 TTFT 对比。
MIN_TIER_REACHABLE = 0.05


def report_reuse_distance(recs: list[dict], gpu_cache_tokens: int) -> bool:
    """把栈距离分布对着 GPU 容量报, 回答"tier 有没有机会被用上"。

    Returns: True 如果这个窗口够得着磁盘 tier 的 load 路径。
    """
    d = reuse_distances(recs)
    print()
    print("── 复用距离 · 磁盘 tier 有没有机会被激活 ──")
    if not d:
        print("无复用, 不适用")
        return False
    cap_blocks = gpu_cache_tokens / BLOCK_TOKENS
    print(f"LRU 栈距离(两次访问同一 block 之间隔了多少 distinct block):")
    print(f"  p50={pct(d,50):.0f}  p90={pct(d,90):.0f}  p99={pct(d,99):.0f}  "
          f"max={max(d)} block")
    print(f"GPU prefix cache 容量 {gpu_cache_tokens} token = {cap_blocks:.0f} block")
    inside = sum(1 for x in d if x < cap_blocks)
    out = len(d) - inside
    print(f"  距离 < GPU 容量: {inside}/{len(d)} ({100*inside/len(d):.1f}%)"
          f"  ← 命中 GPU, **永远轮不到 tier**")
    print(f"  距离 > GPU 容量: {out}/{len(d)} "
          f"({100*out/len(d):.1f}%)  ← 只有这部分才可能打到 tier")
    if max(d) < cap_blocks:
        print(f"!! 连 max({max(d)}) 都够不着容量线({cap_blocks:.0f}) —— 预测磁盘 tier")
        print("!! 的 load 路径**一次都不会**被激活。2026-07-16 实测正是如此:"
              "c=1 的")
        print("!! load_bytes = 0 字节、md0 读 0.00GB。")
    elif out / len(d) < MIN_TIER_REACHABLE:
        print(f"!! 只有 {100*out/len(d):.1f}% 的复用够得着 tier(<{100*MIN_TIER_REACHABLE:.0f}%)"
              f" —— 这个窗口标定不了 load 路径。")
    return out / len(d) >= MIN_TIER_REACHABLE


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


# ── tier 激活哨兵 ────────────────────────────────────────────────────────────
# 2026-07-17 加。教训:2026-07-16 那轮 16 组全跑完、TTFT 报得漂漂亮亮,事后拿
# iostat 核验才发现盘读 ≈ 0、tier 的 load 路径**从没被激活过** —— 脚本对"自己的
# 测量对象在不在"完全无感,报的全是 GPU 排队。E3 的饱和自检(§339)是同一型的教训:
# 有效性哨兵必须由脚本当场喊出来,不能靠人事后反推。
#
# 判据分两层,分别对应"能判"和"判不了":
#   (1) load_bytes ≈ 0  → tier 整体(CPU primary + 磁盘 secondary)一次都没被读过。
#       这是铁证, 脚本自己就能判死, 本轮 TTFT 与 tier 读路径无关。
#   (2) load_bytes > 0  → 打到了 tier, 但**其中多少真落到盘上**这里判不了 ——
#       load_bytes 是 CPU+盘的合账, 命中 CPU tier 不产生任何盘 IO。要分开只能
#       t_wall × iostat 对齐(bench/e6_xcheck.py), 所以这里只提示、不宣判。
OFFLOAD_METRIC_RE = re.compile(
    r"^(vllm:kv_offload_\w+?)(?:\{[^}]*\})?\s+([0-9.eE+-]+)\s*$")


def scrape_offload_metrics(base_url: str) -> Optional[dict]:
    """从 vLLM 的 /metrics 抓 KV offload 计数器。抓不到返回 None(绝不阻塞回放)。"""
    url = base_url.rstrip("/")
    if url.endswith("/v1"):
        url = url[:-3]
    try:
        with urllib.request.urlopen(url.rstrip("/") + "/metrics", timeout=5) as r:
            txt = r.read().decode()
    except Exception as e:
        print(f"注: /metrics 抓取失败({e!r}), tier 激活哨兵本轮跳过")
        return None
    out: dict[str, float] = {}
    for line in txt.splitlines():
        if line.startswith("#"):
            continue
        m = OFFLOAD_METRIC_RE.match(line)
        if m:                       # 多 engine 时同名指标多行, 累加
            out[m.group(1)] = out.get(m.group(1), 0.0) + float(m.group(2))
    return out


def report_tier_activation(m0: Optional[dict], m1: Optional[dict],
                           expect_read_blocks: int):
    """回放前后的 counter 差 = 本轮真实搬运量。回答"tier 被打到了没有"。"""
    if m0 is None or m1 is None:
        return
    d = {k: m1.get(k, 0.0) - v for k, v in m0.items()}
    load = d.get("vllm:kv_offload_load_bytes", 0.0)
    store = d.get("vllm:kv_offload_store_bytes", 0.0)
    expect = expect_read_blocks * KV_MIB_PER_BLOCK * 2**20
    print()
    print("── 有效性哨兵:tier 的 load 路径被激活了吗 ──")
    print(f"本轮 tier load {load/2**30:6.2f} GiB   store {store/2**30:6.2f} GiB"
          f"   (trace 结构预期读 {expect/2**30:.2f} GiB)")
    if expect <= 0:
        return
    frac = load / expect
    if frac < 0.05:
        print(f"!! tier LOAD 路径未激活(实测/预期 = {frac:.1%})——复用被 GPU "
              f"prefix cache / CPU tier 截留在上层了。")
        print("!! 本轮 TTFT 量的是 GPU 排队与 store 路径, **不反映 tier 读路径**,")
        print("!! 不能用来比较 fs/uring 的 load 性能。要激活它需要逐出压力")
        print("!! (churn 那类持续写流), 光有 trace 结构上的前缀复用不够。")
    elif frac < 0.5:
        print(f"?  tier load 只有预期的 {frac:.1%} —— 大部分复用被上层截留,"
              f" 结论要按这个比例打折。")
    else:
        print(f"OK tier load = 预期的 {frac:.1%}。注意:这是 CPU tier + 磁盘 tier"
              f" 的合账,其中多少真落到盘上要 t_wall×iostat 对齐才知道")
        print("   (bench/e6_xcheck.py),命中 CPU tier 不产生任何盘 IO。")


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
    print(f"复用前缀的请求 {len(hit)}/{a['n_req']} ({100*len(hit)/a['n_req']:.0f}%)  "
          f"复用深度 mean={statistics.mean(rd):.1f} block  "
          f"p99={pct(rd,99):.0f} block" if a["n_req"] else "")
    # 这个"读"是 trace 结构上的**逻辑**复用, 不是盘读。中间隔着 GPU prefix cache
    # 和 CPU tier 两层截留 —— 2026-07-16 实测:逻辑读/写 0.91 的窗口, 物理盘读
    # ≈ 0(tier load_bytes 在 c=1 是 0 字节), 复用全被上层吃掉。磁盘 tier 的激活
    # 开关是**逐出压力**, 不是复用率。离线标定看不见这一层, 别拿它预测盘负载。
    print("  ↑ 逻辑复用(trace 结构), **不等于**盘读:GPU prefix cache / CPU tier")
    print("    会截留大部分复用。真实盘读要上机 + iostat 对齐才知道(见 E6 核验)。")
    print()
    reuse_ok = report_reuse_distance(recs, args.gpu_cache_tokens)
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
    disk_ok = a["disk_upper_mib"] / 1024 <= budget
    verdict = "OK" if disk_ok else "!! 超预算, 缩窗口或加盘"
    print(f"盘预算        {budget} GiB   → {verdict}")
    print(f"（uring 若要零逐出, --disk-bytes 需 ≥ 上界；fs 无上限但盘要装得下）")

    # ── 窗口判据汇总 ────────────────────────────────────────────────────────
    # 2026-07-17 加, 直接来自 E6 的教训:当年选窗口只看了 (1) 盘装得下 + 读写比
    # 均衡, 漏掉 (2), 结果 16 组、一整夜跑完, 核验才发现 tier 的 load 路径一次
    # 都没被激活 —— 窗口从一开始就结构性地测不了它要测的东西。两条判据是**反向**
    # 的:窗口越长, 复用距离越够得着 tier, 盘越装不下。中间不一定有解, 没解就得
    # 承认这个尺度测不了, 而不是挑个装得下的窗口自欺。
    print()
    print("── 窗口判据汇总(要标定 load 路径, 前两条必须同时过)──")
    print(f"  (1) 盘装得下           {'OK' if disk_ok else '!! 不过'}")
    print(f"  (2) 复用距离够得着 tier {'OK' if reuse_ok else '!! 不过'}")
    if disk_ok and reuse_ok:
        print("  → 可上机标定 load 路径。上机后仍要看回放末尾的 tier 激活哨兵"
              "(真值), 这里只是离线预测。")
    elif not reuse_ok:
        print("  → **不可用于标定 load 路径**:复用会被 GPU prefix cache 全吃掉,")
        print("     跑出来的 TTFT 量的是 GPU 排队与 store 路径, 不是 tier 读路径。")
        print("     加窗口长度能拉长复用距离, 但盘上界会同比涨 —— 先确认有解。")
    else:
        print("  → 盘装不下:缩窗口, 但缩完要回来重看 (2), 别把 tier 缩没了。")
    if args.max_input_tokens == 0:
        print()
        print("!! --max-input-tokens=0(未过滤):上机时 serve 会拒掉超长请求, 这份")
        print("!! 标定与实跑口径**不符**, 取到的根本不是同一批请求。实跑必须设成")
        print("!! serve 的 --max-model-len(E6 = 16384)。")


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
    m0 = scrape_offload_metrics(args.base_url)   # tier 激活哨兵:回放前的基线
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
        # 在飞压到上限 = 客户端在限流。并发扫描里这是**故意**的(靠它把并发钉在
        # c), 回放退化成闭环并发 c、trace 到达时序被改写 —— 是特性不是告警。
        # 只有当你本意是"按原始到达率开环回放"时, 触顶才意味着并发标定偏低。
        print(f"注: 在飞触顶 {args.max_inflight}, 已退化为闭环并发 "
              f"{args.max_inflight}(到达时序被改写);扫并发时这是预期")

    m1 = scrape_offload_metrics(args.base_url)   # tier 激活哨兵:回放后

    ok = [s for s in samples if s and s.get("ttft") is not None]
    err = [s for s in samples if s and s.get("err")]
    ttfts = [s["ttft"] for s in ok]
    print()
    report_ttft("all revisit TTFT", ttfts)
    # 按 trace 结构分:这条请求的前导 block 之前出现过没有。
    #
    # 注意这是**纯客户端的 trace 分析** —— 只看 hash_ids 的复用结构, 全程不碰
    # vLLM, 不看任何 tier 状态。所以它答的是"这条请求在 trace 里复用了前缀吗",
    # **不是**"它打到 tier 了吗"。原标签写的是"前缀命中(打 tier)", 是错的:复用
    # 可以命中 GPU prefix cache、CPU tier、磁盘 tier 任意一层, 这段代码一层都分
    # 不出来。2026-07-16 实测正是被这个标签坑了 —— 147/150 "命中", 而 tier 的
    # load_bytes 是 0 字节, 复用全被 GPU prefix cache 吃掉了。要知道打没打 tier
    # 看下面的激活哨兵, 要知道打没打盘看 bench/e6_xcheck.py。
    seen: set = set()
    reuse_ttft, first_ttft = [], []
    for idx, r in enumerate(recs):
        s = samples[idx]
        lead = any(h in seen for h in r["hash_ids"][:1])  # 有前导复用
        for h in r["hash_ids"]:
            seen.add(h)
        if s and s.get("ttft") is not None:
            (reuse_ttft if lead else first_ttft).append(s["ttft"])
    report_ttft("  前缀复用(trace 结构)", reuse_ttft)
    report_ttft("  前缀首现(必然全算)", first_ttft)
    if err:
        print(f"失败请求 {len(err)}/{len(recs)}: {err[0].get('err')!r} ...")

    # 预期读量按 trace 结构算(复现的 block 数),哨兵拿它当分母
    report_tier_activation(m0, m1, analyze(recs)["read_blocks"])

    # 实测在飞并发 + 饱和自检(用每条的 t_wall..t_wall+total 做扫描线)。回答
    # "这个 c 有没有把服务器压到饱和":实测并发钉死在 max-inflight、且 ttft 后半段
    # 明显更慢(backlog 在涨) = 该档仍饱和, 它的 TTFT 量的是队列不是 tier, 不能用。
    # 有这两行, 下次一眼看穿过载, 不用靠 fs≈uring 反推(上次那次废数据的教训)。
    iv = [(s["t_wall"], s["t_wall"] + s["total"]) for s in ok]
    ev = []
    for lo, hi in iv:
        ev.append((lo, 1))
        ev.append((hi, -1))
    ev.sort()
    depth = 0
    series = []
    for _, d in ev:
        depth += d
        if d > 0:
            series.append(depth)
    if series:
        print(f"实测在飞并发   p50={pct(series,50):.0f}  p99={pct(series,99):.0f}  "
              f"max={max(series)}  (--max-inflight={args.max_inflight})")
    by_sub = [s["ttft"] for s in sorted(ok, key=lambda s: s["t_wall"])]
    if len(by_sub) >= 4:
        h = len(by_sub) // 2
        m1, m2 = statistics.mean(by_sub[:h]), statistics.mean(by_sub[h:])
        flag = "  ← 后半明显更慢, backlog 在涨, 该档仍饱和" if m2 > 1.5 * m1 else ""
        print(f"ttft 前半 {m1*1000:.0f}ms → 后半 {m2*1000:.0f}ms{flag}")

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
    ap.add_argument("--gpu-cache-tokens", type=int, default=DEFAULT_GPU_CACHE_TOKENS,
                    help="GPU KV cache 容量(token), 复用距离拿它当判据。默认取 E6 "
                         "宿主实测值; 换模型/换卡要照 serve 日志的 "
                         "'GPU KV cache size' 改")
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
