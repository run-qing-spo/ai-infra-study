#!/usr/bin/env bash
# 把 syscall 轴的口径补齐:Python 的 pool / pool-slab 两组在 bench_engine.py 里
# 是拍脑袋估的(`blocks * (2 if not slab else 1)`), 两个 C++ 引擎却是引擎内
# atomic 计数器实测的。"我方实测、对方估算"站不住, 这里用 strace 把两个
# Python 组也实测一遍。
#
# 只为拿计数, 不进性能账 —— strace 每个 syscall 陷一次 ptrace, 墙钟和 CPU
# 全部作废, 那两个轴照旧看 bench_engine.py 的原生跑法。
#
# 预测(1024 blocks, 写下来好让实测去证伪):
#   pool      store: openat+write+close+rename = 4/block
#             load : openat+readv+close        = 3/block   → 合计 7168
#             bench 估的是 2048 → 低估 3.5 倍
#   pool-slab store: pwrite 1/block
#             load : preadv 1/block            → 合计 2048
#             bench 每方向估 1/block → 正好对上
#
# 用法: bash bench/strace_syscalls.sh /root/autodl-tmp [blocks]

set -euo pipefail

DIR="${1:?用法: $0 <测试目录> [blocks]}"
BLOCKS="${2:-1024}"
HERE="$(cd "$(dirname "$0")" && pwd)"

command -v strace >/dev/null || { echo "没装 strace: apt install strace"; exit 1; }

# -f 跟子线程:线程池的 IO 全发生在 worker 线程上, 不加 -f 只能数到主线程。
# -c 只出汇总表。-e 限定 IO 相关的调用, 滤掉解释器自己的 mmap/brk/futex ——
# 那些数量不稳定(取决于 GC 时机和线程争用), 混进来没法横向比。
TRACE='openat,open,write,pwrite64,writev,pwritev,read,pread64,readv,preadv,close,rename,renameat,renameat2,ftruncate,unlink,unlinkat'

for eng in pool pool-slab; do
  echo "=============== $eng (blocks=$BLOCKS) ==============="
  strace -f -c -e "trace=$TRACE" \
    python3 "$HERE/bench_engine.py" \
      --dir "$DIR" --blocks "$BLOCKS" --block-mb 1 --engines "$eng" \
    2>&1 | tail -n 30
  echo
done

echo "读法:calls 一列除以 $BLOCKS 就是每 block 的 syscall 数(store+load 合计)。"
echo "unlink/unlinkat 是 bench 收尾删文件, 不算 IO 路径, 减掉。"
