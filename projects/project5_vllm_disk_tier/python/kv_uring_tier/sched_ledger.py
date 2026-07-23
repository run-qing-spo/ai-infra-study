# sched_ledger — 调度层的 tier job 计时埋点。回答的问题是:
#   "框架提交一次 secondary job 后,多久首次观察到它完成"。
#
# 为什么需要它:uring tier 有 C++ 引擎的 per-job 账本(设备下发→完成,
# manager._RECORD_SCHEMA), fs tier 走 vLLM 原生路径, JobResult 只带
# (job_id, success), 一个时间戳都没有。所以在此之前根本算不出 fs 的 IO
# 时间, e3_window_audit 第 27 行那句"fs 自己的 IO 时间仍需 fs 侧埋点才能证"
# 就是这个窟窿。
#
# 埋在哪、为什么埋这:所有 secondary tier(fs / uring)都由
# SecondaryTierFactory.create_secondary_tier 造出来, 都被同一个
# TieringOffloadingManager 在同一个调度线程里调 submit_load / get_finished_jobs。
# 在工厂这一层给每个新 tier 实例的这几个方法包一层计时, fs 和 uring 就是
# 同一段代码、同一高度量出来的 —— 不会重演"拿 fs 的 pread 时间比 uring 的
# 设备下发时间"那种苹果比橘子(那正是 no-overclaim 要防的)。
#
#   t_submit = submit_load/submit_store 被调用的时刻
#   t_done   = 框架 get_finished_jobs() 首次看到这个 job 完成的时刻
#   job sojourn = t_done - t_submit。它含 tier 排队、IO 和 scheduler 轮询粒度,
#   但在补到 request_resume 事件前,不能把它直接命名成"请求阻塞时间"。
#
# 这比 uring 的 C++ 账本(设备下发→完成)宽一点:它含了排队 + 轮询粒度。
# 等 request_resume 事件补齐后,才能判断其中多少真正落在请求关键路径上。
# uring 的窄账本留作"设备读本身多快"的交叉验证,两者不混。
#
# wall time 保留给旧的 iostat 窗口对齐;正式持续时间使用 monotonic_ns,
# 不受 NTP/系统校时影响。
#
# 开关:只在设了环境变量 KVTIER_SCHED_LEDGER(指向要写的 .jsonl)时启用,
# 没设就完全不动手。任何异常一律吞掉打日志 —— 埋点绝不能把 serve 带崩。
import json
import logging
import os
import threading
import time

from .request_ids import extract_req_id

logger = logging.getLogger(__name__)


def _extract_job_metadata(args, kwargs):
    if "job_metadata" in kwargs:
        return kwargs["job_metadata"]
    return args[0] if args else None


def _instrument_tier(tier, write_row):
    """给单个 tier 实例的 submit/get_finished 覆盖上计时版(实例级, 不动类)。"""
    tier_type = getattr(tier, "tier_type", "?")
    # job_id -> job metadata。只在调度线程读写, 无需加锁,
    # 但 write_row 落文件是共享的, 那把锁在外面。
    pending: dict = {}
    run_id = os.environ.get("KVTIER_RUN_ID", "")

    orig_submit_load = tier.submit_load
    orig_submit_store = tier.submit_store
    orig_get_finished = tier.get_finished_jobs

    def _stash(is_write, args, kwargs):
        jm = _extract_job_metadata(args, kwargs)
        if jm is None:
            return
        try:
            req_context = getattr(jm, "req_context", None)
            engine_req_id = getattr(req_context, "req_id", None)
            pending[jm.job_id] = {
                "is_write": is_write,
                "is_promotion": bool(getattr(jm, "is_promotion", False)),
                "n_blocks": int(len(jm.block_ids)),
                "req_id": extract_req_id(engine_req_id),
                "vllm_internal_req_id": engine_req_id,
                "t_submit": time.time(),
                "t_submit_mono_ns": time.monotonic_ns(),
            }
        except Exception:
            pass

    def w_submit_load(*args, **kwargs):
        _stash(False, args, kwargs)
        return orig_submit_load(*args, **kwargs)

    def w_submit_store(*args, **kwargs):
        _stash(True, args, kwargs)
        return orig_submit_store(*args, **kwargs)

    def w_get_finished(*args, **kwargs):
        results = list(orig_get_finished(*args, **kwargs))
        t_done = time.time()
        t_done_mono_ns = time.monotonic_ns()
        for r in results:
            info = pending.pop(getattr(r, "job_id", None), None)
            if info is None:
                continue  # store 的完成也会出现;没 stash 到的(极早期)跳过
            write_row({
                "schema_version": 1,
                "record_type": "tier_job",
                "run_id": run_id,
                "tier": tier_type,
                "job_id": r.job_id,
                "req_id": info["req_id"],
                "vllm_internal_req_id": info["vllm_internal_req_id"],
                "is_write": info["is_write"],
                "is_promotion": info["is_promotion"],
                "n_blocks": info["n_blocks"],
                "success": bool(getattr(r, "success", False)),
                "t_submit": info["t_submit"],
                "t_done": t_done,
                "t_submit_mono_ns": info["t_submit_mono_ns"],
                "t_done_mono_ns": t_done_mono_ns,
            })
        return results

    tier.submit_load = w_submit_load
    tier.submit_store = w_submit_store
    tier.get_finished_jobs = w_get_finished


def maybe_install() -> None:
    """由 kv_uring_tier.register() 在每个进程启动时调。设了环境变量才真装。"""
    ledger_path = os.environ.get("KVTIER_SCHED_LEDGER")
    if not ledger_path:
        return
    try:
        from vllm.v1.kv_offload.tiering.factory import SecondaryTierFactory
    except ImportError:
        return  # 老 vLLM 没 tiering

    if getattr(SecondaryTierFactory, "_kvtier_sched_wrapped", False):
        return  # 幂等:plugin 可能被加载多次

    fp = open(ledger_path, "a", buffering=1)  # 行缓冲, 追加
    lock = threading.Lock()

    def write_row(row):
        line = json.dumps(row)
        with lock:
            fp.write(line + "\n")

    orig_create = SecondaryTierFactory.create_secondary_tier.__func__

    @classmethod
    def patched_create(cls, *args, **kwargs):
        tier = orig_create(cls, *args, **kwargs)
        try:
            _instrument_tier(tier, write_row)
        except Exception as e:
            logger.warning("sched_ledger: 该 tier 未计时(不影响 serve): %r", e)
        return tier

    SecondaryTierFactory.create_secondary_tier = patched_create
    SecondaryTierFactory._kvtier_sched_wrapped = True
    logger.info("sched_ledger: 调度层 tier 计时已装载 -> %s", ledger_path)
