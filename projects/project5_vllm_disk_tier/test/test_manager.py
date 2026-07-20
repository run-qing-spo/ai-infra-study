# 策略层单测:UringSecondaryTierManager 的账本逻辑(slot 分配 / LRU 逐出 /
# in-flight pin / job 记账)。数据层用 FakeEngine 替掉, 所以 Mac 上就能跑:
#
#   cd projects/project5_vllm_disk_tier && python3 -m pytest test/ -v
#
# 引擎本体(io_uring 数据面)的验证在 GPU 机器上:
#   make && python3 bench/bench_engine.py --smoke

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))

from kv_uring_tier.manager import (  # noqa: E402
    JobMetadata,
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

    def drain_records(self):
        # 真引擎的 per-job 账本在 C++ 侧;策略层单测只要求接口在
        # (manager 构造时 hasattr 检查, 忘重编 .so 会当场炸)
        return []


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
    assert mgr.lookup(b"k1", None) is False
    store(mgr, 1, [b"k1", b"k2"], [0, 1])
    # 在途:None(=稍后再问), 完成事件还没上报
    assert mgr.lookup(b"k1", None) is None
    assert mgr.has_pending_work()
    results = finish(mgr)
    assert results == {1: True}
    assert mgr.lookup(b"k1", None) is True
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
    assert mgr.lookup(b"k1", None) is False   # 被逐出
    assert mgr.lookup(b"k0", None) is True    # touch 保住了
    assert mgr.lookup(b"new", None) is True


def test_load_pins_slot_against_eviction(mgr):
    for i in range(NUM_SLOTS):
        store(mgr, i, [f"k{i}".encode()], [i % 8])
    finish(mgr)
    load(mgr, 50, [b"k0"], [0])           # k0 有在途 load, pin 住
    store(mgr, 100, [b"new"], [1])        # 满 → 需要逐出, 但必须跳过 k0
    finish(mgr)
    assert mgr.lookup(b"k0", None) is True
    assert mgr.lookup(b"new", None) is True


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
    assert mgr.lookup(b"k1", None) is False
    assert len(mgr._free) == NUM_SLOTS   # slot 全回来了


def test_failed_load_evicts_suspect_data(mgr):
    store(mgr, 1, [b"k1"], [0])
    finish(mgr)
    load(mgr, 2, [b"k1"], [0])
    results = finish(mgr, ok=False)
    assert results == {2: False}
    # 读失败 → 盘上数据可疑, 逐出, 下次 MISS(False) → recompute
    assert mgr.lookup(b"k1", None) is False


def test_engine_ring_full_rolls_back(mgr):
    mgr._engine.reject_next = True
    store(mgr, 1, [b"k1"], [0])
    results = {r.job_id: r.success for r in mgr.get_finished_jobs()}
    assert results == {1: False}
    assert len(mgr._free) == NUM_SLOTS
    assert b"k1" not in mgr._storing


def test_counter_funnel(mgr):
    # E2 账本:offered → filtered/rejected → first/rewrite 的漏斗必须闭合,
    # 这是吸收率对比(uring vs fs 文件账)的口径基础
    for i in range(NUM_SLOTS):
        store(mgr, i, [f"k{i}".encode()], [i % 8])
    finish(mgr)
    c = mgr._counters
    assert c["store_blocks_offered"] == NUM_SLOTS
    assert c["store_blocks_first"] == NUM_SLOTS

    store(mgr, 10, [b"k0"], [0])            # 已在盘 → filter
    assert c["store_blocks_filtered"] == 1

    store(mgr, 11, [b"new"], [0])           # 满 → 逐出 LRU 头 k0(最老)
    finish(mgr)
    assert c["evicted_blocks"] == 1
    assert c["store_blocks_first"] == NUM_SLOTS + 1

    store(mgr, 12, [b"k0"], [1])            # 被逐出的 k0 回来 → 重写
    finish(mgr)
    assert c["store_blocks_rewrite"] == 1
    # 唯一块口径:_ever_stored 只进不出, k0 重写不重复计
    assert len(mgr._ever_stored) == NUM_SLOTS + 1

    mgr.lookup(b"k0", None)                 # hit
    mgr.lookup(b"ghost", None)              # miss
    assert c["lookup_hit"] >= 1 and c["lookup_miss"] >= 1


def test_capacity_reject_counted(mgr):
    for i in range(NUM_SLOTS):
        store(mgr, i, [f"k{i}".encode()], [i % 8])
    finish(mgr)
    for i in range(NUM_SLOTS):              # 全部 pin 住 → 逐无可逐
        load(mgr, 50 + i, [f"k{i}".encode()], [i % 8])
    store(mgr, 100, [b"new"], [0])
    assert mgr._counters["store_jobs_rejected_capacity"] == 1
    assert mgr._counters["store_blocks_rejected_capacity"] == 1
    # load 侧计数(fixture 的 load 都带 is_promotion=True)
    assert mgr._counters["load_jobs_promotion"] == NUM_SLOTS


def test_stats_dump_on_shutdown(tmp_path):
    buf = memoryview(bytearray(8 * BLOCK)).cast("B", (8, BLOCK))
    m = MgrWithFakeEngine(
        offloading_spec=None,
        primary_kv_view=buf,
        tier_type="uring",
        path=str(tmp_path / "kv.bin"),
        disk_bytes_to_use=NUM_SLOTS * BLOCK,
    )
    store(m, 1, [b"k1"], [0])
    m._engine.complete_all()
    list(m.get_finished_jobs())
    # 关停时还有一个在途 job: shutdown 必须自己收割它, 否则整批漏账
    store(m, 2, [b"k2"], [1])
    m.shutdown()
    # 账本文件在 backing 旁边, backing 本身被 shutdown 删掉(不存在也无妨)
    data = json.loads((tmp_path / "kv.bin.stats.json").read_text())
    assert data["counters"]["store_blocks_first"] == 2   # 含关停时收割的那个
    assert data["gauges"]["present_blocks"] == 2
    assert data["gauges"]["storing_blocks"] == 0         # 没有悬空未收割的块
    assert data["job_records"]["schema"][0] == "job_id"
