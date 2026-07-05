# 策略层单测:UringSecondaryTierManager 的账本逻辑(slot 分配 / LRU 逐出 /
# in-flight pin / job 记账)。数据层用 FakeEngine 替掉, 所以 Mac 上就能跑:
#
#   cd projects/project5_vllm_disk_tier && python3 -m pytest test/ -v
#
# 引擎本体(io_uring 数据面)的验证在 GPU 机器上:
#   make && python3 bench/bench_engine.py --smoke

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))

from kv_uring_tier.manager import (  # noqa: E402
    JobMetadata,
    LookupResult,
    UringSecondaryTierManager,
)

BLOCK = 4096
NUM_SLOTS = 4


class FakeEngine:
    """接口同 _kvtier.Engine, 完成事件手动驱动(complete_all)。"""

    def __init__(self):
        self.jobs = []          # (job_id, op, slots, bids)
        self.finished = []      # (job_id, ok)
        self.reject_next = False

    def submit_store(self, job_id, slots, bids):
        return self._submit(job_id, "store", slots, bids)

    def submit_load(self, job_id, slots, bids):
        return self._submit(job_id, "load", slots, bids)

    def _submit(self, job_id, op, slots, bids):
        if self.reject_next:
            self.reject_next = False
            return False
        self.jobs.append((job_id, op, list(slots), list(bids)))
        return True

    def complete_all(self, ok=True):
        for job_id, *_ in self.jobs:
            self.finished.append((job_id, ok))
        self.jobs = []

    def poll_finished(self, max_n=1024):
        out, self.finished = self.finished, []
        return out

    def drain(self):
        self.complete_all()

    def stats(self):
        return {}


class MgrWithFakeEngine(UringSecondaryTierManager):
    def _create_engine(self, view, path, queue_depth, use_odirect):
        return FakeEngine()


@pytest.fixture
def mgr():
    # 2D memoryview: strides[0] = BLOCK, 模拟 vLLM primary view 的形状
    buf = memoryview(bytearray(8 * BLOCK)).cast("B", (8, BLOCK))
    return MgrWithFakeEngine(
        offloading_spec=None,
        primary_kv_view=buf,
        tier_type="uring",
        path="/tmp/unused.bin",
        disk_bytes_to_use=NUM_SLOTS * BLOCK,
    )


def store(mgr, job_id, keys, bids):
    mgr.submit_store(JobMetadata(job_id=job_id, keys=keys, block_ids=bids,
                                 is_promotion=False, req_context=None))


def load(mgr, job_id, keys, bids):
    mgr.submit_load(JobMetadata(job_id=job_id, keys=keys, block_ids=bids,
                                is_promotion=True, req_context=None))


def finish(mgr, ok=True):
    mgr._engine.complete_all(ok)
    return {r.job_id: r.success for r in mgr.get_finished_jobs()}


def test_store_then_hit(mgr):
    assert mgr.lookup(b"k1", None) == LookupResult.MISS
    store(mgr, 1, [b"k1", b"k2"], [0, 1])
    # 在途:RETRY, 完成事件还没上报
    assert mgr.lookup(b"k1", None) == LookupResult.RETRY
    assert mgr.has_pending_work()
    results = finish(mgr)
    assert results == {1: True}
    assert mgr.lookup(b"k1", None) == LookupResult.HIT
    assert not mgr.has_pending_work()


def test_store_dedup_already_present(mgr):
    store(mgr, 1, [b"k1"], [0])
    finish(mgr)
    # k1 已在盘上 → 过滤后 job 为空, 本地立即成功, 不再打引擎
    store(mgr, 2, [b"k1"], [0])
    assert mgr._engine.jobs == []
    results = {r.job_id: r.success for r in mgr.get_finished_jobs()}
    assert results == {2: True}


def test_lru_eviction_when_full(mgr):
    for i in range(NUM_SLOTS):
        store(mgr, i, [f"k{i}".encode()], [i % 8])
    finish(mgr)
    mgr.touch([b"k0"], None)  # k0 变最新, 最老的变成 k1
    store(mgr, 100, [b"new"], [0])
    finish(mgr)
    assert mgr.lookup(b"k1", None) == LookupResult.MISS   # 被逐出
    assert mgr.lookup(b"k0", None) == LookupResult.HIT    # touch 保住了
    assert mgr.lookup(b"new", None) == LookupResult.HIT


def test_load_pins_slot_against_eviction(mgr):
    for i in range(NUM_SLOTS):
        store(mgr, i, [f"k{i}".encode()], [i % 8])
    finish(mgr)
    load(mgr, 50, [b"k0"], [0])           # k0 有在途 load, pin 住
    store(mgr, 100, [b"new"], [1])        # 满 → 需要逐出, 但必须跳过 k0
    finish(mgr)
    assert mgr.lookup(b"k0", None) == LookupResult.HIT
    assert mgr.lookup(b"new", None) == LookupResult.HIT


def test_store_fails_when_nothing_evictable(mgr):
    for i in range(NUM_SLOTS):
        store(mgr, i, [f"k{i}".encode()], [i % 8])
    finish(mgr)
    for i in range(NUM_SLOTS):           # 全部 pin 住
        load(mgr, 50 + i, [f"k{i}".encode()], [i % 8])
    store(mgr, 100, [b"new"], [0])       # 逐无可逐 → 立即失败
    results = {r.job_id: r.success for r in mgr.get_finished_jobs()}
    assert results[100] is False
    # 账本没被污染:失败的 job 不占 slot
    assert len(mgr._free) == 0 and len(mgr._present) == NUM_SLOTS


def test_load_missing_key_fails(mgr):
    load(mgr, 1, [b"ghost"], [0])
    results = {r.job_id: r.success for r in mgr.get_finished_jobs()}
    assert results == {1: False}


def test_failed_store_returns_slots(mgr):
    store(mgr, 1, [b"k1"], [0])
    results = finish(mgr, ok=False)
    assert results == {1: False}
    assert mgr.lookup(b"k1", None) == LookupResult.MISS
    assert len(mgr._free) == NUM_SLOTS   # slot 全回来了


def test_failed_load_evicts_suspect_data(mgr):
    store(mgr, 1, [b"k1"], [0])
    finish(mgr)
    load(mgr, 2, [b"k1"], [0])
    results = finish(mgr, ok=False)
    assert results == {2: False}
    # 读失败 → 盘上数据可疑, 逐出, 下次 MISS → recompute
    assert mgr.lookup(b"k1", None) == LookupResult.MISS


def test_engine_ring_full_rolls_back(mgr):
    mgr._engine.reject_next = True
    store(mgr, 1, [b"k1"], [0])
    results = {r.job_id: r.success for r in mgr.get_finished_jobs()}
    assert results == {1: False}
    assert len(mgr._free) == NUM_SLOTS
    assert b"k1" not in mgr._storing
