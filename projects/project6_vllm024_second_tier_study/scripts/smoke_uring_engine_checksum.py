#!/usr/bin/env python3
"""Full-buffer checksum smoke test for the reused uring-slab prototype."""

from __future__ import annotations

import argparse
import hashlib
import json
import mmap
import os
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--path", required=True)
    parser.add_argument("--blocks", type=int, default=64)
    parser.add_argument("--block-bytes", type=int, default=1 << 20)
    parser.add_argument("--queue-depth", type=int, default=32)
    parser.add_argument("--prewarm", action="store_true")
    return parser.parse_args()


def wait_for_one(engine: object, expected_job_id: int) -> None:
    engine.drain()
    completed = engine.poll_finished()
    if completed != [(expected_job_id, True)]:
        raise RuntimeError(
            f"job {expected_job_id}: expected one successful completion, "
            f"got {completed!r}"
        )


def main() -> int:
    args = parse_args()
    path = Path(args.path).resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.unlink(missing_ok=True)

    from kv_uring_tier import _kvtier

    total_bytes = args.blocks * args.block_bytes
    region = mmap.mmap(-1, total_bytes)
    view = memoryview(region)

    for block_id in range(args.blocks):
        digest = hashlib.sha256(f"block:{block_id}".encode()).digest()
        payload = (digest * (args.block_bytes // len(digest) + 1))[
            : args.block_bytes
        ]
        start = block_id * args.block_bytes
        view[start : start + args.block_bytes] = payload

    expected_sha256 = hashlib.sha256(view).hexdigest()
    slots = list(range(args.blocks))
    engine = _kvtier.Engine(
        view,
        str(path),
        args.blocks,
        args.block_bytes,
        args.queue_depth,
        True,
        args.prewarm,
    )

    try:
        if not engine.submit_store(1, slots, slots):
            raise RuntimeError("store submission rejected")
        wait_for_one(engine, 1)
        after_store = engine.stats()

        view[:] = b"\x00" * total_bytes
        zero_sha256 = hashlib.sha256(view).hexdigest()
        if not engine.submit_load(2, slots, slots):
            raise RuntimeError("load submission rejected")
        wait_for_one(engine, 2)
        observed_sha256 = hashlib.sha256(view).hexdigest()
        after_load = engine.stats()
    finally:
        del engine

    report = {
        "schema_version": 1,
        "engine": "uring-slab-prototype",
        "path": str(path),
        "filesystem": os.statvfs(path.parent).f_fsid,
        "blocks": args.blocks,
        "block_bytes": args.block_bytes,
        "total_bytes": total_bytes,
        "queue_depth": args.queue_depth,
        "odirect": True,
        "prewarm": args.prewarm,
        "expected_sha256": expected_sha256,
        "zero_sha256": zero_sha256,
        "observed_sha256": observed_sha256,
        "after_store": dict(after_store),
        "after_load": dict(after_load),
        "status": "PASS" if observed_sha256 == expected_sha256 else "FAIL",
    }
    print(json.dumps(report, indent=2, sort_keys=True))

    view.release()
    region.close()
    path.unlink(missing_ok=True)
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
