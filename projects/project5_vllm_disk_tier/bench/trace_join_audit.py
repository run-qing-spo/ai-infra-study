#!/usr/bin/env python3
"""Audit request-ID joins across one end-to-end result directory.

This is the first (identity) gate of the measurement system.  It deliberately
does not claim that timing hook semantics are correct; known-delay and A/A
tests are separate gates.
"""

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path


def read_jsonl(path):
    rows = []
    with path.open() as f:
        for line_no, line in enumerate(f, 1):
            if not line.strip():
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path.name}:{line_no}: invalid JSON: {exc}")
    return rows


def flatten_samples(group_data):
    samples = group_data.get("samples")
    if isinstance(samples, list):
        return samples
    if isinstance(samples, dict):
        return [row for rows in samples.values() for row in rows]
    raise ValueError("group JSON has neither list nor dict samples")


def audit_group(result_dir, group):
    errors = []
    warnings = []

    def check(ok, message):
        if not ok:
            errors.append(message)

    group_path = result_dir / f"group_{group}.json"
    map_path = result_dir / f"request_map_{group}.jsonl"
    ledger_path = result_dir / f"tier_stats_{group}.sched.records.jsonl"
    engine_path = result_dir / f"tier_stats_{group}.records.jsonl"

    if not group_path.exists():
        return [f"missing {group_path.name}"], warnings, {}
    if not map_path.exists():
        return [f"missing {map_path.name}"], warnings, {}

    group_data = json.loads(group_path.read_text())
    samples = flatten_samples(group_data)
    maps = read_jsonl(map_path)
    ledger = read_jsonl(ledger_path) if ledger_path.exists() else []

    sample_by_req = {}
    for i, sample in enumerate(samples):
        req_id = sample.get("req_id")
        check(bool(req_id), f"client sample #{i} has no req_id")
        if not req_id:
            continue
        check(req_id not in sample_by_req, f"duplicate client req_id {req_id}")
        sample_by_req[req_id] = sample
        check(sample.get("response_id") == f"cmpl-{req_id}",
              f"{req_id}: response_id does not equal cmpl-<req_id>")

    maps_by_req = defaultdict(list)
    maps_by_internal = {}
    for row in maps:
        req_id = row.get("req_id")
        internal = row.get("vllm_internal_req_id")
        external = row.get("vllm_external_req_id")
        check(bool(req_id), "request map row has no req_id")
        check(bool(internal), f"{req_id}: request map has no internal ID")
        check(bool(external), f"{req_id}: request map has no external ID")
        if not req_id or not internal:
            continue
        maps_by_req[req_id].append(row)
        check(internal not in maps_by_internal,
              f"duplicate vLLM internal ID {internal}")
        maps_by_internal[internal] = row
        if isinstance(external, str):
            check(external.startswith(f"cmpl-{req_id}-"),
                  f"{req_id}: unexpected vLLM external ID {external!r}")
        if isinstance(internal, str) and isinstance(external, str):
            check(internal.startswith(external + "-"),
                  f"{req_id}: internal ID is not derived from external ID")

    for req_id in sample_by_req:
        check(len(maps_by_req.get(req_id, ())) == 1,
              f"{req_id}: expected exactly one external->internal map, got "
              f"{len(maps_by_req.get(req_id, ()))}")
    for req_id in maps_by_req:
        check(req_id in sample_by_req,
              f"{req_id}: mapped by server but absent from client samples")

    run_ids = {row.get("run_id") for row in samples + maps + ledger}
    run_ids.discard(None)
    run_ids.discard("")
    check(len(run_ids) == 1, f"expected one non-empty run_id, got {sorted(run_ids)}")

    job_keys = set()
    for row in ledger:
        req_id = row.get("req_id")
        internal = row.get("vllm_internal_req_id")
        key = (row.get("tier"), row.get("job_id"))
        check(key not in job_keys, f"duplicate tier job key {key}")
        job_keys.add(key)
        check(req_id in sample_by_req,
              f"tier job {key}: req_id {req_id!r} absent from client samples")
        check(internal in maps_by_internal,
              f"tier job {key}: internal ID absent from request map")
        if internal in maps_by_internal:
            check(maps_by_internal[internal].get("req_id") == req_id,
                  f"tier job {key}: canonical req_id disagrees with map")
        check(isinstance(row.get("success"), bool),
              f"tier job {key}: success is not boolean")
        check(row.get("t_done_mono_ns", -1) >= row.get("t_submit_mono_ns", 0),
              f"tier job {key}: monotonic timestamps are reversed")

    engine_rows = []
    if engine_path.exists():
        engine_rows = read_jsonl(engine_path)
        ledger_job_ids = {row.get("job_id") for row in ledger}
        for i, row in enumerate(engine_rows):
            # The C++ engine keeps its compact positional schema.  job_id is
            # column 0 and joins through the scheduler ledger to req_id.
            check(isinstance(row, list) and len(row) >= 1,
                  f"engine row #{i} is not a positional job record")
            if isinstance(row, list) and row:
                check(row[0] in ledger_job_ids,
                      f"engine job {row[0]} has no scheduler-ledger foreign key")

    if not ledger:
        warnings.append("no scheduler tier ledger (valid only for no-tier runs)")

    counts = {
        "client_requests": len(samples),
        "request_maps": len(maps),
        "tier_jobs": len(ledger),
        "engine_jobs": len(engine_rows),
    }
    return errors, warnings, counts


def discover_groups(result_dir):
    return sorted(
        path.stem[len("group_"):]
        for path in result_dir.glob("group_*.json")
    )


def main():
    parser = argparse.ArgumentParser(description="audit req_id joins")
    parser.add_argument("result_dir", type=Path)
    parser.add_argument("--group", action="append",
                        help="group name; repeatable (default: discover all)")
    args = parser.parse_args()

    groups = args.group or discover_groups(args.result_dir)
    if not groups:
        raise SystemExit("no group_*.json files found")

    failed = False
    for group in groups:
        errors, warnings, counts = audit_group(args.result_dir, group)
        status = "FAIL" if errors else "PASS"
        failed = failed or bool(errors)
        print(f"[{status}] {group}: " + " ".join(
            f"{key}={value}" for key, value in counts.items()))
        for message in errors:
            print(f"  ERROR: {message}")
        for message in warnings:
            print(f"  WARN:  {message}")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
