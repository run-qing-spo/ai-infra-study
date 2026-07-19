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

import json
import logging
import os
import time
from collections import Counter, OrderedDict
from collections.abc import Collection, Iterable
from typing import TYPE_CHECKING, Any

# 必须挂进 vLLM 的日志体系: EngineCore 子进程里 root logger 是默认的
# WARNING + 无 handler, 裸 getLogger 的 INFO/WARNING 全被吞 —— E1 那轮
# serve 日志里看不到 prewarm 行、更糟的是 O_DIRECT 降级警报也不可见,
# 就是这个原因。注意光换成 init_logger(__name__) 还不够(E1 第三轮实证):
# vLLM 的 logging config 只给 "vllm" 这棵层级树挂 handler, logger 名字
# 必须以 vllm. 开头才继承得到, 所以这里强行冠名。裸跑单测时没有 vllm,
# 退回标准 logging。
try:
    from vllm.logger import init_logger
    logger = init_logger("vllm." + __name__)
except ImportError:
    logger = logging.getLogger(__name__)

try:
    import vllm  # noqa: F401

    _HAS_VLLM = True
except ImportError:
    _HAS_VLLM = False

if _HAS_VLLM:
    # vLLM 在场时名字对不上必须当场炸在 import(= serve 启动阶段), 不许
    # 静默滑进下面的替身分支 —— 2026-07-10 踩坑: vLLM 删掉 LookupResult
    # (lookup 改返回 bool|None)后, 旧的整块 try/except 把 ImportError 吞
    # 成替身, RequestOffloadingContext 变空壳, 直到第一个请求进来才炸
    # AttributeError, 定位绕了一大圈。
    from vllm.v1.kv_offload.tiering.base import (
        JobMetadata,
        JobResult,
        OffloadKey,
        ReqContext,
        RequestOffloadingContext,
        SecondaryTierManager,
    )
else:
    # 无 vLLM 环境(Mac 单测 / 独立 bench)用形状对齐的最小替身。
    # 只复刻本文件用到的字段, 不是完整 mock —— 上真机永远走上面的分支。
    from dataclasses import dataclass, field

    OffloadKey = bytes  # type: ignore[misc,assignment]

    @dataclass
    class ReqContext:  # type: ignore[no-redef]
        req_id: str = ""

    @dataclass
    class RequestOffloadingContext:  # type: ignore[no-redef]
        # 真身的 policy 是 OffloadPolicy 枚举(默认 BLOCK_LEVEL);替身只
        # 对齐"有这个字段", 值不参与单测断言
        policy: Any = None

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
            "use_odirect": true,       # 可选;对齐不满足时自动降级并告警
            "prewarm": true            # 可选;启动时预写整个 backing 文件, 把
                                       # fallocate 的 unwritten extent 全部翻成
                                       # written, 消除首写 NOWAIT 失败 → io-wq
                                       # punt(BENCH_ANALYSIS §3 dd 预写的内化)。
                                       # 代价是启动多花 文件大小/盘写带宽 的时间
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
        prewarm: bool = True,
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
        # 走实例属性而不是加参数:_create_engine 的签名被单测的 FakeEngine
        # 覆写钉住了, 别为一个开关破坏那个接缝
        self._prewarm = prewarm

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

        # ---- E2 观测账本(COMPARE_PLAN E2 节的三件工具缺口) ----
        # 唯一块口径:_ever_stored 只进不出(逐出也不删), 于是
        # counters["store_blocks_first"] 的累计值 = 累计唯一块数 —— 和 fs 的
        # 文件账同口径(fs 无逐出, 文件集合天然 = 唯一块集合)。引擎的
        # bytes_written 是物理口径, 含逐出后重写(E3 的 2.9x 写放大就混在
        # 里面), 不能当吸收率的分子。
        self._ever_stored: set[OffloadKey] = set()
        # 漏斗计数:offered → filtered / 容量拒 / 环满拒 → 首写 / 重写 / IO 失败。
        # "上层静默丢块"在这层数不到(丢弃发生在 vLLM 侧, 调用根本不来),
        # 证伪靠两头对账:bench 算的 churn 产出块数 vs 这里的 offered。
        self._counters: Counter = Counter()
        # 引擎 per-job 账本的 Python 侧缓存(drain_records 批量取走后攒着)
        self._job_records: list[tuple] = []
        # 初始化成"现在", 让第一次落盘发生在启动 _DUMP_INTERVAL_S 之后 ——
        # 单测(毫秒级)永远不会触发周期 dump, 不往 /tmp 撒文件
        self._last_dump: float = time.monotonic()
        self._dump_warned: bool = False
        # 忘了重编 .so 就当场炸在 serve 启动, 别让 E2 跑完一轮才发现账本是空的
        if not hasattr(self._engine, "drain_records"):
            raise RuntimeError(
                "engine has no drain_records(): stale _kvtier .so, rebuild (make)"
            )

        logger.info(
            "UringSecondaryTierManager: %d slots x %d bytes at %s "
            "(odirect=%s, prewarm=%s)",
            self._num_slots,
            self._block_size,
            path,
            use_odirect,
            prewarm,
        )

    # 拆出来是为了单测能换 FakeEngine(见 test/test_manager.py)
    def _create_engine(self, view: memoryview, path: str, queue_depth: int,
                       use_odirect: bool):
        from kv_uring_tier import _kvtier  # .so 只在 Linux 上有

        try:
            return _kvtier.Engine(
                view, path, self._num_slots, self._block_size,
                queue_depth, use_odirect, self._prewarm,
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
                queue_depth, False, self._prewarm,
            )

    # ------------------------------------------------------------------ #
    # SecondaryTierManager 接口
    # ------------------------------------------------------------------ #

    # 返回值合同(tiering/base.py): True=在盘上, False=不在, None=在途稍后再问
    def lookup(self, key: OffloadKey, req_context: ReqContext) -> bool | None:
        # 三态计数是 promotion 对账的一半:lookup 答过 hit 的块数减去实际
        # 来的 load 块数 = 上层放弃的 promotion(CPU tier 满时 vLLM 的
        # _initiate_promotion 直接判 MISS, 不会调到这层)。
        if key in self._present:
            self._counters["lookup_hit"] += 1
            return True
        if key in self._storing:
            # 正在落盘:数据此刻还在 primary(框架 pin 着), 让框架稍后再问
            self._counters["lookup_retry"] += 1
            return None
        self._counters["lookup_miss"] += 1
        return False

    def touch(self, keys: Collection[OffloadKey], req_context: ReqContext) -> None:
        for k in keys:
            if k in self._present:
                self._present.move_to_end(k)

    def submit_store(self, job_metadata: JobMetadata) -> None:
        job_id = job_metadata.job_id
        self._counters["store_blocks_offered"] += len(job_metadata.keys)
        # 已在盘上/在途的 block 不重写(接口要求第 1 条:filter)
        todo = [
            (k, int(bid))
            for k, bid in zip(job_metadata.keys, job_metadata.block_ids)
            if k not in self._present and k not in self._storing
        ]
        self._counters["store_blocks_filtered"] += (
            len(job_metadata.keys) - len(todo)
        )
        if not todo:
            self._local_results.append(JobResult(job_id=job_id, success=True))
            return

        slots = self._take_slots(len(todo))
        if slots is None:
            # 满且逐无可逐(全被 in-flight pin 住)→ 拒绝这个 job。
            # 上层的兜底是这些 block 留在 primary 或被丢弃后 recompute。
            self._counters["store_jobs_rejected_capacity"] += 1
            self._counters["store_blocks_rejected_capacity"] += len(todo)
            self._local_results.append(JobResult(job_id=job_id, success=False))
            return

        keys = [k for k, _ in todo]
        bids = [b for _, b in todo]
        for k, s in zip(keys, slots):
            self._storing[k] = s
        if not self._engine.submit_store(job_id, slots, bids):
            # 引擎入口环满(1023 个在途 job 才会发生):回滚并报失败
            self._counters["store_jobs_rejected_ring"] += 1
            self._counters["store_blocks_rejected_ring"] += len(keys)
            for k in keys:
                self._free.append(self._storing.pop(k))
            self._local_results.append(JobResult(job_id=job_id, success=False))
            return
        self._job_keys[job_id] = ("store", keys)

    def submit_load(self, job_metadata: JobMetadata) -> None:
        job_id = job_metadata.job_id
        pairs = list(zip(job_metadata.keys, job_metadata.block_ids))
        # promotion 对账的另一半。is_promotion 直接访问不 getattr 兜底:
        # 真身没这个字段就当场炸(同 import 段"名字对不上不许静默滑过"的
        # 纪律), 静默记 0 会把 E2 的丢块分析带偏。
        self._counters["load_jobs"] += 1
        self._counters["load_blocks"] += len(pairs)
        if job_metadata.is_promotion:
            self._counters["load_jobs_promotion"] += 1
            self._counters["load_blocks_promotion"] += len(pairs)
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
                        # 唯一块判定只认"成功落上盘"这一刻:first = 一生第一次,
                        # rewrite = 逐出后回来重写(容量逼出来的物理重复写)
                        if k in self._ever_stored:
                            self._counters["store_blocks_rewrite"] += 1
                        else:
                            self._ever_stored.add(k)
                            self._counters["store_blocks_first"] += 1
                        self._present[k] = slot  # 插入即 LRU 最新
                    else:
                        self._counters["store_blocks_io_failed"] += 1
                        self._free.append(slot)
            else:
                for k in keys:
                    self._unpin(k)
                if not ok:
                    self._counters["load_blocks_io_failed"] += len(keys)
                    # 读失败说明盘上数据可疑(短读/介质错):整批逐出,
                    # 下次这些 key 直接 MISS → recompute, 不再踩同一个坑
                    for k in keys:
                        if k in self._present and k not in self._load_refs:
                            self._free.append(self._present.pop(k))
                            self._counters["evicted_after_load_fail"] += 1
            results.append(JobResult(job_id=job_id, success=ok))
        self._maybe_dump()
        return results

    def has_pending_work(self) -> bool:
        return bool(self._job_keys) or bool(self._local_results)

    def on_new_request(self, req_context: ReqContext) -> RequestOffloadingContext:
        return RequestOffloadingContext()

    def drain_jobs(self) -> None:
        self._engine.drain()

    def shutdown(self) -> None:
        self._engine.drain()
        # 账本终写要在删 backing 之前;stats 文件本身不删, bench 收尾要收走
        self._dump_stats()
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
    # E2 账本出口
    # ------------------------------------------------------------------ #
    # serve 场景下本对象活在 EngineCore 子进程里, bench 拿不到 Python 对象,
    # 账本唯一的出口是文件(<backing>.stats.json)。周期覆盖写 + shutdown
    # 终写:优雅关停(--shutdown-timeout 30)拿全量;被 SIGKILL 时最多丢
    # 最后一个周期 —— /dev/shm 泄漏那课的直接应用:留档义务不能只挂在
    # 优雅路径上。

    _DUMP_INTERVAL_S = 10.0

    # drain_records() 返回 tuple, 字段序由 pybind 侧定死, 这里是唯一的
    # schema 权威;分析脚本按它解列, 不许自己猜下标
    _RECORD_SCHEMA = [
        "job_id", "is_write", "n_blocks",
        "t_submit", "t_first_issue", "t_done",
        "q_jobs_at_submit", "dev_inflight_at_issue",
    ]

    def tier_stats(self) -> dict:
        return {
            "t_wall": time.time(),
            "block_size": self._block_size,
            "num_slots": self._num_slots,
            "counters": dict(self._counters),
            "gauges": {
                "present_blocks": len(self._present),
                "storing_blocks": len(self._storing),
                "load_ref_keys": len(self._load_refs),
                "free_slots": len(self._free),
                "ever_stored_blocks": len(self._ever_stored),
                "inflight_jobs": len(self._job_keys),
            },
            "engine": self.engine_stats(),
        }

    def _maybe_dump(self) -> None:
        if time.monotonic() - self._last_dump < self._DUMP_INTERVAL_S:
            return
        self._dump_stats()

    def _dump_stats(self) -> None:
        self._last_dump = time.monotonic()
        self._job_records.extend(self._engine.drain_records())
        payload = self.tier_stats()
        payload["job_records"] = {
            "schema": self._RECORD_SCHEMA,
            "rows": self._job_records,
        }
        path = self._path + ".stats.json"
        try:
            tmp = path + ".tmp"
            with open(tmp, "w") as f:
                json.dump(payload, f)
            os.replace(tmp, path)  # 原子替换:分析脚本永远读不到半截 JSON
        except OSError as e:
            # dump 是观测不是功能, 不许把 scheduler 线程炸掉;只告警一次
            if not self._dump_warned:
                self._dump_warned = True
                logger.warning("tier stats dump to %s failed: %s", path, e)

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
                # 计数放 pop 处:即使本 job 最终拿不够被拒, 这些 key 也真的
                # 从盘上账本消失了(原逻辑如此 —— slot 进 free, key 不回来)
                self._counters["evicted_blocks"] += 1
        if len(slots) < n:
            self._free.extend(slots)
            return None
        return slots
