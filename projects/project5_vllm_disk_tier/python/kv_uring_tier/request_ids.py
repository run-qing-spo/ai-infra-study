"""Stable benchmark request IDs and vLLM ID extraction helpers.

vLLM 0.24.0 keeps two request IDs internally:

* the OpenAI-serving ID (for example ``cmpl-<id>-0``), and
* an EngineCore ID with an extra random suffix.

The benchmark therefore embeds a stable, recognisable ID in the caller supplied
ID and preserves the native vLLM IDs as foreign keys.  Do not strip vLLM IDs and
pretend they are identical; the explicit mapping is part of the measurement
audit trail.
"""

from __future__ import annotations

import re
import uuid
from typing import Optional


_REQ_ID_RE = re.compile(r"kvt-[0-9a-f]{32}")
_REQ_ID_NAMESPACE = uuid.UUID("9de9f04d-37d5-4c75-a94f-31d4de2f765a")


def make_req_id(run_id: str, logical_id: str) -> str:
    """Return a deterministic request ID for one execution in one run.

    ``logical_id`` describes the workload position (phase/round/index).  The
    run ID is included so repeated serve lifecycles cannot collide.
    """
    if not run_id:
        raise ValueError("run_id must be non-empty")
    if not logical_id:
        raise ValueError("logical_id must be non-empty")
    value = uuid.uuid5(_REQ_ID_NAMESPACE, run_id + "\0" + logical_id)
    return "kvt-" + value.hex


def extract_req_id(value: object) -> Optional[str]:
    """Extract our stable ID from a client, OpenAI, or EngineCore ID."""
    if not isinstance(value, str):
        return None
    match = _REQ_ID_RE.search(value)
    return match.group(0) if match else None

