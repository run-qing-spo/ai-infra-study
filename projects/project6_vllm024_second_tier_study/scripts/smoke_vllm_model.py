#!/usr/bin/env python3
"""Run a minimal, machine-readable vLLM 0.24.0 model smoke test."""

from __future__ import annotations

import argparse
import json
import os
import time
from importlib.metadata import version as distribution_version


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--model",
        default="Qwen/Qwen2.5-0.5B-Instruct",
        help="Public Hugging Face model ID or local model path.",
    )
    parser.add_argument("--max-model-len", type=int, default=512)
    parser.add_argument("--max-tokens", type=int, default=8)
    parser.add_argument("--gpu-memory-utilization", type=float, default=0.5)
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    import torch
    import vllm
    from vllm import LLM, SamplingParams

    report: dict[str, object] = {
        "schema_version": 1,
        "model": args.model,
        "vllm_module_version": vllm.__version__,
        "vllm_distribution_version": distribution_version("vllm"),
        "torch_version": torch.__version__,
        "torch_cuda": torch.version.cuda,
        "gpu": torch.cuda.get_device_name(0),
        "vllm_use_flashinfer_sampler": os.environ.get(
            "VLLM_USE_FLASHINFER_SAMPLER"
        ),
        "status": "FAIL",
    }

    started = time.monotonic()
    llm = LLM(
        model=args.model,
        dtype="float16",
        enforce_eager=True,
        gpu_memory_utilization=args.gpu_memory_utilization,
        max_model_len=args.max_model_len,
        max_num_seqs=1,
        trust_remote_code=False,
    )
    ready = time.monotonic()

    outputs = llm.generate(
        ["Reply with exactly one short greeting."],
        SamplingParams(temperature=0.0, max_tokens=args.max_tokens),
    )
    finished = time.monotonic()

    generated = outputs[0].outputs[0]
    report.update(
        {
            "engine_init_seconds": ready - started,
            "generation_seconds": finished - ready,
            "generated_text": generated.text,
            "generated_token_ids": generated.token_ids,
            "n_generated_tokens": len(generated.token_ids),
            "status": "PASS" if generated.token_ids else "FAIL",
        }
    )
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if generated.token_ids else 1


if __name__ == "__main__":
    raise SystemExit(main())
