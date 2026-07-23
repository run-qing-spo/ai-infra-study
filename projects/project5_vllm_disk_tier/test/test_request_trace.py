import sys
import json
from pathlib import Path
from types import SimpleNamespace


sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "bench"))

from kv_uring_tier.request_ids import extract_req_id, make_req_id  # noqa: E402
from kv_uring_tier.request_trace import _mapping_row  # noqa: E402
from kv_uring_tier.sched_ledger import _instrument_tier  # noqa: E402
from trace_join_audit import audit_group  # noqa: E402


def test_req_id_is_deterministic_and_embeddable():
    req_id = make_req_id("run-a", "revisit:1:session:7")
    assert req_id == make_req_id("run-a", "revisit:1:session:7")
    assert req_id != make_req_id("run-b", "revisit:1:session:7")
    assert extract_req_id(req_id) == req_id
    assert extract_req_id(f"cmpl-{req_id}") == req_id
    assert extract_req_id(f"cmpl-{req_id}-0-deadbeef") == req_id
    assert extract_req_id("cmpl-unmanaged") is None


def test_request_mapping_preserves_native_ids():
    req_id = make_req_id("run-a", "prime:1:session:0")
    external = f"cmpl-{req_id}-0"
    internal = f"{external}-cafebabe"
    row = _mapping_row(
        SimpleNamespace(external_req_id=external, request_id=internal),
        "run-a",
    )
    assert row is not None
    assert row["req_id"] == req_id
    assert row["vllm_external_req_id"] == external
    assert row["vllm_internal_req_id"] == internal
    assert _mapping_row(
        SimpleNamespace(external_req_id="cmpl-random", request_id="random-1"),
        "run-a",
    ) is None


class FakeTier:
    tier_type = "fs"

    def __init__(self):
        self.finished = []

    def submit_load(self, job_metadata):
        return None

    def submit_store(self, job_metadata):
        return None

    def get_finished_jobs(self):
        rows, self.finished = self.finished, []
        return rows


def test_scheduler_ledger_carries_req_id_and_success(monkeypatch):
    monkeypatch.setenv("KVTIER_RUN_ID", "run-a")
    req_id = make_req_id("run-a", "revisit:1:session:0")
    internal = f"cmpl-{req_id}-0-12345678"
    metadata = SimpleNamespace(
        job_id=9,
        block_ids=[1, 2, 3],
        is_promotion=True,
        req_context=SimpleNamespace(req_id=internal),
    )
    tier = FakeTier()
    rows = []
    _instrument_tier(tier, rows.append)

    tier.submit_load(metadata)
    tier.finished.append(SimpleNamespace(job_id=9, success=True))
    tier.get_finished_jobs()

    assert len(rows) == 1
    row = rows[0]
    assert row["run_id"] == "run-a"
    assert row["req_id"] == req_id
    assert row["vllm_internal_req_id"] == internal
    assert row["job_id"] == 9
    assert row["is_promotion"] is True
    assert row["is_write"] is False
    assert row["success"] is True
    assert row["n_blocks"] == 3
    assert row["t_done_mono_ns"] >= row["t_submit_mono_ns"]


def test_join_audit_accepts_complete_chain_and_rejects_missing_map(tmp_path):
    group = "E3_fs_a"
    run_id = "results-1:E3_fs_a"
    req_id = make_req_id(run_id, "revisit:1:session:0")
    response_id = f"cmpl-{req_id}"
    external = f"{response_id}-0"
    internal = f"{external}-12345678"

    group_data = {
        "config": {"run_id": run_id},
        "samples": {
            "revisit@1": [{
                "run_id": run_id,
                "req_id": req_id,
                "response_id": response_id,
                "ttft": 0.1,
            }]
        },
    }
    (tmp_path / f"group_{group}.json").write_text(json.dumps(group_data))
    map_row = {
        "run_id": run_id,
        "req_id": req_id,
        "vllm_external_req_id": external,
        "vllm_internal_req_id": internal,
    }
    (tmp_path / f"request_map_{group}.jsonl").write_text(
        json.dumps(map_row) + "\n"
    )
    ledger_row = {
        "run_id": run_id,
        "tier": "fs",
        "job_id": 7,
        "req_id": req_id,
        "vllm_internal_req_id": internal,
        "success": True,
        "t_submit_mono_ns": 10,
        "t_done_mono_ns": 20,
    }
    (tmp_path / f"tier_stats_{group}.sched.records.jsonl").write_text(
        json.dumps(ledger_row) + "\n"
    )

    errors, warnings, counts = audit_group(tmp_path, group)
    assert errors == []
    assert warnings == []
    assert counts == {
        "client_requests": 1,
        "request_maps": 1,
        "tier_jobs": 1,
        "engine_jobs": 0,
    }

    (tmp_path / f"request_map_{group}.jsonl").write_text("")
    errors, _, _ = audit_group(tmp_path, group)
    assert any("expected exactly one" in error for error in errors)
