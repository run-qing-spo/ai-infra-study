from __future__ import annotations

import argparse
import gc
import hashlib
import json
import math
import mmap
import os
import platform
import shutil
import socket
import sys
import time
import traceback
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import numpy as np

from uring_slab_tier.instrumentation import ContractViolation, InstrumentedTier
from uring_slab_tier.native_fs_noise import (
    _file_summary,
    _harness_identity,
    _make_jobs,
    _make_mock_spec,
    _read_diskstats,
    _read_key_value_file,
    _rusage,
    _safe_remove_run_dir,
    _safe_root,
    _subtract,
    _subtract_numeric,
    _utc_now,
    _validate_environment,
    _write_json_once,
    _write_jsonl_once,
)
from uring_slab_tier.noise_gate import percentile
from uring_slab_tier.paired_stats import (
    balanced_pair_orders,
    evaluate_aa_campaign,
)


def _load_config(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as source:
        config = json.load(source)
    if int(config.get("schema_version", -1)) != 2:
        raise ValueError("paired FS A/A requires config schema_version=2")

    expected_top_level = {
        "schema_version",
        "campaign_name",
        "expected",
        "workload",
        "pairing",
        "safety",
        "system_observation",
        "gate",
    }
    unknown = set(config) - expected_top_level
    missing = expected_top_level - set(config)
    if unknown:
        raise ValueError(f"unknown top-level config fields: {sorted(unknown)}")
    if missing:
        raise ValueError(f"missing top-level config fields: {sorted(missing)}")

    expected_nested = {
        "workload": {
            "block_bytes",
            "dataset_blocks",
            "blocks_per_job",
            "n_read_threads",
            "n_write_threads",
            "warmup_passes",
            "measurement_passes",
            "minimum_measured_phase_seconds",
            "poll_interval_seconds",
            "phase_timeout_seconds",
            "lookup_timeout_seconds",
            "seed",
        },
        "pairing": {
            "windows",
            "pairs_per_window",
            "window_pause_seconds",
            "randomization_seed",
            "max_inter_arm_gap_seconds",
        },
        "safety": {
            "max_live_backing_bytes_per_arm",
            "minimum_free_bytes",
            "retain_backing_files",
            "drop_caches",
        },
        "system_observation": {"block_device"},
        "gate": {
            "required_complete_pairs",
            "primary_metric",
            "higher_is_better",
            "bootstrap_samples",
            "bootstrap_seed",
            "sign_flip_samples",
            "sign_flip_seed",
            "power_samples",
            "power_seed",
            "minimum_detectable_effect",
            "pass_absolute_aa_bias",
            "max_absolute_aa_bias",
            "pass_noise_floor",
            "max_noise_floor",
            "minimum_power",
            "pass_absolute_window_effect",
            "max_absolute_window_effect",
            "pass_absolute_position_effect",
            "max_absolute_position_effect",
        },
    }
    for section, expected_fields in expected_nested.items():
        actual_fields = set(config[section])
        unknown_fields = actual_fields - expected_fields
        missing_fields = expected_fields - actual_fields
        if unknown_fields:
            raise ValueError(
                f"unknown {section} config fields: {sorted(unknown_fields)}"
            )
        if missing_fields:
            raise ValueError(
                f"missing {section} config fields: {sorted(missing_fields)}"
            )

    workload = config["workload"]
    pairing = config["pairing"]
    safety = config["safety"]
    gate = config["gate"]
    if bool(safety.get("drop_caches")):
        raise ValueError("drop_caches must remain false")
    if int(pairing["pairs_per_window"]) % 2:
        raise ValueError("pairs_per_window must be even")
    expected_pairs = int(pairing["windows"]) * int(
        pairing["pairs_per_window"]
    )
    if int(gate["required_complete_pairs"]) != expected_pairs:
        raise ValueError(
            "required_complete_pairs must equal windows * pairs_per_window"
        )
    dataset_bytes = int(workload["block_bytes"]) * int(
        workload["dataset_blocks"]
    )
    if dataset_bytes > int(safety["max_live_backing_bytes_per_arm"]):
        raise ValueError("dataset exceeds per-arm backing safety limit")
    if int(workload["measurement_passes"]) <= 0:
        raise ValueError("measurement_passes must be positive")
    if int(workload["warmup_passes"]) < 0:
        raise ValueError("warmup_passes must be non-negative")
    if float(workload["minimum_measured_phase_seconds"]) <= 0:
        raise ValueError("minimum measured phase duration must be positive")
    if int(workload["block_bytes"]) % mmap.PAGESIZE:
        raise ValueError("block_bytes must be page aligned")
    if str(gate["primary_metric"]) != "load_sustained_throughput_mib_s":
        raise ValueError("v1 only qualifies load sustained throughput")
    for pass_field, fail_field in (
        ("pass_absolute_aa_bias", "max_absolute_aa_bias"),
        ("pass_noise_floor", "max_noise_floor"),
        ("pass_absolute_window_effect", "max_absolute_window_effect"),
        ("pass_absolute_position_effect", "max_absolute_position_effect"),
    ):
        if float(gate[pass_field]) > float(gate[fail_field]):
            raise ValueError(f"{pass_field} must not exceed {fail_field}")
    return config


def _make_schedule(config: dict[str, Any]) -> list[dict[str, int | str]]:
    pairing = config["pairing"]
    windows = int(pairing["windows"])
    pairs_per_window = int(pairing["pairs_per_window"])
    seed = int(pairing["randomization_seed"])
    store = balanced_pair_orders(
        windows=windows,
        pairs_per_window=pairs_per_window,
        seed=seed,
    )
    load = balanced_pair_orders(
        windows=windows,
        pairs_per_window=pairs_per_window,
        seed=seed + 1,
    )
    schedule = []
    for store_item, load_item in zip(store, load):
        if (
            store_item["window"],
            store_item["pair_index"],
        ) != (
            load_item["window"],
            load_item["pair_index"],
        ):
            raise AssertionError("store/load schedule coordinates differ")
        schedule.append(
            {
                "window": int(store_item["window"]),
                "pair_index": int(store_item["pair_index"]),
                "store_order": str(store_item["order"]),
                "load_order": str(load_item["order"]),
            }
        )
    return schedule


def _schedule_sha256(schedule: list[dict[str, int | str]]) -> str:
    canonical = json.dumps(
        schedule, sort_keys=True, separators=(",", ":")
    ).encode()
    return hashlib.sha256(canonical).hexdigest()


def _snapshot_harness(
    campaign_dir: Path,
    harness_identity: dict[str, Any],
) -> None:
    project_root = Path(harness_identity["project_root"])
    snapshot_root = campaign_dir / "harness_snapshot"
    snapshot_root.mkdir(exist_ok=False)
    for relative_path in harness_identity["files"]:
        if relative_path.startswith("external-config:"):
            continue
        source = project_root / relative_path
        target = snapshot_root / relative_path
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)


def _make_keys(
    dataset_identity: str,
    num_blocks: int,
    make_offload_key: Any,
) -> list[bytes]:
    return [
        make_offload_key(
            hashlib.sha256(
                f"{dataset_identity}:{block_index}".encode()
            ).digest(),
            0,
        )
        for block_index in range(num_blocks)
    ]


def _fill_dataset(
    array: np.ndarray[Any, np.dtype[np.uint8]],
    *,
    seed: int,
    window: int,
    pair_index: int,
) -> None:
    for block_index in range(array.shape[0]):
        array[block_index].fill(
            (seed + window * 31 + pair_index * 17 + block_index) % 251
        )


def _run_job_wave(
    tier: InstrumentedTier,
    jobs: list[Any],
    *,
    direction: str,
    poll_interval: float,
    timeout: float,
) -> dict[str, Any]:
    expected_ids = {int(job.job_id) for job in jobs}
    phase_start_ns = time.monotonic_ns()
    if direction == "store":
        for job in jobs:
            tier.submit_store(job)
    elif direction == "load":
        for job in jobs:
            tier.submit_load(job)
    else:
        raise ValueError(f"unknown direction={direction!r}")

    results: dict[int, bool] = {}
    deadline = time.monotonic() + timeout
    polls = 0
    while expected_ids - results.keys():
        if time.monotonic() >= deadline:
            missing = sorted(expected_ids - results.keys())
            raise TimeoutError(f"{direction} timed out; missing jobs={missing}")
        time.sleep(poll_interval)
        polls += 1
        for result in tier.get_finished_jobs():
            job_id = int(result.job_id)
            if job_id in results:
                raise ContractViolation(f"duplicate result for job {job_id}")
            results[job_id] = bool(result.success)
    phase_end_ns = time.monotonic_ns()
    tier.drain_jobs()
    unexpected = list(tier.get_finished_jobs())
    if unexpected:
        raise ContractViolation(
            f"{direction}: unexpected post-wave completions={unexpected}"
        )

    observed_by_id = {
        job.job_id: job
        for job in tier.completed_jobs
        if job.job_id in expected_ids
    }
    if set(observed_by_id) != expected_ids:
        raise ContractViolation(
            f"{direction}: observed ids differ from submitted ids"
        )
    observed = [observed_by_id[job_id] for job_id in sorted(expected_ids)]
    sojourn_ms = [job.observed_sojourn_ns / 1_000_000 for job in observed]
    submit_us = [job.submit_call_ns / 1_000 for job in observed]
    total_bytes = sum(job.n_bytes for job in observed)
    wall_ns = phase_end_ns - phase_start_ns
    return {
        "direction": direction,
        "jobs": len(jobs),
        "blocks": sum(len(job.keys) for job in jobs),
        "bytes": total_bytes,
        "phase_wall_ns": wall_ns,
        "throughput_mib_s": (
            total_bytes / (1024 * 1024) / (wall_ns / 1_000_000_000)
        ),
        "polls": polls,
        "all_success": all(results.values()),
        "job_p50_ms": percentile(sojourn_ms, 0.50),
        "job_p95_ms": percentile(sojourn_ms, 0.95),
        "job_p99_ms": percentile(sojourn_ms, 0.99),
        "job_max_ms": max(sojourn_ms),
        "submit_p50_us": percentile(submit_us, 0.50),
        "submit_p95_us": percentile(submit_us, 0.95),
        "completion_ids": sorted(results),
        "_sojourn_ms": sojourn_ms,
        "_submit_us": submit_us,
        "_phase_start_ns": phase_start_ns,
        "_phase_end_ns": phase_end_ns,
    }


def _lookup_all(
    tier: InstrumentedTier,
    keys: list[bytes],
    req_context: Any,
    *,
    poll_interval: float,
    timeout: float,
) -> dict[str, Any]:
    start_ns = time.monotonic_ns()
    initial = [tier.lookup(key, req_context) for key in keys]
    tier.on_schedule_end()
    resolved = initial
    polls = 0
    deadline = time.monotonic() + timeout
    while any(result is None for result in resolved):
        if time.monotonic() >= deadline:
            raise TimeoutError("lookup did not resolve before deadline")
        time.sleep(poll_interval)
        polls += 1
        resolved = [tier.lookup(key, req_context) for key in keys]
        if any(result is None for result in resolved):
            tier.on_schedule_end()
    end_ns = time.monotonic_ns()
    tier.on_request_finished(req_context)
    return {
        "keys": len(keys),
        "initial_none": sum(result is None for result in initial),
        "resolved_true": sum(result is True for result in resolved),
        "resolved_false": sum(result is False for result in resolved),
        "polls": polls,
        "lookup_resolution_ns": end_ns - start_ns,
        "all_present": all(result is True for result in resolved),
    }


def _prepare_arm(
    *,
    config: dict[str, Any],
    campaign_id: str,
    pair_id: str,
    arm: str,
    window: int,
    pair_index: int,
    arm_output_dir: Path,
    arm_data_dir: Path,
    dataset_identity: str,
) -> dict[str, Any]:
    from vllm.v1.kv_offload.base import ReqContext, make_offload_key
    from vllm.v1.kv_offload.tiering.base import JobMetadata
    from vllm.v1.kv_offload.tiering.fs.manager import FileSystemTierManager

    workload = config["workload"]
    block_bytes = int(workload["block_bytes"])
    num_blocks = int(workload["dataset_blocks"])
    total_bytes = block_bytes * num_blocks
    arm_output_dir.mkdir(parents=True, exist_ok=False)
    arm_data_dir.mkdir(parents=True, exist_ok=False)
    _write_json_once(
        arm_output_dir / "prepare_manifest.json",
        {
            "schema_version": 2,
            "campaign_id": campaign_id,
            "pair_id": pair_id,
            "arm": arm,
            "dataset_identity": dataset_identity,
            "started_at": _utc_now(),
        },
    )

    memory = mmap.mmap(-1, total_bytes)
    array = np.ndarray((num_blocks, block_bytes), dtype=np.uint8, buffer=memory)
    _fill_dataset(
        array,
        seed=int(workload["seed"]),
        window=window,
        pair_index=pair_index,
    )
    primary_view = memoryview(array)
    expected_checksum = hashlib.sha256(primary_view.cast("B")).hexdigest()
    inner_tier = FileSystemTierManager(
        offloading_spec=_make_mock_spec(block_bytes),
        primary_kv_view=primary_view,
        tier_type="fs",
        root_dir=str(arm_data_dir),
        n_read_threads=int(workload["n_read_threads"]),
        n_write_threads=int(workload["n_write_threads"]),
    )
    tier = InstrumentedTier(
        inner_tier,
        run_id=f"{pair_id}-{arm}-prepare",
        block_bytes=block_bytes,
    )
    req_context = ReqContext(req_id=f"{pair_id}-{arm}-prepare")
    keys = _make_keys(dataset_identity, num_blocks, make_offload_key)
    jobs = _make_jobs(
        job_id_start=1,
        keys=keys,
        blocks_per_job=int(workload["blocks_per_job"]),
        is_promotion=False,
        req_context=req_context,
        job_metadata_type=JobMetadata,
    )
    failure: BaseException | None = None
    result: dict[str, Any] | None = None
    try:
        store = _run_job_wave(
            tier,
            jobs,
            direction="store",
            poll_interval=float(workload["poll_interval_seconds"]),
            timeout=float(workload["phase_timeout_seconds"]),
        )
        files = _file_summary(arm_data_dir)
        checks = {
            "store_all_success": bool(store["all_success"]),
            "file_count_match": files["bin_files"] == num_blocks,
            "file_bytes_match": files["bin_bytes"] == total_bytes,
            "no_tmp_files": files["tmp_files"] == 0,
            "no_pending_jobs": tier.pending_job_count == 0,
        }
        result = {
            "schema_version": 2,
            "campaign_id": campaign_id,
            "pair_id": pair_id,
            "arm": arm,
            "status": "PASS" if all(checks.values()) else "FAIL",
            "finished_at": _utc_now(),
            "dataset_identity": dataset_identity,
            "dataset_bytes": total_bytes,
            "expected_checksum": expected_checksum,
            "store": {
                key: value
                for key, value in store.items()
                if not key.startswith("_")
            },
            "files": files,
            "correctness": {"pass": all(checks.values()), "checks": checks},
        }
        return result
    except BaseException as exc:
        failure = exc
        raise
    finally:
        shutdown_failure: BaseException | None = None
        try:
            if tier.pending_job_count:
                inner_tier.drain_jobs()
                tier.get_finished_jobs()
            tier.shutdown()
        except BaseException as exc:
            shutdown_failure = exc
        if result is not None:
            result["instrumentation"] = tier.event_stats
            result["correctness"]["checks"]["events_not_dropped"] = (
                tier.event_stats["dropped_events"] == 0
            )
            result["correctness"]["checks"]["shutdown_clean"] = (
                shutdown_failure is None
            )
            result["correctness"]["pass"] = all(
                result["correctness"]["checks"].values()
            )
            result["status"] = (
                "PASS" if result["correctness"]["pass"] else "FAIL"
            )
            _write_json_once(arm_output_dir / "prepare_result.json", result)
        if failure is not None:
            _write_json_once(
                arm_output_dir / "prepare_failure.json",
                {
                    "schema_version": 2,
                    "type": type(failure).__name__,
                    "message": str(failure),
                    "traceback": "".join(
                        traceback.format_exception(
                            type(failure), failure, failure.__traceback__
                        )
                    ),
                },
            )
        _write_jsonl_once(
            arm_output_dir / "prepare_events.jsonl", tier.events
        )
        primary_view.release()
        del tier
        del inner_tier
        del array
        gc.collect()
        memory.close()


def _measure_arm(
    *,
    config: dict[str, Any],
    campaign_id: str,
    pair_id: str,
    arm: str,
    window: int,
    pair_index: int,
    arm_output_dir: Path,
    arm_data_dir: Path,
    dataset_identity: str,
    expected_checksum: str,
) -> dict[str, Any]:
    from vllm.v1.kv_offload.base import ReqContext, make_offload_key
    from vllm.v1.kv_offload.tiering.base import JobMetadata
    from vllm.v1.kv_offload.tiering.fs.manager import FileSystemTierManager

    workload = config["workload"]
    block_bytes = int(workload["block_bytes"])
    num_blocks = int(workload["dataset_blocks"])
    total_bytes = block_bytes * num_blocks
    memory = mmap.mmap(-1, total_bytes)
    array = np.ndarray((num_blocks, block_bytes), dtype=np.uint8, buffer=memory)
    array.fill(0)
    primary_view = memoryview(array)
    inner_tier = FileSystemTierManager(
        offloading_spec=_make_mock_spec(block_bytes),
        primary_kv_view=primary_view,
        tier_type="fs",
        root_dir=str(arm_data_dir),
        n_read_threads=int(workload["n_read_threads"]),
        n_write_threads=int(workload["n_write_threads"]),
    )
    tier = InstrumentedTier(
        inner_tier,
        run_id=f"{pair_id}-{arm}-load",
        block_bytes=block_bytes,
    )
    req_context = ReqContext(req_id=f"{pair_id}-{arm}-load")
    tier.on_new_request(req_context)
    keys = _make_keys(dataset_identity, num_blocks, make_offload_key)
    lookup = _lookup_all(
        tier,
        keys,
        req_context,
        poll_interval=float(workload["poll_interval_seconds"]),
        timeout=float(workload["lookup_timeout_seconds"]),
    )

    proc_io_before = _read_key_value_file(Path("/proc/self/io"))
    disk_before = _read_diskstats(
        config["system_observation"]["block_device"]
    )
    usage_before = _rusage()
    result: dict[str, Any] | None = None
    failure: BaseException | None = None
    all_sojourn_ms: list[float] = []
    all_submit_us: list[float] = []
    measured_waves: list[dict[str, Any]] = []
    try:
        next_job_id = 1
        warmup_passes = int(workload["warmup_passes"])
        for pass_index in range(warmup_passes):
            jobs = _make_jobs(
                job_id_start=next_job_id,
                keys=keys,
                blocks_per_job=int(workload["blocks_per_job"]),
                is_promotion=True,
                req_context=req_context,
                job_metadata_type=JobMetadata,
            )
            next_job_id += len(jobs)
            _run_job_wave(
                tier,
                jobs,
                direction="load",
                poll_interval=float(workload["poll_interval_seconds"]),
                timeout=float(workload["phase_timeout_seconds"]),
            )

        warmup_checksum = hashlib.sha256(primary_view.cast("B")).hexdigest()
        if warmup_checksum != expected_checksum:
            raise ContractViolation("warmup checksum mismatch")

        measurement_passes = int(workload["measurement_passes"])
        measured_phase_start_ns: int | None = None
        measured_phase_end_ns: int | None = None
        for pass_index in range(measurement_passes):
            jobs = _make_jobs(
                job_id_start=next_job_id,
                keys=keys,
                blocks_per_job=int(workload["blocks_per_job"]),
                is_promotion=True,
                req_context=req_context,
                job_metadata_type=JobMetadata,
            )
            next_job_id += len(jobs)
            wave = _run_job_wave(
                tier,
                jobs,
                direction="load",
                poll_interval=float(workload["poll_interval_seconds"]),
                timeout=float(workload["phase_timeout_seconds"]),
            )
            all_sojourn_ms.extend(wave.pop("_sojourn_ms"))
            all_submit_us.extend(wave.pop("_submit_us"))
            wave_start_ns = int(wave.pop("_phase_start_ns"))
            wave_end_ns = int(wave.pop("_phase_end_ns"))
            if measured_phase_start_ns is None:
                measured_phase_start_ns = wave_start_ns
            measured_phase_end_ns = wave_end_ns
            wave["pass_index"] = pass_index
            measured_waves.append(wave)
        if measured_phase_start_ns is None or measured_phase_end_ns is None:
            raise AssertionError("measurement phase produced no waves")

        observed_checksum = hashlib.sha256(primary_view.cast("B")).hexdigest()
        measured_phase_wall_ns = (
            measured_phase_end_ns - measured_phase_start_ns
        )
        measured_bytes = total_bytes * measurement_passes
        measured_seconds = measured_phase_wall_ns / 1_000_000_000
        throughput = measured_bytes / (1024 * 1024) / measured_seconds
        files = _file_summary(arm_data_dir)
        expected_measured_jobs = measurement_passes * math.ceil(
            num_blocks / int(workload["blocks_per_job"])
        )
        checks = {
            "lookup_all_present": bool(lookup["all_present"]),
            "warmup_checksum_match": warmup_checksum == expected_checksum,
            "measured_checksum_match": observed_checksum
            == expected_checksum,
            "all_measured_waves_success": all(
                wave["all_success"] for wave in measured_waves
            ),
            "measurement_passes_match": len(measured_waves)
            == measurement_passes,
            "measured_jobs_match": sum(
                int(wave["jobs"]) for wave in measured_waves
            )
            == expected_measured_jobs,
            "measured_bytes_match": sum(
                int(wave["bytes"]) for wave in measured_waves
            )
            == measured_bytes,
            "minimum_phase_duration": measured_seconds
            >= float(workload["minimum_measured_phase_seconds"]),
            "file_count_match": files["bin_files"] == num_blocks,
            "file_bytes_match": files["bin_bytes"] == total_bytes,
            "no_tmp_files": files["tmp_files"] == 0,
            "no_pending_jobs": tier.pending_job_count == 0,
        }
        result = {
            "schema_version": 2,
            "campaign_id": campaign_id,
            "pair_id": pair_id,
            "arm": arm,
            "window": window,
            "pair_index": pair_index,
            "status": "PASS" if all(checks.values()) else "FAIL",
            "finished_at": _utc_now(),
            "dataset_identity": dataset_identity,
            "lookup": lookup,
            "warmup_passes": warmup_passes,
            "measurement_passes": measurement_passes,
            "measured_bytes": measured_bytes,
            "measured_phase_started_mono_ns": measured_phase_start_ns,
            "measured_phase_finished_mono_ns": measured_phase_end_ns,
            "measured_phase_wall_ns": measured_phase_wall_ns,
            "measured_waves": measured_waves,
            "checksum": {
                "algorithm": "sha256",
                "expected": expected_checksum,
                "warmup": warmup_checksum,
                "measured": observed_checksum,
            },
            "files": files,
            "metrics": {
                "load_sustained_throughput_mib_s": throughput,
                "load_job_p50_ms": percentile(all_sojourn_ms, 0.50),
                "load_job_p95_ms": percentile(all_sojourn_ms, 0.95),
                "load_job_p99_ms": percentile(all_sojourn_ms, 0.99),
                "load_job_max_ms": max(all_sojourn_ms),
                "load_submit_p50_us": percentile(all_submit_us, 0.50),
                "load_submit_p95_us": percentile(all_submit_us, 0.95),
                "load_wave_throughput_p50_mib_s": percentile(
                    (
                        float(wave["throughput_mib_s"])
                        for wave in measured_waves
                    ),
                    0.50,
                ),
                "load_wave_throughput_p95_mib_s": percentile(
                    (
                        float(wave["throughput_mib_s"])
                        for wave in measured_waves
                    ),
                    0.95,
                ),
            },
            "correctness": {"pass": all(checks.values()), "checks": checks},
        }
        return result
    except BaseException as exc:
        failure = exc
        raise
    finally:
        proc_io_after = _read_key_value_file(Path("/proc/self/io"))
        disk_after = _read_diskstats(
            config["system_observation"]["block_device"]
        )
        usage_after = _rusage()
        system_delta = {
            "proc_io": _subtract(proc_io_after, proc_io_before),
            "diskstats": _subtract(disk_after, disk_before),
            "rusage": _subtract_numeric(usage_after, usage_before),
            "loadavg": os.getloadavg(),
        }
        shutdown_failure: BaseException | None = None
        try:
            if tier.pending_job_count:
                inner_tier.drain_jobs()
                tier.get_finished_jobs()
            tier.shutdown()
        except BaseException as exc:
            shutdown_failure = exc
        if result is not None:
            result["instrumentation"] = tier.event_stats
            result["system_delta"] = system_delta
            result["correctness"]["checks"]["events_not_dropped"] = (
                tier.event_stats["dropped_events"] == 0
            )
            result["correctness"]["checks"]["shutdown_clean"] = (
                shutdown_failure is None
            )
            result["correctness"]["pass"] = all(
                result["correctness"]["checks"].values()
            )
            result["status"] = (
                "PASS" if result["correctness"]["pass"] else "FAIL"
            )
            _write_json_once(arm_output_dir / "load_result.json", result)
        if failure is not None:
            _write_json_once(
                arm_output_dir / "load_failure.json",
                {
                    "schema_version": 2,
                    "type": type(failure).__name__,
                    "message": str(failure),
                    "traceback": "".join(
                        traceback.format_exception(
                            type(failure), failure, failure.__traceback__
                        )
                    ),
                    "system_delta": system_delta,
                },
            )
        _write_jsonl_once(arm_output_dir / "load_events.jsonl", tier.events)
        primary_view.release()
        del tier
        del inner_tier
        del array
        gc.collect()
        memory.close()


def _run_pair(
    *,
    config: dict[str, Any],
    campaign_id: str,
    item: dict[str, int | str],
    campaign_dir: Path,
    campaign_data_dir: Path,
) -> dict[str, Any]:
    window = int(item["window"])
    pair_index = int(item["pair_index"])
    pair_id = f"w{window:02d}-p{pair_index:02d}"
    pair_output_dir = campaign_dir / pair_id
    pair_data_dir = campaign_data_dir / pair_id
    pair_output_dir.mkdir(exist_ok=False)
    pair_data_dir.mkdir(parents=True, exist_ok=False)
    dataset_identity = (
        f"seed={int(config['workload']['seed'])}:"
        f"window={window}:pair={pair_index}"
    )
    pair_manifest = {
        "schema_version": 2,
        "campaign_id": campaign_id,
        "pair_id": pair_id,
        "window": window,
        "pair_index": pair_index,
        "store_order": item["store_order"],
        "load_order": item["load_order"],
        "dataset_identity": dataset_identity,
        "started_at": _utc_now(),
    }
    _write_json_once(pair_output_dir / "pair_manifest.json", pair_manifest)

    preparations: dict[str, dict[str, Any]] = {}
    runs: dict[str, dict[str, Any]] = {}
    failures: list[dict[str, str]] = []
    cleanup_failure: str | None = None
    try:
        for arm in str(item["store_order"]):
            arm_output_dir = pair_output_dir / arm
            arm_data_dir = pair_data_dir / arm
            try:
                preparations[arm] = _prepare_arm(
                    config=config,
                    campaign_id=campaign_id,
                    pair_id=pair_id,
                    arm=arm,
                    window=window,
                    pair_index=pair_index,
                    arm_output_dir=arm_output_dir,
                    arm_data_dir=arm_data_dir,
                    dataset_identity=dataset_identity,
                )
            except BaseException as exc:
                failures.append(
                    {
                        "arm": arm,
                        "phase": "prepare",
                        "type": type(exc).__name__,
                        "message": str(exc),
                    }
                )

        for arm in str(item["load_order"]):
            if arm not in preparations or preparations[arm]["status"] != "PASS":
                continue
            try:
                runs[arm] = _measure_arm(
                    config=config,
                    campaign_id=campaign_id,
                    pair_id=pair_id,
                    arm=arm,
                    window=window,
                    pair_index=pair_index,
                    arm_output_dir=pair_output_dir / arm,
                    arm_data_dir=pair_data_dir / arm,
                    dataset_identity=dataset_identity,
                    expected_checksum=preparations[arm][
                        "expected_checksum"
                    ],
                )
            except BaseException as exc:
                failures.append(
                    {
                        "arm": arm,
                        "phase": "load",
                        "type": type(exc).__name__,
                        "message": str(exc),
                    }
                )
    finally:
        if not bool(config["safety"]["retain_backing_files"]):
            try:
                _safe_remove_run_dir(pair_data_dir, campaign_data_dir)
            except BaseException as exc:
                cleanup_failure = f"{type(exc).__name__}: {exc}"

    load_order = str(item["load_order"])
    inter_arm_gap_seconds = None
    if set(runs) == {"A", "B"}:
        first_arm, second_arm = load_order
        inter_arm_gap_seconds = (
            int(runs[second_arm]["measured_phase_started_mono_ns"])
            - int(runs[first_arm]["measured_phase_finished_mono_ns"])
        ) / 1_000_000_000
    max_gap = float(config["pairing"]["max_inter_arm_gap_seconds"])
    checks = {
        "both_preparations_pass": set(preparations) == {"A", "B"}
        and all(item["status"] == "PASS" for item in preparations.values()),
        "both_load_runs_pass": set(runs) == {"A", "B"}
        and all(item["status"] == "PASS" for item in runs.values()),
        "no_phase_failures": not failures,
        "inter_arm_gap_recorded": inter_arm_gap_seconds is not None,
        "inter_arm_gap_within_limit": inter_arm_gap_seconds is not None
        and inter_arm_gap_seconds <= max_gap,
        "backing_cleanup_pass": cleanup_failure is None,
    }
    status = "PASS" if all(checks.values()) else "FAIL"
    pair_result = {
        **pair_manifest,
        "status": status,
        "finished_at": _utc_now(),
        "inter_arm_gap_seconds": inter_arm_gap_seconds,
        "preparations": preparations,
        "run_a": runs.get("A", {"status": "MISSING"}),
        "run_b": runs.get("B", {"status": "MISSING"}),
        "failures": failures,
        "cleanup_failure": cleanup_failure,
        "correctness": {"pass": all(checks.values()), "checks": checks},
    }
    _write_json_once(pair_output_dir / "pair_result.json", pair_result)
    if status == "PASS":
        (pair_output_dir / "DONE").touch(exist_ok=False)
    return pair_result


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    parser.add_argument("--data-root", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    config_path = args.config.resolve()
    config = _load_config(config_path)
    output_root = _safe_root(args.output_root, "output root")
    data_root = _safe_root(args.data_root, "data root")
    environment = _validate_environment(config)
    if environment["status"] != "PASS":
        print(json.dumps(environment, indent=2, sort_keys=True), file=sys.stderr)
        return 2

    harness_identity = _harness_identity(config_path)
    schedule = _make_schedule(config)
    campaign_id = (
        datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        + "-"
        + uuid.uuid4().hex[:8]
    )
    campaign_dir = output_root / campaign_id
    campaign_dir.mkdir(exist_ok=False)
    campaign_data_dir = data_root / campaign_id
    campaign_data_dir.mkdir(parents=True, exist_ok=False)
    minimum_free_bytes = int(config["safety"]["minimum_free_bytes"])
    observed_free_bytes = shutil.disk_usage(campaign_data_dir).free
    if observed_free_bytes < minimum_free_bytes:
        campaign_data_dir.rmdir()
        campaign_dir.rmdir()
        raise RuntimeError(
            f"insufficient free bytes: need {minimum_free_bytes}, "
            f"observed {observed_free_bytes}"
        )
    campaign = {
        "schema_version": 2,
        "campaign_id": campaign_id,
        "campaign_name": config["campaign_name"],
        "started_at": _utc_now(),
        "hostname": socket.gethostname(),
        "platform": platform.platform(),
        "process": {
            "pid": os.getpid(),
            "cpu_affinity": sorted(os.sched_getaffinity(0)),
            "cgroup": Path("/proc/self/cgroup").read_text(encoding="utf-8"),
        },
        "environment": environment,
        "storage_preflight": {
            "minimum_free_bytes": minimum_free_bytes,
            "observed_free_bytes": observed_free_bytes,
        },
        "harness_identity": harness_identity,
        "schedule": schedule,
        "schedule_sha256": _schedule_sha256(schedule),
        "config": config,
    }
    _write_json_once(campaign_dir / "campaign.json", campaign)
    _snapshot_harness(campaign_dir, harness_identity)

    results: list[dict[str, Any]] = []
    windows = int(config["pairing"]["windows"])
    for window in range(windows):
        window_items = [
            item for item in schedule if int(item["window"]) == window
        ]
        for item in window_items:
            result = _run_pair(
                config=config,
                campaign_id=campaign_id,
                item=item,
                campaign_dir=campaign_dir,
                campaign_data_dir=campaign_data_dir,
            )
            results.append(result)
            metric = result.get("run_a", {}).get("metrics", {}).get(
                "load_sustained_throughput_mib_s"
            )
            metric_b = result.get("run_b", {}).get("metrics", {}).get(
                "load_sustained_throughput_mib_s"
            )
            print(
                json.dumps(
                    {
                        "pair_id": result["pair_id"],
                        "status": result["status"],
                        "load_order": result["load_order"],
                        "a_load_mib_s": metric,
                        "b_load_mib_s": metric_b,
                    },
                    sort_keys=True,
                ),
                flush=True,
            )
        if window + 1 < windows:
            time.sleep(float(config["pairing"]["window_pause_seconds"]))

    gate = evaluate_aa_campaign(results, config["gate"])
    summary = {
        "schema_version": 2,
        "campaign_id": campaign_id,
        "finished_at": _utc_now(),
        "attempted_pairs": len(schedule),
        "gate": gate,
    }
    _write_json_once(campaign_dir / "summary.json", summary)
    if gate["status"] in {"PASS", "CONDITIONAL"}:
        (campaign_dir / "DONE").touch(exist_ok=False)
    try:
        campaign_data_dir.rmdir()
    except OSError:
        pass
    print(json.dumps(summary, indent=2, sort_keys=True), flush=True)
    return 0 if gate["status"] in {"PASS", "CONDITIONAL"} else 1


if __name__ == "__main__":
    raise SystemExit(main())
