from __future__ import annotations

import hashlib
import time
from collections.abc import Callable, Collection, Iterable
from dataclasses import dataclass
from typing import Any, Protocol


SCHEMA_VERSION = 1
_MISSING = object()


class ContractViolation(RuntimeError):
    """Raised when a backend or caller violates the frozen experiment contract."""


class JobMetadataLike(Protocol):
    job_id: int
    keys: Collection[bytes]
    block_ids: Collection[int]
    is_promotion: bool


class JobResultLike(Protocol):
    job_id: int
    success: bool


@dataclass(frozen=True)
class ObservedJob:
    job_id: int
    direction: str
    n_blocks: int
    n_bytes: int
    submit_enter_ns: int
    submit_return_ns: int
    observed_done_ns: int
    success: bool

    @property
    def submit_call_ns(self) -> int:
        return self.submit_return_ns - self.submit_enter_ns

    @property
    def observed_sojourn_ns(self) -> int:
        return self.observed_done_ns - self.submit_enter_ns


@dataclass
class _PendingJob:
    direction: str
    n_blocks: int
    n_bytes: int
    submit_enter_ns: int
    submit_return_ns: int = 0


class InstrumentedTier:
    """Backend-neutral observer at the SecondaryTierManager boundary.

    The wrapper intentionally measures only call boundaries visible to the
    scheduler. It does not claim to measure device service time.
    """

    def __init__(
        self,
        tier: Any,
        *,
        run_id: str,
        block_bytes: int,
        clock_ns: Callable[[], int] = time.monotonic_ns,
        wall_clock_ns: Callable[[], int] = time.time_ns,
        max_events: int = 200_000,
    ) -> None:
        if block_bytes <= 0:
            raise ValueError("block_bytes must be positive")
        if max_events <= 0:
            raise ValueError("max_events must be positive")
        self._tier = tier
        self.run_id = run_id
        self.block_bytes = block_bytes
        self._clock_ns = clock_ns
        self._wall_clock_ns = wall_clock_ns
        self._max_events = max_events
        self._sequence = 0
        self._dropped_events = 0
        self._pending: dict[int, _PendingJob] = {}
        self._completed: dict[int, ObservedJob] = {}
        self._lookup_last_result: dict[tuple[str | None, bytes], bool | None] = {}
        self._lookup_keys_by_request: dict[str | None, set[bytes]] = {}
        self._suppressed_lookup_retries = 0
        self.events: list[dict[str, Any]] = []
        self._emit("tier_observer_started", pending_jobs=0)

    @property
    def pending_job_count(self) -> int:
        return len(self._pending)

    @property
    def completed_jobs(self) -> tuple[ObservedJob, ...]:
        return tuple(self._completed.values())

    @property
    def event_stats(self) -> dict[str, int]:
        return {
            "recorded_events": len(self.events),
            "dropped_events": self._dropped_events,
            "suppressed_lookup_retries": self._suppressed_lookup_retries,
            "max_events": self._max_events,
        }

    def _emit(self, event: str, **fields: Any) -> None:
        if len(self.events) >= self._max_events:
            self._dropped_events += 1
            return
        self._sequence += 1
        record = {
            "schema_version": SCHEMA_VERSION,
            "run_id": self.run_id,
            "sequence": self._sequence,
            "t_wall_ns": self._wall_clock_ns(),
            "t_mono_ns": self._clock_ns(),
            "event": event,
            "tier": getattr(self._tier, "tier_type", type(self._tier).__name__),
        }
        record.update(fields)
        self.events.append(record)

    @staticmethod
    def _key_fingerprint(key: bytes) -> str:
        return hashlib.sha256(bytes(key)).hexdigest()[:16]

    def lookup(self, key: bytes, req_context: Any) -> bool | None:
        enter_ns = self._clock_ns()
        result = self._tier.lookup(key, req_context)
        return_ns = self._clock_ns()
        request_id = getattr(req_context, "req_id", None)
        raw_key = bytes(key)
        lookup_id = (request_id, raw_key)
        previous = self._lookup_last_result.get(lookup_id, _MISSING)
        if previous is _MISSING or previous is not result:
            self._lookup_last_result[lookup_id] = result
            self._lookup_keys_by_request.setdefault(request_id, set()).add(raw_key)
            self._emit(
                "lookup_return",
                request_id=request_id,
                key_fingerprint=self._key_fingerprint(raw_key),
                result=result,
                call_ns=return_ns - enter_ns,
                pending_jobs=len(self._pending),
            )
        else:
            self._suppressed_lookup_retries += 1
        return result

    def _submit(self, direction: str, job_metadata: JobMetadataLike) -> None:
        job_id = int(job_metadata.job_id)
        if job_id in self._pending or job_id in self._completed:
            raise ContractViolation(f"duplicate job_id {job_id}")

        keys = job_metadata.keys
        block_ids = job_metadata.block_ids
        n_keys = len(keys)
        n_block_ids = len(block_ids)
        if n_keys < 1:
            raise ContractViolation("empty jobs are outside contract v1")
        if n_keys != n_block_ids:
            raise ContractViolation(
                f"job {job_id}: {n_keys} keys != {n_block_ids} block_ids"
            )
        expected_promotion = direction == "load"
        if bool(job_metadata.is_promotion) != expected_promotion:
            raise ContractViolation(
                f"job {job_id}: is_promotion={job_metadata.is_promotion} "
                f"does not match direction={direction}"
            )

        enter_ns = self._clock_ns()
        pending = _PendingJob(
            direction=direction,
            n_blocks=n_keys,
            n_bytes=n_keys * self.block_bytes,
            submit_enter_ns=enter_ns,
        )
        self._pending[job_id] = pending
        self._emit(
            "job_submit_enter",
            direction=direction,
            job_id=job_id,
            n_blocks=n_keys,
            n_bytes=pending.n_bytes,
            pending_jobs=len(self._pending),
        )
        try:
            if direction == "store":
                self._tier.submit_store(job_metadata)
            else:
                self._tier.submit_load(job_metadata)
        except Exception:
            del self._pending[job_id]
            self._emit(
                "job_submit_raised",
                direction=direction,
                job_id=job_id,
                n_blocks=n_keys,
                n_bytes=pending.n_bytes,
                pending_jobs=len(self._pending),
            )
            raise

        pending.submit_return_ns = self._clock_ns()
        self._emit(
            "job_submit_return",
            direction=direction,
            job_id=job_id,
            n_blocks=n_keys,
            n_bytes=pending.n_bytes,
            submit_call_ns=pending.submit_return_ns - enter_ns,
            pending_jobs=len(self._pending),
        )

    def submit_store(self, job_metadata: JobMetadataLike) -> None:
        self._submit("store", job_metadata)

    def submit_load(self, job_metadata: JobMetadataLike) -> None:
        self._submit("load", job_metadata)

    def get_finished_jobs(self) -> list[JobResultLike]:
        poll_enter_ns = self._clock_ns()
        results = list(self._tier.get_finished_jobs())
        observed_ns = self._clock_ns()
        for result in results:
            job_id = int(result.job_id)
            pending = self._pending.pop(job_id, None)
            if pending is None:
                if job_id in self._completed:
                    raise ContractViolation(f"duplicate completion for job {job_id}")
                raise ContractViolation(f"completion for unknown job {job_id}")
            observed = ObservedJob(
                job_id=job_id,
                direction=pending.direction,
                n_blocks=pending.n_blocks,
                n_bytes=pending.n_bytes,
                submit_enter_ns=pending.submit_enter_ns,
                submit_return_ns=pending.submit_return_ns,
                observed_done_ns=observed_ns,
                success=bool(result.success),
            )
            self._completed[job_id] = observed
            self._emit(
                "job_observed_done",
                direction=observed.direction,
                job_id=job_id,
                n_blocks=observed.n_blocks,
                n_bytes=observed.n_bytes,
                success=observed.success,
                submit_call_ns=observed.submit_call_ns,
                tier_job_observed_sojourn_ns=observed.observed_sojourn_ns,
                pending_jobs=len(self._pending),
            )
        self._emit(
            "completion_poll",
            poll_call_ns=observed_ns - poll_enter_ns,
            n_results=len(results),
            pending_jobs=len(self._pending),
        )
        return results

    def on_new_request(self, req_context: Any) -> Any:
        return self._tier.on_new_request(req_context)

    def on_request_finished(self, req_context: Any) -> None:
        self._tier.on_request_finished(req_context)
        request_id = getattr(req_context, "req_id", None)
        for raw_key in self._lookup_keys_by_request.pop(request_id, ()):
            self._lookup_last_result.pop((request_id, raw_key), None)

    def on_schedule_end(self) -> None:
        self._tier.on_schedule_end()
        self._emit("schedule_end", pending_jobs=len(self._pending))

    def touch(self, keys: Collection[bytes], req_context: Any) -> None:
        self._tier.touch(keys, req_context)

    def has_pending_work(self) -> bool:
        return bool(self._tier.has_pending_work())

    def drain_jobs(self) -> None:
        enter_ns = self._clock_ns()
        self._tier.drain_jobs()
        self._emit(
            "drain_return",
            call_ns=self._clock_ns() - enter_ns,
            pending_jobs=len(self._pending),
        )

    def shutdown(self) -> None:
        if self._pending:
            raise ContractViolation(
                f"shutdown attempted with {len(self._pending)} pending jobs"
            )
        enter_ns = self._clock_ns()
        self._tier.shutdown()
        self._emit(
            "shutdown_return",
            call_ns=self._clock_ns() - enter_ns,
            pending_jobs=0,
            suppressed_lookup_retries=self._suppressed_lookup_retries,
            dropped_events=self._dropped_events,
        )

    def jobs_for_direction(self, direction: str) -> tuple[ObservedJob, ...]:
        return tuple(
            job for job in self._completed.values() if job.direction == direction
        )

    def assert_job_accounting(self) -> None:
        if self._pending:
            raise ContractViolation(
                f"{len(self._pending)} submitted jobs lack completion"
            )

    def __getattr__(self, name: str) -> Any:
        return getattr(self._tier, name)
