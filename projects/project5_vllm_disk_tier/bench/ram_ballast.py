#!/usr/bin/env python3
"""RAM ballast:占住匿名内存不放, 把留给 page cache 的空间挤掉。

用途是构造 E1 的"冷"条件(COMPARE_PLAN E1 节):fs tier 走 buffered 读,
只要 page cache 还放得下工作集, 它的"读盘"其实是在读 RAM;把 cache 的
生存空间压到小于工作集, buffered 读才被逼回真实设备。AutoDL 是容器,
drop_caches 和 cgroup 都没权限, 占内存是唯一可用的挤压手段。

原理:匿名脏页没有 backing file, 容器里通常又没 swap, 内核回收内存时
动不了它, 只能拿 page cache 开刀 —— ballast 占多少, page cache 的生存
空间就少多少。有 swap 的机器上匿名页可能被换出去"漏气", 所以填完后
尽力 mlock 一把;锁不上(RLIMIT_MEMLOCK 不够)只告警不退出, 靠
/proc/swaps 的检查提示人工确认。

两种用法:

    # 自适应(推荐, overnight 脚本用这个): 填到 cgroup 配额里留给
    # page cache 的余量 ≤ 4 GiB 为止。容器场景下 page cache 记账在
    # 自己的 cgroup 头上(谁读的文件算谁的), 所以判据要对着 cgroup 的
    # 配额算, 不是 /proc/meminfo 的宿主数字 —— AutoDL 上 free 显示的是
    # 宿主 1TB, 容器配额可能只有 120G, 差一个数量级。
    # serve 就绪后再起它: serve 的匿名内存(CPU tier pinned 等)先就位,
    # "还剩多少给 cache"才算得准, 也不跟模型加载抢内存。
    python3 bench/ram_ballast.py --cache-room-gib 4 &

    # 固定量: 抱住 24 GiB, 适合手工实验
    python3 bench/ram_ballast.py --gib 24 &

    # stdout 出现 "BALLAST READY" 才算生效(overnight 脚本靠这行等它)
    kill %1        # 用完释放(进程死即归还)
"""

from __future__ import annotations

import argparse
import ctypes
import mmap
import os
import signal
import sys
import time

GIB = 1 << 30
CHUNK = 16 << 20   # 16MiB 一笔写满, python 层循环次数少, 填 24G 也就几秒


def meminfo_mb(key: str) -> int | None:
    """从 /proc/meminfo 读一个字段(kB), 换算成 MB;非 Linux 返回 None。"""
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith(key + ":"):
                    return int(line.split()[1]) // 1024
    except OSError:
        pass
    return None


def cgroup_mem() -> dict | None:
    """读本容器 cgroup 的内存记账(v2 优先, 回落 v1), 单位字节。

    返回 {"limit", "anon", "shmem"};没上限或读不到返回 None。
    留给 page cache 的余量 = limit - anon - shmem:匿名页和 tmpfs 都是
    回收不掉的硬占用, 剩下的空间才轮得到文件缓存住。
    """
    try:
        if os.path.exists("/sys/fs/cgroup/memory.max"):        # v2
            raw = open("/sys/fs/cgroup/memory.max").read().strip()
            if raw == "max":
                return None
            st = _read_kv("/sys/fs/cgroup/memory.stat")
            return {"limit": int(raw), "anon": st["anon"],
                    "shmem": st.get("shmem", 0)}
        base = "/sys/fs/cgroup/memory/"                        # v1
        limit = int(open(base + "memory.limit_in_bytes").read())
        if limit >= 1 << 60:      # v1 没设上限时是 PAGE_COUNTER_MAX 天文数字
            return None
        st = _read_kv(base + "memory.stat")
        return {"limit": limit, "anon": st["total_rss"],
                "shmem": st.get("total_shmem", 0)}
    except (OSError, KeyError, ValueError):
        return None


def _read_kv(path: str) -> dict:
    out = {}
    with open(path) as f:
        for line in f:
            k, _, v = line.partition(" ")
            out[k] = int(v)
    return out


def swap_active() -> bool:
    try:
        with open("/proc/swaps") as f:
            return len(f.readlines()) > 1   # 首行是表头
    except OSError:
        return False


def try_mlock(buf: mmap.mmap, nbytes: int) -> bool:
    """尽力 mlock;失败(通常是 RLIMIT_MEMLOCK)返回 False, 不算致命。"""
    try:
        libc = ctypes.CDLL(None, use_errno=True)
        addr = ctypes.addressof(ctypes.c_char.from_buffer(buf))
        return libc.mlock(ctypes.c_void_p(addr), ctypes.c_size_t(nbytes)) == 0
    except (OSError, ValueError):
        return False


def alloc_touched(nbytes: int) -> mmap.mmap:
    """mmap 一块匿名内存并逐块真写:匿名页懒分配, 不写不占物理页。"""
    buf = mmap.mmap(-1, nbytes)
    zeros = b"\0" * CHUNK
    written = 0
    while written < nbytes:
        n = min(CHUNK, nbytes - written)
        buf[written:written + n] = zeros[:n]
        written += n
    return buf


def cache_room_bytes() -> int:
    cg = cgroup_mem()
    if cg is None:
        sys.exit("cgroup 内存上限读不到(或没设上限), --cache-room-gib 用不了; "
                 "退回 --gib 固定量模式")
    return cg["limit"] - cg["anon"] - cg["shmem"]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--gib", type=float,
                    help="固定量模式: 要抱住的匿名内存量(GiB)")
    ap.add_argument("--cache-room-gib", type=float,
                    help="自适应模式: 填到 cgroup 配额里留给 page cache "
                         "的余量 ≤ 这个值(GiB)为止")
    args = ap.parse_args()
    if (args.gib is None) == (args.cache_room_gib is None):
        ap.error("--gib 和 --cache-room-gib 二选一")

    avail0 = meminfo_mb("MemAvailable")

    # SIGTERM/SIGINT 干净退出;内存本身进程一死就归还, handler 只为退出码
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda *_: sys.exit(0))

    bufs = []           # 抱住引用防 GC;自适应模式是一串 1GiB 块
    total = 0
    t0 = time.monotonic()
    if args.gib is not None:
        total = int(args.gib * GIB)
        bufs.append(alloc_touched(total))
        tag = f"fixed {args.gib:g} GiB"
    else:
        # 每次咬 1GiB 再重新读余量:我们自己的匿名页也记在 anon 里,
        # 一边填一边收敛, 不需要预估 serve 占多少。填充会逼内核回收
        # cache, 越到后面越慢, 这是预期行为不是卡死。
        target = int(args.cache_room_gib * GIB)
        room0 = cache_room_bytes()
        while True:
            room = cache_room_bytes()
            if room <= target:
                break
            step = min(GIB, room - target)
            bufs.append(alloc_touched(step))
            total += step
            if total % (16 * GIB) < GIB:
                print(f"  已填 {total / GIB:.0f} GiB, cache 余量 "
                      f"{room / GIB:.1f} GiB → 目标 {target / GIB:.1f} GiB",
                      flush=True)
        tag = (f"cache-room {room0 / GIB:.1f} -> {cache_room_bytes() / GIB:.1f}"
               f" GiB (目标 {args.cache_room_gib:g}, 填了 {total / GIB:.1f} GiB)")

    locked = all(try_mlock(b, len(b)) for b in bufs) if bufs else False
    if not locked and swap_active():
        print("警告: mlock 失败且系统有 swap, 匿名页可能被换出, "
              "压制效果要盯着 cgroup 记账确认", flush=True)

    avail1 = meminfo_mb("MemAvailable")
    print(f"BALLAST READY {tag}, 填充耗时 "
          f"{time.monotonic() - t0:.1f}s, mlock={'ok' if locked else 'no'}, "
          f"MemAvailable {avail0} -> {avail1} MB", flush=True)

    # 抱住不放, 直到被 kill;短 sleep 让信号来得及处理
    while True:
        time.sleep(5)


if __name__ == "__main__":
    main()
