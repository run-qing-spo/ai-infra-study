#!/usr/bin/env python3
"""Send one deterministic prompt to a local vLLM FS-tier smoke server."""

from __future__ import annotations

import argparse
import json
import time
import urllib.request


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--url",
        default="http://127.0.0.1:18080/v1/completions",
    )
    parser.add_argument(
        "--model",
        default="Qwen/Qwen2.5-0.5B-Instruct",
    )
    parser.add_argument("--repetitions", type=int, default=100)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    prompt = (
        "Summarize this deterministic sequence in one word: "
        + "alpha beta gamma delta " * args.repetitions
    )
    payload = {
        "model": args.model,
        "prompt": prompt,
        "max_tokens": 1,
        "temperature": 0,
        "seed": 0,
    }
    request = urllib.request.Request(
        args.url,
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )

    started = time.monotonic()
    with urllib.request.urlopen(request, timeout=120) as response:
        body = json.load(response)
        status_code = response.status
    elapsed = time.monotonic() - started

    output = {
        "schema_version": 1,
        "status": "PASS" if status_code == 200 else "FAIL",
        "http_status": status_code,
        "elapsed_seconds": elapsed,
        "model": args.model,
        "prompt_repetitions": args.repetitions,
        "usage": body.get("usage"),
        "completion_text": body["choices"][0]["text"],
        "request_id": body.get("id"),
    }
    print(json.dumps(output, indent=2, sort_keys=True))
    return 0 if output["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
