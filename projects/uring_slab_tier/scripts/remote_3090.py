#!/usr/bin/env python3
"""Safe orchestration for the fingerprint-pinned RTX 3090 experiment host.

The host address is deliberately not stored in the repository. The script
resolves only entries in the caller's known_hosts whose public-key fingerprint
matches the value already recorded in the evidence index.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import shlex
import subprocess
import sys
from pathlib import Path


EXPECTED_HOST_KEY_SHA256 = (
    "Xe3L/8ja9Qo37KrfvElLR58YTOhv0KiEG8idfv/CEr0"
)
REMOTE_ROOT = Path("/root/uring-slab-experiments")
REMOTE_SOURCE = REMOTE_ROOT / "source/uring-slab-tier"
REMOTE_PYTHON = (
    REMOTE_ROOT / "venvs/vllm024-cu129-clean/bin/python"
)


def _fingerprint(encoded_key: str) -> str:
    blob = base64.b64decode(encoded_key)
    return base64.b64encode(hashlib.sha256(blob).digest()).decode().rstrip("=")


def _known_host_endpoints() -> list[tuple[str, int]]:
    known_hosts = Path.home() / ".ssh/known_hosts"
    if not known_hosts.exists():
        raise RuntimeError("~/.ssh/known_hosts does not exist")
    endpoints: list[tuple[str, int]] = []
    for line in known_hosts.read_text(errors="replace").splitlines():
        fields = line.split()
        if len(fields) < 3 or line.startswith("#"):
            continue
        try:
            fingerprint = _fingerprint(fields[2])
        except Exception:
            continue
        if fingerprint != EXPECTED_HOST_KEY_SHA256:
            continue
        for host_field in fields[0].split(","):
            if host_field.startswith("|1|"):
                continue
            if host_field.startswith("[") and "]:" in host_field:
                host, raw_port = host_field[1:].rsplit("]:", 1)
                endpoint = (host, int(raw_port))
            else:
                endpoint = (host_field, 22)
            if endpoint not in endpoints:
                endpoints.append(endpoint)
    if not endpoints:
        raise RuntimeError("no known_hosts entry matches the pinned fingerprint")
    return endpoints


def _ssh_argv(host: str, port: int, remote_command: str) -> list[str]:
    return [
        "ssh",
        "-p",
        str(port),
        "-l",
        "root",
        "-o",
        "BatchMode=yes",
        "-o",
        "StrictHostKeyChecking=yes",
        "-o",
        "ConnectTimeout=8",
        host,
        remote_command,
    ]


def _resolve_live_endpoint() -> tuple[str, int]:
    probe = (
        "test \"$(uname -r)\" = 6.8.0-31-generic "
        "&& nvidia-smi --query-gpu=name --format=csv,noheader "
        "| grep -Fx 'NVIDIA GeForce RTX 3090'"
    )
    for host, port in _known_host_endpoints():
        result = subprocess.run(
            _ssh_argv(host, port, probe),
            check=False,
            capture_output=True,
            text=True,
            timeout=15,
        )
        if result.returncode == 0:
            return host, port
    raise RuntimeError("no fingerprint-matched endpoint passed the 3090 probe")


def _run_remote(
    remote_command: str,
    *,
    capture_output: bool = False,
) -> subprocess.CompletedProcess[str]:
    host, port = _resolve_live_endpoint()
    return subprocess.run(
        _ssh_argv(host, port, remote_command),
        check=False,
        capture_output=capture_output,
        text=True,
    )


def _remote_spec(host: str, path: Path) -> str:
    rendered_host = f"[{host}]" if ":" in host else host
    return f"root@{rendered_host}:{path}"


def command_probe() -> int:
    result = _run_remote(
        "uname -r; "
        "nvidia-smi --query-gpu=name,driver_version,memory.total "
        "--format=csv,noheader; "
        f"test -x {shlex.quote(str(REMOTE_PYTHON))} && echo venv=present",
    )
    return result.returncode


def command_sync() -> int:
    project_root = Path(__file__).resolve().parents[1]
    mkdir = _run_remote(f"mkdir -p {shlex.quote(str(REMOTE_SOURCE))}")
    if mkdir.returncode != 0:
        return mkdir.returncode
    host, port = _resolve_live_endpoint()
    ssh_transport = (
        f"ssh -p {port} -l root -o BatchMode=yes "
        "-o StrictHostKeyChecking=yes -o ConnectTimeout=8"
    )
    command = [
        "rsync",
        "-az",
        "--checksum",
        "--exclude",
        ".git/",
        "--exclude",
        "__pycache__/",
        "--exclude",
        "*.pyc",
        "--exclude",
        "evidence/*/raw/",
        "--exclude",
        "results/",
        "--exclude",
        "data/",
        "--exclude",
        ".venv/",
        "-e",
        ssh_transport,
        f"{project_root}/",
        _remote_spec(host, REMOTE_SOURCE) + "/",
    ]
    return subprocess.run(command, check=False).returncode


def command_test() -> int:
    command = (
        f"cd {shlex.quote(str(REMOTE_SOURCE))} && "
        "PYTHONHASHSEED=0 "
        f"PYTHONPATH={shlex.quote(str(REMOTE_SOURCE / 'src'))} "
        f"{shlex.quote(str(REMOTE_PYTHON))} "
        "-m unittest discover -s tests -v"
    )
    return _run_remote(command).returncode


def command_campaign(config_name: str) -> int:
    if config_name not in {
        "native_fs_paired_aa_smoke.json",
        "native_fs_paired_aa_v1.json",
    }:
        raise ValueError(f"unsupported campaign config {config_name!r}")
    suffix = "smoke" if config_name.endswith("smoke.json") else "formal"
    command = (
        f"cd {shlex.quote(str(REMOTE_SOURCE))} && "
        "PYTHONHASHSEED=0 "
        f"PYTHONPATH={shlex.quote(str(REMOTE_SOURCE / 'src'))} "
        f"{shlex.quote(str(REMOTE_PYTHON))} "
        "-m uring_slab_tier.paired_fs_aa "
        f"--config configs/{shlex.quote(config_name)} "
        f"--output-root {shlex.quote(str(REMOTE_ROOT / 'results/paired-fs-aa' / suffix))} "
        f"--data-root {shlex.quote(str(REMOTE_ROOT / 'data/paired-fs-aa' / suffix))}"
    )
    return _run_remote(command).returncode


def command_pull(kind: str) -> int:
    if kind not in {"smoke", "formal"}:
        raise ValueError(f"unsupported pull kind {kind!r}")
    remote_parent = REMOTE_ROOT / "results/paired-fs-aa" / kind
    latest = _run_remote(
        f"find {shlex.quote(str(remote_parent))} "
        "-mindepth 1 -maxdepth 1 -type d -printf '%f\\n' "
        "| LC_ALL=C sort | tail -n 1",
        capture_output=True,
    )
    if latest.returncode != 0:
        return latest.returncode
    campaign_id = latest.stdout.strip()
    if not campaign_id or "/" in campaign_id or campaign_id.startswith("."):
        raise RuntimeError("remote did not return a safe campaign id")

    project_root = Path(__file__).resolve().parents[1]
    if kind == "formal":
        local_parent = project_root / "evidence/paired_fs_aa/raw"
    else:
        local_parent = project_root / "results/remote-smoke"
    local_campaign = local_parent / campaign_id
    if local_campaign.exists():
        raise RuntimeError(
            f"refusing to overwrite existing local campaign {campaign_id}"
        )
    local_campaign.mkdir(parents=True, exist_ok=False)

    host, port = _resolve_live_endpoint()
    ssh_transport = (
        f"ssh -p {port} -l root -o BatchMode=yes "
        "-o StrictHostKeyChecking=yes -o ConnectTimeout=8"
    )
    command = [
        "rsync",
        "-az",
        "--checksum",
        "-e",
        ssh_transport,
        _remote_spec(host, remote_parent / campaign_id) + "/",
        f"{local_campaign}/",
    ]
    result = subprocess.run(command, check=False)
    if result.returncode == 0:
        print(f"pulled_campaign={campaign_id}")
    return result.returncode


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "command",
        choices=(
            "probe",
            "sync",
            "test",
            "smoke",
            "formal",
            "pull-smoke",
            "pull-formal",
        ),
    )
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    if args.command == "probe":
        return command_probe()
    if args.command == "sync":
        return command_sync()
    if args.command == "test":
        return command_test()
    if args.command == "smoke":
        return command_campaign("native_fs_paired_aa_smoke.json")
    if args.command == "formal":
        return command_campaign("native_fs_paired_aa_v1.json")
    if args.command == "pull-smoke":
        return command_pull("smoke")
    if args.command == "pull-formal":
        return command_pull("formal")
    raise AssertionError(args.command)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"{type(exc).__name__}: {exc}", file=sys.stderr)
        raise SystemExit(2)
