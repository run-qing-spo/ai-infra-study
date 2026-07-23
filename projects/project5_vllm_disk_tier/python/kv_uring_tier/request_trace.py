"""Record the vLLM external-request-ID to EngineCore-ID mapping.

This is deliberately installed through the existing vLLM general plugin.  It
does not alter ID generation: vLLM still owns both native IDs, while the trace
records how they relate to the stable ``req_id`` embedded by the benchmark.
"""

from __future__ import annotations

import json
import logging
import os
import threading
import time
from typing import Any, Callable

from .request_ids import extract_req_id


logger = logging.getLogger(__name__)
_write_lock = threading.Lock()


def _mapping_row(request: Any, run_id: str) -> dict[str, Any] | None:
    external_id = getattr(request, "external_req_id", None)
    internal_id = getattr(request, "request_id", None)
    req_id = extract_req_id(external_id) or extract_req_id(internal_id)
    if req_id is None:
        return None  # Not one of this benchmark's managed requests.
    return {
        "schema_version": 1,
        "record_type": "request_id_map",
        "run_id": run_id,
        "req_id": req_id,
        "vllm_external_req_id": external_id,
        "vllm_internal_req_id": internal_id,
        "t_mono_ns": time.monotonic_ns(),
        "t_wall_ns": time.time_ns(),
        "pid": os.getpid(),
    }


def _make_writer(path: str) -> Callable[[dict[str, Any]], None]:
    # O_APPEND is important if a deployment has more than one API process.
    # One compact JSON row is emitted by one write() call under the process lock.
    fp = open(path, "a", buffering=1)

    def write_row(row: dict[str, Any]) -> None:
        line = json.dumps(row, separators=(",", ":"), sort_keys=True)
        with _write_lock:
            fp.write(line + "\n")

    return write_row


def maybe_install() -> None:
    """Install the mapping probe when ``KVTIER_REQ_MAP`` is configured."""
    path = os.environ.get("KVTIER_REQ_MAP")
    if not path:
        return

    try:
        from vllm.v1.engine.input_processor import InputProcessor
    except ImportError:
        return

    if getattr(InputProcessor, "_kvtier_req_map_wrapped", False):
        return

    run_id = os.environ.get("KVTIER_RUN_ID", "")
    if not run_id:
        logger.warning("request_trace: KVTIER_RUN_ID is empty; trace joins will fail")
    write_row = _make_writer(path)
    original = InputProcessor.assign_request_id

    def wrapped(request: Any) -> None:
        original(request)
        row = _mapping_row(request, run_id)
        if row is not None:
            write_row(row)

    # vLLM 0.24.0 defines this as @staticmethod.  Preserve that calling
    # convention so instances do not inject ``self`` into the wrapper.
    InputProcessor.assign_request_id = staticmethod(wrapped)
    InputProcessor._kvtier_req_map_wrapped = True
    logger.info("request_trace: request ID mapping enabled -> %s", path)

