from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import socket
import subprocess
import sys
import time
import traceback
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from uring_slab_tier.noise_gate import evaluate_campaign


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _write_json_once(path: Path, value: Any) -> None:
    with path.open("x", encoding="utf-8") as output:
        json.dump(value, output, indent=2, sort_keys=True)
        output.write("\n")


def _safe_root(path: Path, label: str) -> Path:
    resolved = path.expanduser().resolve()
    forbidden = {Path("/"), Path.home().resolve(), Path("/root"), Path("/tmp")}
    if resolved in forbidden or len(resolved.parts) < 4:
        raise ValueError(f"unsafe {label}: {resolved}")
    resolved.mkdir(parents=True, exist_ok=True)
    return resolved


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


def _load_config(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as source:
        config = json.load(source)
    if int(config.get("schema_version", -1)) != 1:
        raise ValueError("only config schema_version=1 is supported")
    if bool(config["safety"].get("drop_caches")):
        raise ValueError("drop_caches must remain false")
    num_bytes = int(config["workload"]["num_bytes"])
    block_bytes = int(config["workload"]["block_bytes"])
    if num_bytes > int(config["safety"]["max_backing_bytes"]):
        raise ValueError("backing file exceeds safety limit")
    if num_bytes % block_bytes:
        raise ValueError("num_bytes must be divisible by block_bytes")
    return config


def _fio_version() -> str:
    result = subprocess.run(
        ["fio", "--version"], check=True, capture_output=True, text=True
    )
    return result.stdout.strip()


def _fio_command(
    *,
    name: str,
    filename: Path,
    rw: str,
    config: dict[str, Any],
    seed: int,
) -> list[str]:
    workload = config["workload"]
    return [
        "fio",
        f"--name={name}",
        f"--filename={filename}",
        f"--rw={rw}",
        f"--ioengine={workload['ioengine']}",
        "--direct=1",
        f"--bs={int(workload['block_bytes'])}",
        f"--size={int(workload['num_bytes'])}",
        f"--iodepth={int(workload['iodepth'])}",
        "--numjobs=1",
        "--group_reporting=1",
        "--time_based=0",
        "--randrepeat=1",
        f"--randseed={seed}",
        "--norandommap=0",
        "--invalidate=1",
        "--output-format=json",
    ]


def _run_fio(command: list[str]) -> dict[str, Any]:
    result = subprocess.run(command, check=False, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"fio exited {result.returncode}: {result.stderr.strip()}"
        )
    return json.loads(result.stdout)


def _direction_metrics(
    fio_output: dict[str, Any], direction: str, expected_bytes: int
) -> dict[str, Any]:
    jobs = fio_output.get("jobs", [])
    if len(jobs) != 1:
        raise RuntimeError(f"expected one fio job, got {len(jobs)}")
    job = jobs[0]
    section = job[direction]
    clat = section["clat_ns"]
    percentiles = clat.get("percentile", {})
    p95_ns = float(percentiles["95.000000"])
    p99_ns = float(percentiles["99.000000"])
    io_bytes = int(section["io_bytes"])
    error = int(job["error"])
    return {
        "error": error,
        "io_bytes": io_bytes,
        "expected_bytes": expected_bytes,
        "bytes_match": io_bytes == expected_bytes,
        "runtime_ms": int(section["runtime"]),
        "bw_bytes_s": float(section["bw_bytes"]),
        "throughput_mib_s": float(section["bw_bytes"]) / (1024 * 1024),
        "iops": float(section["iops"]),
        "clat_p95_ms": p95_ns / 1_000_000,
        "clat_p99_ms": p99_ns / 1_000_000,
        "clat_max_ms": float(clat["max"]) / 1_000_000,
    }


def _prepare_file(path: Path, config: dict[str, Any]) -> dict[str, Any]:
    flags = os.O_CREAT | os.O_EXCL | os.O_RDWR
    fd = os.open(path, flags, 0o600)
    try:
        os.posix_fallocate(fd, 0, int(config["workload"]["num_bytes"]))
    finally:
        os.close(fd)
    command = _fio_command(
        name="prepare",
        filename=path,
        rw="write",
        config=config,
        seed=int(config["workload"]["seed"]),
    )
    output = _run_fio(command)
    metrics = _direction_metrics(
        output, "write", int(config["workload"]["num_bytes"])
    )
    if metrics["error"] or not metrics["bytes_match"]:
        raise RuntimeError(f"fio prepare failed: {metrics}")
    return {"command": command, "metrics": metrics, "fio": output}


def _run_one(
    *,
    config: dict[str, Any],
    campaign_id: str,
    run_id: str,
    window: int,
    repetition: int,
    data_file: Path,
    run_dir: Path,
) -> dict[str, Any]:
    run_dir.mkdir(exist_ok=False)
    seed_policy = str(config["workload"].get("seed_policy", "per_run"))
    if seed_policy == "fixed":
        seed = int(config["workload"]["seed"])
    elif seed_policy == "per_run":
        seed = int(config["workload"]["seed"]) + window * 1000 + repetition
    else:
        raise ValueError(f"unknown seed_policy={seed_policy!r}")
    write_command = _fio_command(
        name=f"{run_id}-write",
        filename=data_file,
        rw="randwrite",
        config=config,
        seed=seed,
    )
    read_command = _fio_command(
        name=f"{run_id}-read",
        filename=data_file,
        rw="randread",
        config=config,
        seed=seed,
    )
    _write_json_once(
        run_dir / "manifest.json",
        {
            "schema_version": 1,
            "campaign_id": campaign_id,
            "run_id": run_id,
            "window": window,
            "repetition": repetition,
            "seed": seed,
            "seed_policy": seed_policy,
            "write_command": write_command,
            "read_command": read_command,
            "started_at": _utc_now(),
        },
    )
    try:
        write_output = _run_fio(write_command)
        read_output = _run_fio(read_command)
        _write_json_once(run_dir / "fio_write.json", write_output)
        _write_json_once(run_dir / "fio_read.json", read_output)
        expected_bytes = int(config["workload"]["num_bytes"])
        write = _direction_metrics(write_output, "write", expected_bytes)
        read = _direction_metrics(read_output, "read", expected_bytes)
        checks = {
            "write_error_zero": write["error"] == 0,
            "read_error_zero": read["error"] == 0,
            "write_bytes_match": write["bytes_match"],
            "read_bytes_match": read["bytes_match"],
        }
        status = "PASS" if all(checks.values()) else "FAIL"
        result = {
            "schema_version": 1,
            "campaign_id": campaign_id,
            "run_id": run_id,
            "window": window,
            "repetition": repetition,
            "status": status,
            "finished_at": _utc_now(),
            "write": write,
            "read": read,
            "correctness": {"pass": all(checks.values()), "checks": checks},
            "metrics": {
                "store_throughput_mib_s": write["throughput_mib_s"],
                "load_throughput_mib_s": read["throughput_mib_s"],
                "store_job_p95_ms": write["clat_p95_ms"],
                "load_job_p95_ms": read["clat_p95_ms"],
            },
        }
        _write_json_once(run_dir / "result.json", result)
        if status == "PASS":
            (run_dir / "DONE").touch(exist_ok=False)
        return result
    except BaseException as exc:
        _write_json_once(
            run_dir / "failure.json",
            {
                "schema_version": 1,
                "run_id": run_id,
                "type": type(exc).__name__,
                "message": str(exc),
                "traceback": "".join(
                    traceback.format_exception(type(exc), exc, exc.__traceback__)
                ),
            },
        )
        raise


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
    observed_fio_version = _fio_version()
    expected_fio_version = config["expected"]["fio_version"]
    if observed_fio_version != expected_fio_version:
        print(
            f"fio version mismatch: expected {expected_fio_version}, "
            f"got {observed_fio_version}",
            file=sys.stderr,
        )
        return 2

    output_root = _safe_root(args.output_root, "output root")
    data_root = _safe_root(args.data_root, "data root")
    campaign_id = (
        datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        + "-"
        + uuid.uuid4().hex[:8]
    )
    campaign_dir = output_root / campaign_id
    campaign_dir.mkdir(exist_ok=False)
    campaign_data_dir = data_root / campaign_id
    campaign_data_dir.mkdir(parents=True, exist_ok=False)
    data_file = campaign_data_dir / "preallocated-control.bin"
    campaign = {
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
        "fio_version": observed_fio_version,
        "harness_identity": _harness_identity(config_path),
        "config": config,
    }
    _write_json_once(campaign_dir / "campaign.json", campaign)

    try:
        prepare = _prepare_file(data_file, config)
        _write_json_once(campaign_dir / "prepare.json", prepare)
        workload = config["workload"]
        windows = int(workload["windows"])
        repetitions = int(workload["repetitions_per_window"])
        results: list[dict[str, Any]] = []
        failures = 0
        for window in range(windows):
            for repetition in range(repetitions):
                run_id = f"w{window:02d}-r{repetition:02d}"
                try:
                    result = _run_one(
                        config=config,
                        campaign_id=campaign_id,
                        run_id=run_id,
                        window=window,
                        repetition=repetition,
                        data_file=data_file,
                        run_dir=campaign_dir / run_id,
                    )
                    results.append(result)
                    print(
                        json.dumps(
                            {
                                "run_id": run_id,
                                "status": result["status"],
                                "write_mib_s": result["write"][
                                    "throughput_mib_s"
                                ],
                                "read_mib_s": result["read"]["throughput_mib_s"],
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

        gate = evaluate_campaign(results, config["gate"])
        summary = {
            "schema_version": 1,
            "campaign_id": campaign_id,
            "finished_at": _utc_now(),
            "attempted_runs": windows * repetitions,
            "runner_failures": failures,
            "gate": gate,
        }
        _write_json_once(campaign_dir / "summary.json", summary)
        if gate["status"] in {"PASS", "WARN"}:
            (campaign_dir / "DONE").touch(exist_ok=False)
        print(json.dumps(summary, indent=2, sort_keys=True), flush=True)
        return 0 if gate["status"] == "PASS" else 1
    finally:
        if not bool(config["safety"]["retain_backing_file"]):
            data_file.unlink(missing_ok=True)
            try:
                campaign_data_dir.rmdir()
            except OSError:
                pass


if __name__ == "__main__":
    raise SystemExit(main())
