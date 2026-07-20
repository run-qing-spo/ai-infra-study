#!/usr/bin/env python3
# 微基准:同一份 block 搬运负载, 四种磁盘引擎对打(不涉及 vLLM, 纯 IO 层):
#
#   uring : 本项目引擎 —— SPSC → 单线程 io_uring 批量提交, O_DIRECT, 单大文件
#   cpp-pool : C++ 对照组(BENCH_ANALYSIS 的提交模型一节) —— std::thread 线程池 + 同步
#           pread/pwrite, 其余(单大文件/O_DIRECT/submit-poll 接口)与 uring
#           引擎一字不差。uring vs cpp-pool 的差 = 提交模型本身, 不掺 GIL。
#   pool  : 复刻 vLLM fs tier 的语义 —— 线程池, 每 block 一个文件,
#           O_DIRECT, 写 tmp + os.replace(照抄 tiering/fs/io.py 的做法)
#   pool-slab : 消融组 —— 同样线程池, 但去掉 file-per-block, 写同一个大文件。
#           用来把 "文件元数据开销" 和 "提交模型开销" 两个变量拆开:
#           pool vs pool-slab 的差 = 元数据;pool-slab vs cpp-pool 的差 =
#           GIL/Python;cpp-pool vs uring 的差 = 提交模型。
#
# 量什么(stdout 一行一个场景, 也可 --json):
#   wall_s      总墙钟
#   MB_s        有效带宽
#   iops        block IO / s
#   cpu_util    (utime+stime)/wall —— 单线程提交省 CPU 的直接证据
#   syscalls≈   uring 引擎的 submit_calls(pool 组按 2×blocks 估:write+replace)
#
# 用法(Linux + 已 make):
#   python3 bench/bench_engine.py --smoke                    # 64 块正确性冒烟
#   python3 bench/bench_engine.py --dir /root/autodl-tmp     # 完整对比
#   python3 bench/bench_engine.py --dir ... --block-mb 1 --blocks 2048 --json

import argparse
import concurrent.futures as cf
import json
import mmap
import os
import resource
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))

O_DIRECT = getattr(os, "O_DIRECT", 0)


def make_region(num_blocks: int, block_bytes: int) -> memoryview:
    # mmap 匿名映射:页对齐, 跟 SharedOffloadRegion 的对齐性质一致
    m = mmap.mmap(-1, num_blocks * block_bytes)
    # pre-touch:匿名页懒分配, 不先摸一遍的话 first-touch 缺页会发生在引擎
    # worker 的提交路径里(GUP pin 页时), 把"分配+清零整个缓冲区"记到引擎
    # 头上 —— §10 翻身实验的 perf 在 io_submit_sqes 下面直接抓到
    # shmem_fault/clear_page_erms。真实场景内存池长期存活, 这个一次性成本
    # 不该进计时窗口。(只写 block 头 8 字节不够:1MB block 有 256 页,
    # 只有第 1 页被摸到, 其余 255 页仍留给 worker 首触。)
    zeros = b"\0" * (16 << 20)
    left = num_blocks * block_bytes
    while left > 0:
        n = min(len(zeros), left)
        m.write(zeros[:n])
        left -= n
    m.seek(0)
    mv = memoryview(m)
    # 填可校验的内容:每个 block 开头 8 字节写 block 序号
    for i in range(num_blocks):
        mv[i * block_bytes : i * block_bytes + 8] = i.to_bytes(8, "little")
    return mv


def cpu_time() -> float:
    r = resource.getrusage(resource.RUSAGE_SELF)
    return r.ru_utime + r.ru_stime


def count_iou_workers() -> int:
    # io-wq worker 是本进程的线程, 出现在 /proc/self/task 里, comm 叫 iou-wrk-*。
    # 用来验证 punt+per-inode-hash 假设:store 期间恒 1 = write 被串到单 worker,
    # load 期间几十 = read 不 hash 可并行。
    n = 0
    for tid in os.listdir("/proc/self/task"):
        try:
            with open(f"/proc/self/task/{tid}/comm") as f:
                if f.read().startswith("iou-wrk"):
                    n += 1
        except FileNotFoundError:
            pass  # 线程刚退出, 忽略
    return n


# ---------------------------------------------------------------- uring ----

def run_uring(mv, args, direction: str, cpp_pool: bool = False) -> dict:
    from kv_uring_tier import _kvtier

    # cpp-pool 对照组走的就是这条函数:submit/poll/drain 的 bench 侧代码
    # 零差异, 只换引擎类 —— 对照里唯一变量是引擎内部的提交模型
    # (io_uring 批量 vs 线程池同步 syscall)。depth 参数语义随引擎走:
    # uring 是 queue_depth, cpp-pool 是线程数(取 --pool-threads, 和
    # Python pool 组同宽, 三方可比)。
    eng_cls = _kvtier.PoolEngine if cpp_pool else _kvtier.Engine
    depth = args.pool_threads if cpp_pool else args.queue_depth
    fname = "bench_cpppool" if cpp_pool else "bench_uring"

    bs = args.block_bytes
    n_shard = args.shard_files
    assert args.blocks % n_shard == 0, "--blocks 必须能被 --shard-files 整除"
    chunk = args.blocks // n_shard

    # 分片探针:N 个引擎(各自 ring + worker 线程)把负载拆到多文件/同一文件,
    # 验证"单 fd O_DIRECT write 串行"发生在哪一层:
    #   多文件提速 & 同文件也提速 → 串行在单 ring 的 io-wq(punt 后按 inode 排单列)
    #   多文件提速 & 同文件不提速 → 串行真的在 fs/inode 层
    # 注意 --shard-same-file 时 N 个引擎写同一段 offset, 数据互相覆盖,
    # 纯性能探针, 不做正确性。
    engines, paths = [], []
    for k in range(n_shard):
        if args.shard_same_file or n_shard == 1:
            path = os.path.join(args.dir, f"{fname}.bin")
        else:
            path = os.path.join(args.dir, f"{fname}.{k}.bin")
        paths.append(path)
        region = mv[k * chunk * bs : (k + 1) * chunk * bs]
        engines.append(eng_cls(region, path, chunk, bs,
                               depth, not args.no_odirect))

    t0, c0 = time.perf_counter(), cpu_time()
    total_jobs = 0
    for eng in engines:
        submit = eng.submit_store if direction == "store" else eng.submit_load
        job_id = 0
        for start in range(0, chunk, args.job_blocks):
            ids = list(range(start, min(start + args.job_blocks, chunk)))
            # slot == block_id:1:1 布局, bench 只量数据面, 不掺账本逻辑
            assert submit(job_id, ids, ids)
            job_id += 1
        total_jobs += job_id
    done = 0
    iou_peak, last_sample = 0, 0.0
    while done < total_jobs:
        got = 0
        for eng in engines:
            got += len(eng.poll_finished())
        done += got
        # 限频 10ms 采一次:poll loop 是忙转, 每圈读 /proc 会污染 CPU 数字
        now = time.perf_counter()
        if now - last_sample >= 0.01:
            iou_peak = max(iou_peak, count_iou_workers())
            last_sample = now
        if got == 0 and done < total_jobs:
            # 空轮真睡:主线程忙轮询恒占 1 核, cpu_util 反映不了 worker 侧
            # 成本(§8.6 旧账)。sleep(0) 只让出 GIL 不省 CPU, 要真睡;
            # 0.5ms 粒度对秒级 wall 的影响可忽略。
            time.sleep(0.0005)
    wall, cpu = time.perf_counter() - t0, cpu_time() - c0

    syscalls, failed = 0, 0
    for eng in engines:
        s = eng.stats()
        syscalls += s["submit_calls"]
        failed += s["jobs_failed"]
    del engines
    if direction == "load":
        for p in set(paths):
            os.unlink(p)
    return dict(wall_s=wall, cpu_s=cpu, syscalls=syscalls,
                failed=failed, iou_wrk=iou_peak)


# ----------------------------------------------------------------- pool ----

def _pool_write_file(path: str, mv, off: int, size: int):
    tmp = path + ".tmp"
    fd = os.open(tmp, os.O_CREAT | os.O_EXCL | os.O_WRONLY | O_DIRECT, 0o644)
    try:
        os.write(fd, mv[off : off + size])
    finally:
        os.close(fd)
    os.replace(tmp, path)


def _pool_read_file(path: str, mv, off: int, size: int):
    fd = os.open(path, os.O_RDONLY | O_DIRECT)
    try:
        os.readv(fd, [mv[off : off + size]])
    finally:
        os.close(fd)


def _pool_write_slab(fd: int, mv, off: int, size: int):
    os.pwrite(fd, mv[off : off + size], off)


def _pool_read_slab(fd: int, mv, off: int, size: int):
    os.preadv(fd, [mv[off : off + size]], off)


def run_pool(mv, args, direction: str, slab: bool) -> dict:
    bs = args.block_bytes
    if slab:
        path = os.path.join(args.dir, "bench_pool_slab.bin")
        fd = os.open(path, os.O_CREAT | os.O_RDWR | O_DIRECT, 0o644)
        os.ftruncate(fd, args.blocks * bs)
        fn = _pool_write_slab if direction == "store" else _pool_read_slab
        tasks = [(fd, mv, i * bs, bs) for i in range(args.blocks)]
    else:
        d = os.path.join(args.dir, "bench_pool_files")
        os.makedirs(d, exist_ok=True)
        fn = _pool_write_file if direction == "store" else _pool_read_file
        tasks = [(os.path.join(d, f"{i}.bin"), mv, i * bs, bs)
                 for i in range(args.blocks)]

    t0, c0 = time.perf_counter(), cpu_time()
    # 16+16 是 fs tier 的默认线程数;这里合成一个 32 线程池等价对待单向负载
    with cf.ThreadPoolExecutor(max_workers=args.pool_threads) as ex:
        list(ex.map(lambda t: fn(*t), tasks))
    wall, cpu = time.perf_counter() - t0, cpu_time() - c0

    if slab:
        os.close(fd)
        if direction == "load":
            os.unlink(path)
    elif direction == "load":
        for p, *_ in tasks:
            os.unlink(p)
    # file-per-block 一次写 ≈ open+write+close+replace ≥4 次 syscall, 记 IO 主体
    return dict(wall_s=wall, cpu_s=cpu,
                syscalls=args.blocks * (2 if not slab else 1), failed=0)


# ---------------------------------------------------------------- smoke ----

def smoke(args):
    """写 → 清内存 → 读回 → 校验, 验证引擎数据面正确性(两个 C++ 引擎都过)。"""
    from kv_uring_tier import _kvtier

    n, bs = 64, args.block_bytes
    for name, eng_cls in (("uring", _kvtier.Engine),
                          ("cpp-pool", _kvtier.PoolEngine)):
        mv = make_region(n, bs)
        path = os.path.join(args.dir, f"smoke_{name}.bin")
        # 第 5 个参数对 Engine 是 queue_depth, 对 PoolEngine 是 num_threads,
        # 冒烟不看性能, 都给 64 就行
        eng = eng_cls(mv, path, n, bs, 64, not args.no_odirect)

        assert eng.submit_store(1, list(range(n)), list(range(n)))
        eng.drain()
        (jid, ok), = eng.poll_finished()
        assert jid == 1 and ok, f"{name}: store failed"

        for i in range(n):  # 抹掉内存里的标记
            mv[i * bs : i * bs + 8] = b"\xff" * 8
        assert eng.submit_load(2, list(range(n)), list(range(n)))
        eng.drain()
        (jid, ok), = eng.poll_finished()
        assert jid == 2 and ok, f"{name}: load failed"

        for i in range(n):
            got = int.from_bytes(mv[i * bs : i * bs + 8], "little")
            assert got == i, f"{name}: block {i}: got {got}"
        del eng
        os.unlink(path)
        print(f"SMOKE OK [{name}]: {n} blocks x {bs} B round-trip verified "
              f"(odirect={not args.no_odirect})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="/tmp", help="放测试文件的目录(选实际要测的盘)")
    ap.add_argument("--block-mb", type=float, default=1.0,
                    help="block 大小 MiB(默认 1;应设成目标模型的实际 stride)")
    ap.add_argument("--blocks", type=int, default=1024)
    ap.add_argument("--job-blocks", type=int, default=32,
                    help="uring 引擎一个 job 里的 block 数(≈一次 preempt 的批量)")
    ap.add_argument("--queue-depth", type=int, default=32)
    ap.add_argument("--shard-files", type=int, default=1,
                    help="uring 分片探针: 拆成 N 个引擎+N 个文件(定位单 fd 串行层次)")
    ap.add_argument("--shard-same-file", action="store_true",
                    help="分片对照组: N 个引擎共用同一个文件(同 inode 多 ring)")
    ap.add_argument("--pool-threads", type=int, default=32)
    ap.add_argument("--no-odirect", action="store_true")
    ap.add_argument("--engines", default="uring,cpp-pool,pool,pool-slab")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--smoke", action="store_true")
    args = ap.parse_args()
    args.block_bytes = int(args.block_mb * 1024 * 1024)
    assert args.block_bytes % 4096 == 0, "O_DIRECT 需要 4K 对齐"

    if args.smoke:
        smoke(args)
        return

    total_mb = args.blocks * args.block_bytes / (1 << 20)
    results = []
    for direction in ("store", "load"):
        mv = make_region(args.blocks, args.block_bytes)
        for name in args.engines.split(","):
            if name == "uring":
                r = run_uring(mv, args, direction)
            elif name == "cpp-pool":
                r = run_uring(mv, args, direction, cpp_pool=True)
            elif name == "pool":
                # load 之前得先有文件:pool 的 load 依赖同组 store 留下的文件,
                # 所以按 store→load 顺序跑同一个引擎即可
                r = run_pool(mv, args, direction, slab=False)
            elif name == "pool-slab":
                r = run_pool(mv, args, direction, slab=True)
            else:
                raise SystemExit(f"unknown engine {name}")
            row = dict(engine=name, direction=direction,
                       wall_s=round(r["wall_s"], 3),
                       MB_s=round(total_mb / r["wall_s"], 1),
                       iops=round(args.blocks / r["wall_s"], 1),
                       cpu_util=round(r["cpu_s"] / r["wall_s"], 3),
                       syscalls=r["syscalls"], failed=r["failed"],
                       iou_wrk=r.get("iou_wrk", "-"))
            results.append(row)
            if not args.json:
                print("  ".join(f"{k}={v}" for k, v in row.items()))
    if args.json:
        print(json.dumps(dict(config=vars(args), results=results), indent=2))


if __name__ == "__main__":
    main()
