from __future__ import annotations

import unittest
from dataclasses import dataclass

from uring_slab_tier.instrumentation import ContractViolation, InstrumentedTier


@dataclass
class FakeJob:
    job_id: int
    keys: list[bytes]
    block_ids: list[int]
    is_promotion: bool


@dataclass
class FakeResult:
    job_id: int
    success: bool


class FakeTier:
    tier_type = "fake"

    def __init__(self) -> None:
        self.finished: list[FakeResult] = []

    def submit_store(self, job: FakeJob) -> None:
        self.finished.append(FakeResult(job.job_id, True))

    def submit_load(self, job: FakeJob) -> None:
        self.finished.append(FakeResult(job.job_id, True))

    def get_finished_jobs(self) -> list[FakeResult]:
        result = self.finished
        self.finished = []
        return result

    def lookup(self, key: bytes, req_context: object) -> bool:
        return key == b"present"

    def on_new_request(self, req_context: object) -> object:
        return object()

    def on_request_finished(self, req_context: object) -> None:
        return None

    def on_schedule_end(self) -> None:
        return None

    def touch(self, keys: list[bytes], req_context: object) -> None:
        return None

    def has_pending_work(self) -> bool:
        return False

    def drain_jobs(self) -> None:
        return None

    def shutdown(self) -> None:
        return None


class InstrumentedTierTest(unittest.TestCase):
    def test_store_and_load_are_accounted_once(self) -> None:
        clock = iter(range(1, 100)).__next__
        tier = InstrumentedTier(
            FakeTier(),
            run_id="run",
            block_bytes=4096,
            clock_ns=clock,
            wall_clock_ns=clock,
        )
        tier.submit_store(FakeJob(1, [b"a", b"b"], [0, 1], False))
        tier.submit_load(FakeJob(2, [b"a"], [2], True))
        results = tier.get_finished_jobs()
        self.assertEqual([result.job_id for result in results], [1, 2])
        self.assertEqual(tier.pending_job_count, 0)
        self.assertEqual(len(tier.jobs_for_direction("store")), 1)
        self.assertEqual(len(tier.jobs_for_direction("load")), 1)
        tier.assert_job_accounting()
        tier.shutdown()

    def test_rejects_empty_and_mismatched_jobs(self) -> None:
        tier = InstrumentedTier(FakeTier(), run_id="run", block_bytes=4096)
        with self.assertRaisesRegex(ContractViolation, "empty"):
            tier.submit_store(FakeJob(1, [], [], False))
        with self.assertRaisesRegex(ContractViolation, "keys"):
            tier.submit_store(FakeJob(2, [b"a"], [], False))

    def test_rejects_wrong_direction_and_duplicate_id(self) -> None:
        tier = InstrumentedTier(FakeTier(), run_id="run", block_bytes=4096)
        with self.assertRaisesRegex(ContractViolation, "is_promotion"):
            tier.submit_store(FakeJob(1, [b"a"], [0], True))
        tier.submit_store(FakeJob(2, [b"a"], [0], False))
        with self.assertRaisesRegex(ContractViolation, "duplicate job_id"):
            tier.submit_load(FakeJob(2, [b"a"], [0], True))

    def test_lookup_records_tristate_result(self) -> None:
        tier = InstrumentedTier(FakeTier(), run_id="run", block_bytes=4096)
        context = type("Context", (), {"req_id": "request"})()
        self.assertTrue(tier.lookup(b"present", context))
        self.assertTrue(tier.lookup(b"present", context))
        self.assertFalse(tier.lookup(b"missing", context))
        events = [event for event in tier.events if event["event"] == "lookup_return"]
        self.assertEqual([event["result"] for event in events], [True, False])
        self.assertEqual(tier.event_stats["suppressed_lookup_retries"], 1)

    def test_event_limit_is_bounded_and_reported(self) -> None:
        tier = InstrumentedTier(
            FakeTier(), run_id="run", block_bytes=4096, max_events=2
        )
        context = type("Context", (), {"req_id": "request"})()
        tier.lookup(b"present", context)
        tier.lookup(b"missing", context)
        self.assertEqual(tier.event_stats["recorded_events"], 2)
        self.assertEqual(tier.event_stats["dropped_events"], 1)


if __name__ == "__main__":
    unittest.main()
