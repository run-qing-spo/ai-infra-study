#!/usr/bin/env bash
set -euo pipefail

EXPERIMENT_ROOT="${EXPERIMENT_ROOT:-/root/uring-slab-experiments}"
VLLM_VENV="${VLLM_VENV:-${EXPERIMENT_ROOT}/venvs/vllm024-cu129-clean}"
MODEL="${MODEL:-Qwen/Qwen2.5-0.5B-Instruct}"
PORT="${PORT:-18080}"
FS_ROOT="${FS_ROOT:-${EXPERIMENT_ROOT}/data/vllm-fs-smoke}"

mkdir -p "${FS_ROOT}"

KV_TRANSFER_CONFIG="$(
  printf '%s' \
    '{"kv_connector":"OffloadingConnector","kv_role":"kv_both",' \
    '"kv_connector_extra_config":{' \
    '"spec_name":"TieringOffloadingSpec",' \
    '"cpu_bytes_to_use":268435456,' \
    '"block_size":16,' \
    '"eviction_policy":"lru",' \
    '"offload_prompt_only":true,' \
    '"secondary_tiers":[{' \
    '"type":"fs",' \
    "\"root_dir\":\"${FS_ROOT}\"" \
    '}]}}'
)"

export CUDA_VISIBLE_DEVICES="${CUDA_VISIBLE_DEVICES:-0}"
export HF_HOME="${HF_HOME:-${EXPERIMENT_ROOT}/cache/huggingface}"
export HF_HUB_DISABLE_XET="${HF_HUB_DISABLE_XET:-1}"
export PYTHONHASHSEED="${PYTHONHASHSEED:-0}"
export VLLM_USE_FLASHINFER_SAMPLER="${VLLM_USE_FLASHINFER_SAMPLER:-0}"
export VLLM_WORKER_MULTIPROC_METHOD="${VLLM_WORKER_MULTIPROC_METHOD:-spawn}"

exec "${VLLM_VENV}/bin/vllm" serve "${MODEL}" \
  --host 127.0.0.1 \
  --port "${PORT}" \
  --dtype float16 \
  --max-model-len 512 \
  --max-num-seqs 4 \
  --gpu-memory-utilization 0.5 \
  --enforce-eager \
  --kv-transfer-config "${KV_TRANSFER_CONFIG}"
