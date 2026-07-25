#!/usr/bin/env python3
"""Validate the exact vLLM runtime needed by this project.

This script intentionally does not download a model or start a server. It
checks the package/source identity, CUDA execution, and the v0.24.0 tiering
interfaces that the adapter will import.
"""

from __future__ import annotations

import json
import os
import platform
import subprocess
import sys
from importlib.metadata import version as distribution_version
from pathlib import Path
from typing import Any


EXPECTED_VLLM_RELEASE = "0.24.0"
EXPECTED_VLLM_BUILD = "0.24.0+cu129"
EXPECTED_VLLM_COMMIT = "ee0da84ab9e04ac7610e28580af62c365e898389"


def git_head(source_dir: Path) -> str | None:
    if not source_dir.is_dir():
        return None
    result = subprocess.run(
        ["git", "-C", str(source_dir), "rev-parse", "HEAD"],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def main() -> int:
    source_dir = Path(
        os.environ.get(
            "VLLM_SOURCE_DIR",
            "/root/uring-slab-experiments/source/vllm",
        )
    )
    report: dict[str, Any] = {
        "schema_version": 1,
        "python_executable": sys.executable,
        "python_version": sys.version,
        "platform": platform.platform(),
        "expected_vllm_release": EXPECTED_VLLM_RELEASE,
        "expected_vllm_build": EXPECTED_VLLM_BUILD,
        "expected_vllm_commit": EXPECTED_VLLM_COMMIT,
        "source_dir": str(source_dir),
    }
    failures: list[str] = []

    try:
        import torch
    except Exception as exc:  # pragma: no cover - runtime evidence path
        report["torch_import"] = {
            "status": "FAIL",
            "type": type(exc).__name__,
            "message": str(exc),
        }
        failures.append("torch import failed")
    else:
        report["torch"] = {
            "version": torch.__version__,
            "cuda_runtime": torch.version.cuda,
            "cuda_available": torch.cuda.is_available(),
            "device_count": torch.cuda.device_count(),
        }
        if not torch.cuda.is_available():
            failures.append("CUDA is unavailable to PyTorch")
        else:
            device = torch.cuda.current_device()
            tensor = torch.arange(1 << 20, device=device, dtype=torch.int64)
            observed = int(tensor.sum().item())
            expected = ((1 << 20) - 1) * (1 << 20) // 2
            torch.cuda.synchronize(device)
            report["torch"].update(
                {
                    "device_index": device,
                    "device_name": torch.cuda.get_device_name(device),
                    "device_capability": list(
                        torch.cuda.get_device_capability(device)
                    ),
                    "checksum_observed": observed,
                    "checksum_expected": expected,
                    "cuda_smoke": "PASS" if observed == expected else "FAIL",
                }
            )
            if observed != expected:
                failures.append("CUDA checksum mismatch")

    try:
        import vllm
    except Exception as exc:  # pragma: no cover - runtime evidence path
        report["vllm_import"] = {
            "status": "FAIL",
            "type": type(exc).__name__,
            "message": str(exc),
        }
        failures.append("vLLM import failed")
    else:
        actual_version = getattr(vllm, "__version__", None)
        installed_distribution = distribution_version("vllm")
        report["vllm"] = {
            "module_version": actual_version,
            "distribution_version": installed_distribution,
            "path": getattr(vllm, "__file__", None),
        }
        if actual_version != EXPECTED_VLLM_RELEASE:
            failures.append(
                f"vLLM release mismatch: expected {EXPECTED_VLLM_RELEASE}, "
                f"got {actual_version}"
            )
        if installed_distribution != EXPECTED_VLLM_BUILD:
            failures.append(
                f"vLLM build mismatch: expected {EXPECTED_VLLM_BUILD}, "
                f"got {installed_distribution}"
            )

        try:
            from vllm.distributed.kv_transfer.kv_connector.v1.offloading_connector import (
                OffloadingConnector,
            )
            from vllm.v1.kv_offload.tiering.base import SecondaryTierManager
            from vllm.v1.kv_offload.tiering.factory import SecondaryTierFactory
            from vllm.v1.kv_offload.tiering.fs.manager import (
                FileSystemTierManager,
            )
        except Exception as exc:
            report["tiering_imports"] = {
                "status": "FAIL",
                "type": type(exc).__name__,
                "message": str(exc),
            }
            failures.append("vLLM tiering interface import failed")
        else:
            report["tiering_imports"] = {
                "status": "PASS",
                "OffloadingConnector": f"{OffloadingConnector.__module__}.{OffloadingConnector.__name__}",
                "SecondaryTierManager": f"{SecondaryTierManager.__module__}.{SecondaryTierManager.__name__}",
                "SecondaryTierFactory": f"{SecondaryTierFactory.__module__}.{SecondaryTierFactory.__name__}",
                "FileSystemTierManager": f"{FileSystemTierManager.__module__}.{FileSystemTierManager.__name__}",
            }

    actual_commit = git_head(source_dir)
    report["source_commit"] = actual_commit
    if actual_commit != EXPECTED_VLLM_COMMIT:
        failures.append(
            f"source commit mismatch: expected {EXPECTED_VLLM_COMMIT}, "
            f"got {actual_commit}"
        )

    report["failures"] = failures
    report["status"] = "PASS" if not failures else "FAIL"
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
