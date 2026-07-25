from __future__ import annotations

import argparse
import gc
import hashlib
import importlib.metadata
import json
import mmap
import os
import platform
import resource
import shutil
import socket
import subprocess
import sys
import time
import traceback
import uuid
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import numpy as np

from uring_slab_tier.instrumentation import ContractViolation, InstrumentedTier
from uring_slab_tier.noise_gate import evaluate_campaign, percentile


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _write_json_once(path: Path, value: Any) -> None:
    with path.open("x", encoding="utf-8") as output:
        json.dump(value, output, indent=2, sort_keys=True)
        output.write("\n")


def _write_jsonl_once(path: Path, records: list[dict[str, Any]]) -> None:
    with path.open("x", encoding="utf-8") as output:
        for record in records:
            output.write(json.dumps(record, sort_keys=True))
            output.write("\n")


def _load_config(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as source:
        config = json.load(source)
    if int(config.get("schema_version", -1)) != 1:
        raise ValueError("only config schema_version=1 is supported")
    return config


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _harness_identity(config_path: Path) -> dict[str, Any]:
    project_root = Path(__file__).resolve().parents[2]
    included_suffixes = {".json", ".md", ".py", ".toml"}
    files = {
        str(path.relative_to(project_root)): _sha256_file(path)
        for path in sorted(project_root.rglob("*"))
        if path.is_file()
        and path.suffix in included_suffixes
        and "__pycache__" not in path.parts
        and not (
            "evidence" in path.parts
            and "raw" in path.parts
        )
    }
    resolved_config = config_path.resolve()
    if project_root not in resolved_config.parents:
        files[f"external-config:{resolved_config}"] = _sha256_file(resolved_config)
    canonical = json.dumps(files, sort_keys=True, separators=(",", ":")).encode()
    return {
        "project_root": str(project_root),
        "tree_sha256": hashlib.sha256(canonical).hexdigest(),
        "files": files,
    }


def _safe_root(path: Path, label: str) -> Path:
    resolved = path.expanduser().resolve()
    forbidden = {Path("/"), Path.home().resolve(), Path("/root"), Path("/tmp")}
    if resolved in forbidden or len(resolved.parts) < 4:
        raise ValueError(f"unsafe {label}: {resolved}")
    resolved.mkdir(parents=True, exist_ok=True)
    return resolved


def _safe_remove_run_dir(run_dir: Path, data_root: Path) -> None:
    resolved_run = run_dir.resolve()
    resolved_root = data_root.resolve()
    if resolved_run == resolved_root or resolved_root not in resolved_run.parents:
        raise RuntimeError(f"refusing to remove unsafe run directory {resolved_run}")
    shutil.rmtree(resolved_run)


def _git_head(source_dir: str) -> str:
    result = subprocess.run(
        ["git", "-C", source_dir, "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def _read_key_value_file(path: Path) -> dict[str, int]:
    values: dict[str, int] = {}
    try:
        with path.open(encoding="utf-8") as source:
            for line in source:
                key, raw_value = line.split(":", 1)
                token = raw_value.strip().split()[0]
                if token.isdigit():
                    values[key] = int(token)
    except FileNotFoundError:
        return {}
    return values


def _read_diskstats(device: str) -> dict[str, int]:
    with Path("/proc/diskstats").open(encoding="utf-8") as source:
        for line in source:
            fields = line.split()
            if len(fields) >= 14 and fields[2] == device:
                return {
                    "reads_completed": int(fields[3]),
                    "sectors_read": int(fields[5]),
                    "read_time_ms": int(fields[6]),
                    "writes_completed": int(fields[7]),
                    "sectors_written": int(fields[9]),
                    "write_time_ms": int(fields[10]),
                    "io_in_progress": int(fields[11]),
                    "io_time_ms": int(fields[12]),
                    "weighted_io_time_ms": int(fields[13]),
                }
    return {}


def _subtract(after: dict[str, int], before: dict[str, int]) -> dict[str, int]:
    return {
        key: after[key] - before.get(key, 0)
        for key in after.keys() & before.keys()
    }


def _rusage() -> dict[str, float | int]:
    usage = resource.getrusage(resource.RUSAGE_SELF)
    return {
        "user_seconds": usage.ru_utime,
        "system_seconds": usage.ru_stime,
        "minor_faults": usage.ru_minflt,
        "major_faults": usage.ru_majflt,
        "voluntary_context_switches": usage.ru_nvcsw,
        "involuntary_context_switches": usage.ru_nivcsw,
    }


def _subtract_numeric(
    after: dict[str, float | int], before: dict[str, float | int]
) -> dict[str, float]:
    return {
        key: float(after[key]) - float(before[key])
        for key in after.keys() & before.keys()
    }


def _make_mock_spec(block_size: int) -> Any:
    parallel = SimpleNamespace(
        tensor_parallel_size=1,
        pipeline_parallel_size=1,
        prefill_context_parallel_size=1,
        decode_context_parallel_size=1,
        rank=0,
    )
    vllm_config = SimpleNamespace(
        model_config=SimpleNamespace(model="uring-slab-contract-noise"),
        cache_config=SimpleNamespace(block_size=16, cache_dtype="torch.float16"),
        parallel_config=parallel,
        use_v2_model_runner=True,
    )
    return SimpleNamespace(
        vllm_config=vllm_config,
        kv_cache_config=SimpleNamespace(kv_cache_groups=[]),
        block_size_factor=1,
        kv_bytes_per_offloaded_block=block_size,
    )


def _make_keys(run_id: str, num_blocks: int, make_offload_key: Any) -> list[bytes]:
    return [
        make_offload_key(
            hashlib.sha256(f"{run_id}:{block_index}".encode()).digest(), 0
        )
        for block_index in range(num_blocks)
    ]


def _make_jobs(
    *,
    job_id_start: int,
    keys: list[bytes],
    blocks_per_job: int,
    is_promotion: bool,
    req_context: Any,
    job_metadata_type: Any,
) -> list[Any]:
    jobs = []
    for offset in range(0, len(keys), blocks_per_job):
        job_keys = keys[offset : offset + blocks_per_job]
        block_ids = np.arange(offset, offset + len(job_keys), dtype=np.int64)
        jobs.append(
            job_metadata_type(
                job_id=job_id_start + len(jobs),
                keys=job_keys,
                block_ids=block_ids,
                is_promotion=is_promotion,
                req_context=req_context,
            )
        )
    return jobs


def _run_transfer_phase(
    tier: InstrumentedTier,
    jobs: list[Any],
    *,
    direction: str,
    poll_interval: float,
    timeout: float,
) -> dict[str, Any]:
    phase_start_ns = time.monotonic_ns()
    expected_ids = {int(job.job_id) for job in jobs}
    if direction == "store":
        for job in jobs:
            tier.submit_store(job)
    else:
        for job in jobs:
            tier.submit_load(job)

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
            results[int(result.job_id)] = bool(result.success)
    phase_end_ns = time.monotonic_ns()
    tier.drain_jobs()
    unexpected = list(tier.get_finished_jobs())
    if unexpected:
        raise ContractViolation(
            f"{direction}: completions appeared after all expected jobs: {unexpected}"
        )
    observed = tier.jobs_for_direction(direction)
    sojourn_ms = [job.observed_sojourn_ns / 1_000_000 for job in observed]
    submit_us = [job.submit_call_ns / 1_000 for job in observed]
    total_bytes = sum(job.n_bytes for job in observed)
    wall_seconds = (phase_end_ns - phase_start_ns) / 1_000_000_000
    return {
        "direction": direction,
        "jobs": len(jobs),
        "blocks": sum(len(job.keys) for job in jobs),
        "bytes": total_bytes,
        "phase_wall_ns": phase_end_ns - phase_start_ns,
        "throughput_mib_s": total_bytes / (1024 * 1024) / wall_seconds,
        "polls": polls,
        "all_success": all(results.values()),
        "job_p50_ms": percentile(sojourn_ms, 0.50),
        "job_p95_ms": percentile(sojourn_ms, 0.95),
        "job_p99_ms": percentile(sojourn_ms, 0.99),
        "job_max_ms": max(sojourn_ms),
        "submit_p50_us": percentile(submit_us, 0.50),
        "submit_p95_us": percentile(submit_us, 0.95),
        "completion_ids": sorted(results),
    }


def _run_lookup_phase(
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
    deadline = time.monotonic() + timeout
    polls = 0
    resolved = initial
    while any(result is None for result in resolved):
        if time.monotonic() >= deadline:
            unresolved = sum(result is None for result in resolved)
            raise TimeoutError(f"lookup timed out with {unresolved} unresolved keys")
        time.sleep(poll_interval)
        polls += 1
        resolved = [tier.lookup(key, req_context) for key in keys]
        if any(result is None for result in resolved):
            # Mirror the scheduler lifecycle: every retry belongs to a new
            # scheduler step, and each step ends with on_schedule_end().
            # FsAsyncLookupManager uses that hook to re-arm result draining.
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


def _file_summary(root: Path) -> dict[str, int]:
    bin_files = 0
    bin_bytes = 0
    tmp_files = 0
    other_files = 0
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        if path.suffix == ".bin":
            bin_files += 1
            bin_bytes += path.stat().st_size
        elif path.name.endswith(".tmp"):
            tmp_files += 1
        else:
            other_files += 1
    return {
        "bin_files": bin_files,
        "bin_bytes": bin_bytes,
        "tmp_files": tmp_files,
        "other_files": other_files,
    }


def _validate_environment(config: dict[str, Any]) -> dict[str, Any]:
    import torch
    import vllm

    expected = config["expected"]
    observed = {
        "python": platform.python_version(),
        "python_executable": sys.executable,
        "pythonhashseed": os.environ.get("PYTHONHASHSEED"),
        "vllm_distribution": importlib.metadata.version("vllm"),
        "vllm_module": vllm.__version__,
        "torch": torch.__version__,
        "torch_cuda": torch.version.cuda,
        "gpu_name": (
            torch.cuda.get_device_name(torch.cuda.current_device())
            if torch.cuda.is_available()
            else None
        ),
        "gpu_capability": (
            list(torch.cuda.get_device_capability(torch.cuda.current_device()))
            if torch.cuda.is_available()
            else None
        ),
        "source_commit": _git_head(expected["source_dir"]),
    }
    failures = []
    if ".".join(platform.python_version_tuple()[:2]) != expected["python_major_minor"]:
        failures.append("python_major_minor")
    for key in (
        "pythonhashseed",
        "vllm_distribution",
        "vllm_module",
        "torch",
        "torch_cuda",
        "gpu_name",
        "source_commit",
    ):
        if observed[key] != expected[key]:
            failures.append(key)
    return {"status": "PASS" if not failures else "FAIL", "observed": observed, "failures": failures}


def _run_one(
    *,
    config: dict[str, Any],
    campaign_id: str,
    run_id: str,
    window: int,
    repetition: int,
    run_output_dir: Path,
    run_data_dir: Path,
    data_root: Path,
) -> dict[str, Any]:
    from vllm.v1.kv_offload.base import ReqContext, make_offload_key
    from vllm.v1.kv_offload.tiering.base import JobMetadata
    from vllm.v1.kv_offload.tiering.fs.manager import FileSystemTierManager

    workload = config["workload"]
    safety = config["safety"]
    block_bytes = int(workload["block_bytes"])
    num_blocks = int(workload["num_blocks"])
    total_bytes = block_bytes * num_blocks
    if total_bytes > int(safety["max_backing_bytes"]):
        raise ValueError(
            f"backing bytes {total_bytes} exceed safety limit "
            f"{safety['max_backing_bytes']}"
        )
    page_size = mmap.PAGESIZE
    if block_bytes % page_size:
        raise ValueError(f"block_bytes={block_bytes} is not page aligned")

    run_output_dir.mkdir(parents=False, exist_ok=False)
    run_data_dir.mkdir(parents=True, exist_ok=False)
    manifest = {
        "schema_version": 1,
        "campaign_id": campaign_id,
        "run_id": run_id,
        "window": window,
        "repetition": repetition,
        "started_at": _utc_now(),
        "config": config,
        "data_dir": str(run_data_dir),
    }
    _write_json_once(run_output_dir / "manifest.json", manifest)

    memory = mmap.mmap(-1, total_bytes)
    array = np.ndarray((num_blocks, block_bytes), dtype=np.uint8, buffer=memory)
    for block_index in range(num_blocks):
        array[block_index].fill(
            (int(workload["seed"]) + window * 31 + repetition * 17 + block_index)
            % 251
        )
    primary_view = memoryview(array)
    expected_checksum = hashlib.sha256(primary_view.cast("B")).hexdigest()
    inner_tier = FileSystemTierManager(
        offloading_spec=_make_mock_spec(block_bytes),
        primary_kv_view=primary_view,
        tier_type="fs",
        root_dir=str(run_data_dir),
        n_read_threads=int(workload["n_read_threads"]),
        n_write_threads=int(workload["n_write_threads"]),
    )
    tier = InstrumentedTier(
        inner_tier, run_id=run_id, block_bytes=block_bytes
    )
    req_context = ReqContext(req_id=run_id)
    keys = _make_keys(run_id, num_blocks, make_offload_key)
    store_jobs = _make_jobs(
        job_id_start=1,
        keys=keys,
        blocks_per_job=int(workload["blocks_per_job"]),
        is_promotion=False,
        req_context=req_context,
        job_metadata_type=JobMetadata,
    )
    load_jobs = _make_jobs(
        job_id_start=1_000_001,
        keys=keys,
        blocks_per_job=int(workload["blocks_per_job"]),
        is_promotion=True,
        req_context=req_context,
        job_metadata_type=JobMetadata,
    )

    proc_io_before = _read_key_value_file(Path("/proc/self/io"))
    disk_before = _read_diskstats(config["system_observation"]["block_device"])
    usage_before = _rusage()
    result: dict[str, Any] | None = None
    failure: BaseException | None = None
    try:
        store = _run_transfer_phase(
            tier,
            store_jobs,
            direction="store",
            poll_interval=float(workload["poll_interval_seconds"]),
            timeout=float(workload["job_timeout_seconds"]),
        )
        lookup = _run_lookup_phase(
            tier,
            keys,
            req_context,
            poll_interval=float(workload["poll_interval_seconds"]),
            timeout=float(workload["lookup_timeout_seconds"]),
        )
        files = _file_summary(run_data_dir)
        array.fill(0)
        load = _run_transfer_phase(
            tier,
            load_jobs,
            direction="load",
            poll_interval=float(workload["poll_interval_seconds"]),
            timeout=float(workload["job_timeout_seconds"]),
        )
        observed_checksum = hashlib.sha256(primary_view.cast("B")).hexdigest()
        tier.assert_job_accounting()
        expected_jobs = len(store_jobs) + len(load_jobs)
        completed_jobs = len(tier.completed_jobs)
        correctness_checks = {
            "store_all_success": bool(store["all_success"]),
            "load_all_success": bool(load["all_success"]),
            "lookup_all_present": bool(lookup["all_present"]),
            "checksum_match": observed_checksum == expected_checksum,
            "file_count_match": files["bin_files"] == num_blocks,
            "file_bytes_match": files["bin_bytes"] == total_bytes,
            "no_tmp_files": files["tmp_files"] == 0,
            "job_count_match": completed_jobs == expected_jobs,
            "no_pending_jobs": tier.pending_job_count == 0,
        }
        correctness_pass = all(correctness_checks.values())
        result = {
            "schema_version": 1,
            "campaign_id": campaign_id,
            "run_id": run_id,
            "window": window,
            "repetition": repetition,
            "status": "PASS" if correctness_pass else "FAIL",
            "finished_at": _utc_now(),
            "workload_bytes": total_bytes,
            "store": store,
            "lookup": lookup,
            "load": load,
            "files": files,
            "checksum": {
                "algorithm": "sha256",
                "expected": expected_checksum,
                "observed": observed_checksum,
            },
            "correctness": {
                "pass": correctness_pass,
                "checks": correctness_checks,
            },
            "metrics": {
                "store_throughput_mib_s": store["throughput_mib_s"],
                "load_throughput_mib_s": load["throughput_mib_s"],
                "store_job_p95_ms": store["job_p95_ms"],
                "load_job_p95_ms": load["job_p95_ms"],
            },
        }
        return result
    except BaseException as exc:
        failure = exc
        raise
    finally:
        proc_io_after = _read_key_value_file(Path("/proc/self/io"))
        disk_after = _read_diskstats(config["system_observation"]["block_device"])
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
        except BaseException as shutdown_exc:
            shutdown_failure = shutdown_exc
            if not (run_output_dir / "shutdown_failure.json").exists():
                _write_json_once(
                    run_output_dir / "shutdown_failure.json",
                    {
                        "type": type(shutdown_exc).__name__,
                        "message": str(shutdown_exc),
                    },
                )
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
            result["system_delta"] = system_delta
            _write_json_once(run_output_dir / "result.json", result)
        if failure is not None:
            _write_json_once(
                run_output_dir / "failure.json",
                {
                    "schema_version": 1,
                    "run_id": run_id,
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
        _write_jsonl_once(run_output_dir / "tier_events.jsonl", tier.events)
        primary_view.release()
        del tier
        del inner_tier
        del array
        gc.collect()
        memory.close()
        if not bool(safety["retain_backing_files"]):
            _safe_remove_run_dir(run_data_dir, data_root)
        if result is not None and result["status"] == "PASS":
            (run_output_dir / "DONE").touch(exist_ok=False)


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
    if bool(config["safety"].get("drop_caches")):
        raise ValueError("drop_caches must remain false")
    output_root = _safe_root(args.output_root, "output root")
    data_root = _safe_root(args.data_root, "data root")
    environment = _validate_environment(config)
    harness_identity = _harness_identity(config_path)
    if environment["status"] != "PASS":
        print(json.dumps(environment, indent=2, sort_keys=True), file=sys.stderr)
        return 2

    campaign_id = (
        datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        + "-"
        + uuid.uuid4().hex[:8]
    )
    campaign_dir = output_root / campaign_id
    campaign_dir.mkdir(exist_ok=False)
    _write_json_once(
        campaign_dir / "campaign.json",
        {
            "schema_version": 1,
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
            "harness_identity": harness_identity,
            "config": config,
        },
    )

    workload = config["workload"]
    results: list[dict[str, Any]] = []
    failures = 0
    windows = int(workload["windows"])
    repetitions = int(workload["repetitions_per_window"])
    for window in range(windows):
        for repetition in range(repetitions):
            run_id = f"w{window:02d}-r{repetition:02d}"
            run_output_dir = campaign_dir / run_id
            run_data_dir = data_root / campaign_id / run_id
            try:
                result = _run_one(
                    config=config,
                    campaign_id=campaign_id,
                    run_id=run_id,
                    window=window,
                    repetition=repetition,
                    run_output_dir=run_output_dir,
                    run_data_dir=run_data_dir,
                    data_root=data_root,
                )
                results.append(result)
                print(
                    json.dumps(
                        {
                            "run_id": run_id,
                            "status": result["status"],
                            "store_mib_s": result["metrics"][
                                "store_throughput_mib_s"
                            ],
                            "load_mib_s": result["metrics"][
                                "load_throughput_mib_s"
                            ],
                        },
                        sort_keys=True,
                    ),
                    flush=True,
                )
            except BaseException as exc:
                failures += 1
                print(
                    json.dumps(
                        {
                            "run_id": run_id,
                            "status": "FAIL",
                            "type": type(exc).__name__,
                            "message": str(exc),
                        },
                        sort_keys=True,
                    ),
                    file=sys.stderr,
                    flush=True,
                )
        if window + 1 < windows:
            time.sleep(float(workload["window_pause_seconds"]))

    gate_result = evaluate_campaign(results, config["gate"])
    summary = {
        "schema_version": 1,
        "campaign_id": campaign_id,
        "finished_at": _utc_now(),
        "attempted_runs": windows * repetitions,
        "runner_failures": failures,
        "gate": gate_result,
    }
    _write_json_once(campaign_dir / "summary.json", summary)
    if gate_result["status"] in {"PASS", "WARN"}:
        (campaign_dir / "DONE").touch(exist_ok=False)
    print(json.dumps(summary, indent=2, sort_keys=True), flush=True)
    return 0 if gate_result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
