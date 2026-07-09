# UringSecondaryTierManager — vLLM TieringOffloadingSpec 的 secondary tier 实现。
#
# 分工(镜像 project4 的策略层/数据层切分):
#   本类(策略层, Python):key→slot 账本、LRU 淘汰、in-flight pin、job 记账。
#     全部操作 O(1) dict/OrderedDict, 跑在 scheduler 线程里, 满足接口
#     "lightweight and non-blocking" 的要求。
#   _kvtier.Engine(数据层, C++):SPSC → 单线程 io_uring 批量提交 O_DIRECT IO。
#
# 与 vLLM 的合同(vllm/v1/kv_offload/tiering/base.py):
#   - submit_store: 框架保证 block_ids 对应的 primary slot 在传输期间被 pin,
#     所以我们只需要保护自己这层的 slot 不被自己复用。
#   - submit_load: 框架保证目标 primary slot 已分配好等着写。
#   - 完成通过 get_finished_jobs() 上报;in-flight 期间 has_pending_work()
#     必须为 True, 否则引擎空转时 scheduler 不再来 poll, 完成事件会饿死。
#
# 容量满的处理(设计决策, 面试会被问):
#   磁盘层满 → LRU 逐出未被 pin 的 key;还不够 → 这个 store job 直接报失败。
#   合法性来自 KV cache 的可重算性:任何一层丢数据, 最坏退化成 recompute,
#   正确性不受影响, 只损失延迟。所以三层(GPU/CPU/disk)的满处理全是
#   "逐出或拒绝", 没有任何一层需要"保证放得下"。

from __future__ import annotations

import logging
import os
from collections import OrderedDict
from collections.abc import Collection, Iterable
from typing import TYPE_CHECKING, Any

logger = logging.getLogger(__name__)

try:
    from vllm.v1.kv_offload.base import (
        LookupResult,
        OffloadKey,
        ReqContext,
        RequestOffloadingContext,
    )
    from vllm.v1.kv_offload.tiering.base import (
        JobMetadata,
        JobResult,
        SecondaryTierManager,
    )

    _HAS_VLLM = True
except ImportError:
    # 无 vLLM 环境(Mac 单测 / 独立 bench)用形状对齐的最小替身。
    # 只复刻本文件用到的字段, 不是完整 mock —— 上真机永远走上面的分支。
    import enum
    from dataclasses import dataclass, field

    _HAS_VLLM = False

    class LookupResult(enum.Enum):  # type: ignore[no-redef]
        HIT = enum.auto()
        MISS = enum.auto()
        RETRY = enum.auto()

    OffloadKey = bytes  # type: ignore[misc,assignment]

    @dataclass
    class ReqContext:  # type: ignore[no-redef]
        req_id: str = ""

    @dataclass
    class RequestOffloadingContext:  # type: ignore[no-redef]
        pass

    @dataclass
    class JobMetadata:  # type: ignore[no-redef]
        job_id: int
        keys: Collection[Any]
        block_ids: Any
        is_promotion: bool = False
        req_context: ReqContext = field(default_factory=ReqContext)

    @dataclass
    class JobResult:  # type: ignore[no-redef]
        job_id: int
        success: bool

    class SecondaryTierManager:  # type: ignore[no-redef]
        def __init__(self, offloading_spec, primary_kv_view, tier_type):
            self._offloading_spec = offloading_spec
            self._primary_kv_view = primary_kv_view
            self.tier_type = tier_type


if TYPE_CHECKING:
    from vllm.v1.kv_offload.base import OffloadingSpec


class UringSecondaryTierManager(SecondaryTierManager):
    """io_uring + O_DIRECT 磁盘 secondary tier。

    tier 配置(secondary_tiers 列表里的 dict)::

        {
            "type": "uring",
            "path": "/root/autodl-tmp/kv_tier.bin",
            "disk_bytes_to_use": 21474836480,
            "queue_depth": 32,         # 可选;默认 = 微基准 sweet spot(BENCH_ANALYSIS §2)
            "use_odirect": true        # 可选;对齐不满足时自动降级并告警
        }
    """

    def __init__(
        self,
        offloading_spec: "OffloadingSpec",
        primary_kv_view: memoryview,
        tier_type: str,
        path: str,
        disk_bytes_to_use: int,
        queue_depth: int = 32,
        use_odirect: bool = True,
    ):
        super().__init__(offloading_spec, primary_kv_view, tier_type)

        assert primary_kv_view.strides is not None
        # primary view 的 stride = 一个 offloaded block 的字节数(框架已按
        # PAGESIZE round_up), 也是我们磁盘 slot 的大小 —— 两侧布局镜像,
        # 搬移就是纯 offset 换算, 这是 P4 "换 backing 不换账本"的直接复用。
        self._block_size: int = primary_kv_view.strides[0]
        self._num_slots: int = int(disk_bytes_to_use) // self._block_size
        if self._num_slots <= 0:
            raise ValueError(
                f"disk_bytes_to_use={disk_bytes_to_use} < one block "
                f"({self._block_size} bytes)"
            )
        self._path = path

        self._engine = self._create_engine(
            primary_kv_view, path, queue_depth, use_odirect
        )

        # ---- 账本(全部只在 scheduler 线程里动, 不需要锁) ----
        # 落定在盘上的 key → slot;OrderedDict 顺序 = LRU(尾部最新)
        self._present: OrderedDict[OffloadKey, int] = OrderedDict()
        # store 在途:key → slot(数据还没落盘, lookup 报 RETRY)
        self._storing: dict[OffloadKey, int] = {}
        # load 在途引用计数:key → 在途 load 数(pin 住, 不许逐出)
        self._load_refs: dict[OffloadKey, int] = {}
        self._free: list[int] = list(range(self._num_slots - 1, -1, -1))
        # job_id → (op, keys);op ∈ {"store", "load"}
        self._job_keys: dict[int, tuple[str, list[OffloadKey]]] = {}
        # 没经过引擎就地完成/失败的 job 结果
        self._local_results: list[JobResult] = []

        logger.info(
            "UringSecondaryTierManager: %d slots x %d bytes at %s (odirect=%s)",
            self._num_slots,
            self._block_size,
            path,
            use_odirect,
        )

    # 拆出来是为了单测能换 FakeEngine(见 test/test_manager.py)
    def _create_engine(self, view: memoryview, path: str, queue_depth: int,
                       use_odirect: bool):
        from kv_uring_tier import _kvtier  # .so 只在 Linux 上有

        try:
            return _kvtier.Engine(
                view, path, self._num_slots, self._block_size,
                queue_depth, use_odirect,
            )
        except ValueError:
            if not use_odirect:
                raise
            # 对齐契约不成立(上游布局变了?)——降级走 page cache, 能跑但
            # 会多一次内核 copy, 打点里能看出来。宁可慢不可挂。
            logger.warning(
                "O_DIRECT alignment check failed (block=%d); falling back to "
                "buffered I/O", self._block_size,
            )
            return _kvtier.Engine(
                view, path, self._num_slots, self._block_size,
                queue_depth, False,
            )

    # ------------------------------------------------------------------ #
    # SecondaryTierManager 接口
    # ------------------------------------------------------------------ #

    def lookup(self, key: OffloadKey, req_context: ReqContext) -> LookupResult:
        if key in self._present:
            return LookupResult.HIT
        if key in self._storing:
            # 正在落盘:数据此刻还在 primary(框架 pin 着), 让框架稍后再问
            return LookupResult.RETRY
        return LookupResult.MISS

    def touch(self, keys: Collection[OffloadKey], req_context: ReqContext) -> None:
        for k in keys:
            if k in self._present:
                self._present.move_to_end(k)

    def submit_store(self, job_metadata: JobMetadata) -> None:
        job_id = job_metadata.job_id
        # 已在盘上/在途的 block 不重写(接口要求第 1 条:filter)
        todo = [
            (k, int(bid))
            for k, bid in zip(job_metadata.keys, job_metadata.block_ids)
            if k not in self._present and k not in self._storing
        ]
        if not todo:
            self._local_results.append(JobResult(job_id=job_id, success=True))
            return

        slots = self._take_slots(len(todo))
        if slots is None:
            # 满且逐无可逐(全被 in-flight pin 住)→ 拒绝这个 job。
            # 上层的兜底是这些 block 留在 primary 或被丢弃后 recompute。
            self._local_results.append(JobResult(job_id=job_id, success=False))
            return

        keys = [k for k, _ in todo]
        bids = [b for _, b in todo]
        for k, s in zip(keys, slots):
            self._storing[k] = s
        if not self._engine.submit_store(job_id, slots, bids):
            # 引擎入口环满(1023 个在途 job 才会发生):回滚并报失败
            for k in keys:
                self._free.append(self._storing.pop(k))
            self._local_results.append(JobResult(job_id=job_id, success=False))
            return
        self._job_keys[job_id] = ("store", keys)

    def submit_load(self, job_metadata: JobMetadata) -> None:
        job_id = job_metadata.job_id
        pairs = list(zip(job_metadata.keys, job_metadata.block_ids))
        if any(k not in self._present for k, _ in pairs):
            # lookup 已经报过 MISS/RETRY, 正常流程走不到;防御性失败
            self._local_results.append(JobResult(job_id=job_id, success=False))
            return

        keys = [k for k, _ in pairs]
        slots = [self._present[k] for k in keys]
        bids = [int(b) for _, b in pairs]
        for k in keys:
            self._load_refs[k] = self._load_refs.get(k, 0) + 1
            self._present.move_to_end(k)  # 命中即热
        if not self._engine.submit_load(job_id, slots, bids):
            for k in keys:
                self._unpin(k)
            self._local_results.append(JobResult(job_id=job_id, success=False))
            return
        self._job_keys[job_id] = ("load", keys)

    def get_finished_jobs(self) -> Iterable[JobResult]:
        results = self._local_results
        self._local_results = []
        for job_id, ok in self._engine.poll_finished():
            op, keys = self._job_keys.pop(job_id)
            if op == "store":
                for k in keys:
                    slot = self._storing.pop(k)
                    if ok:
                        self._present[k] = slot  # 插入即 LRU 最新
                    else:
                        self._free.append(slot)
            else:
                for k in keys:
                    self._unpin(k)
                if not ok:
                    # 读失败说明盘上数据可疑(短读/介质错):整批逐出,
                    # 下次这些 key 直接 MISS → recompute, 不再踩同一个坑
                    for k in keys:
                        if k in self._present and k not in self._load_refs:
                            self._free.append(self._present.pop(k))
            results.append(JobResult(job_id=job_id, success=ok))
        return results

    def has_pending_work(self) -> bool:
        return bool(self._job_keys) or bool(self._local_results)

    def on_new_request(self, req_context: ReqContext) -> RequestOffloadingContext:
        return RequestOffloadingContext()

    def drain_jobs(self) -> None:
        self._engine.drain()

    def shutdown(self) -> None:
        self._engine.drain()
        # cache tier 语义:内容不跨进程生命周期保留(同 P4 SsdBlockStore 析构
        # unlink 的决定)。要做持久化 KV 池是另一个项目。
        try:
            os.remove(self._path)
        except OSError:
            pass

    def get_stats(self):
        return None  # Prometheus 接入是后续工作;引擎侧 stats 见 engine_stats()

    # 给 bench / 调试用:引擎内部计数器
    def engine_stats(self) -> dict:
        return dict(self._engine.stats())

    # ------------------------------------------------------------------ #
    # 内部
    # ------------------------------------------------------------------ #

    def _unpin(self, key: OffloadKey) -> None:
        n = self._load_refs.get(key, 0) - 1
        if n <= 0:
            self._load_refs.pop(key, None)
        else:
            self._load_refs[key] = n

    def _take_slots(self, n: int) -> list[int] | None:
        """拿 n 个空 slot;free 不够就从 LRU 头逐出未 pin 的 key。

        拿不够时把已拿的放回去, 返回 None(调用方整个 job 报失败 ——
        不做部分写入, 让 job 语义保持全有或全无)。
        """
        slots: list[int] = []
        while len(slots) < n and self._free:
            slots.append(self._free.pop())
        if len(slots) < n:
            for k in list(self._present.keys()):
                if len(slots) >= n:
                    break
                if k in self._load_refs:
                    continue  # 有 load 在读它的 slot, 不能动
                slots.append(self._present.pop(k))
        if len(slots) < n:
            self._free.extend(slots)
            return None
        return slots
