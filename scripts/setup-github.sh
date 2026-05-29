#!/usr/bin/env bash
#
# Set up GitHub milestones, labels, and issues for the two project repos.
#
# Prereqs:
#   1. Install GitHub CLI (gh). On Ubuntu, prefer the official package:
#        https://github.com/cli/cli/blob/trunk/docs/install_linux.md
#   2. Authenticate:  gh auth login
#
# Usage:   bash scripts/setup-github.sh
#
# Notes:
#   - Labels use --force, so re-running just updates them (idempotent).
#   - Milestones that already exist are skipped (idempotent).
#   - Issues are only created when the repo has zero issues, to avoid
#     duplicates on re-run. Delete/clear issues first if you want a redo.

set -euo pipefail

OWNER="run-qing-spo"
KV="$OWNER/KV-Tiering"
AC="$OWNER/Async-Checkpoint"

# ---- preflight -------------------------------------------------------------
if ! command -v gh >/dev/null 2>&1; then
  echo "ERROR: gh (GitHub CLI) not found. Install it, then run 'gh auth login'." >&2
  echo "  https://github.com/cli/cli/blob/trunk/docs/install_linux.md" >&2
  exit 1
fi
if ! gh auth status >/dev/null 2>&1; then
  echo "ERROR: gh is not authenticated. Run 'gh auth login' first." >&2
  exit 1
fi

# ---- helpers ---------------------------------------------------------------
mk_label() { # repo name color description
  gh label create "$2" --repo "$1" --color "$3" --description "$4" --force >/dev/null \
    && echo "  label  ~ $2"
}

mk_milestone() { # repo title description
  if gh api -X POST "repos/$1/milestones" -f title="$2" -f description="$3" >/dev/null 2>&1; then
    echo "  milestone + $2"
  else
    echo "  milestone = $2 (exists / skipped)"
  fi
}

mk_issue() { # repo title body milestone label...
  local repo="$1" title="$2" body="$3" ms="$4"; shift 4
  local args=(--repo "$repo" --title "$title" --body "$body")
  [ -n "$ms" ] && args+=(--milestone "$ms")
  local l; for l in "$@"; do args+=(--label "$l"); done
  gh issue create "${args[@]}" >/dev/null && echo "  issue  + $title"
}

repo_has_issues() { # repo -> 0 if it already has any issue
  local n
  n=$(gh issue list --repo "$1" --state all -L 1 --json number -q 'length' 2>/dev/null || echo 0)
  [ "${n:-0}" != "0" ]
}

# ===========================================================================
# Repo 1: KV-Tiering  (flagship - read / serving side)
# ===========================================================================
echo "== $KV =="

mk_label "$KV" "type:build"      "1d76db" "Hands-on implementation"
mk_label "$KV" "stretch"         "fbca04" "Optional / nice-to-have"
mk_label "$KV" "phase:warmup"    "c2e0c6" "Warmup phase"
mk_label "$KV" "phase:flagship"  "1d76db" "Flagship main phase"
mk_label "$KV" "phase:advanced"  "5319e7" "Advanced / later phase"

mk_milestone "$KV" "Stage 0 · Baseline & Measurement" "fio + microbench, run SOTA offloading, build TTFT pipeline"
mk_milestone "$KV" "Stage 1 · Trace Characterization"  "Mooncake hash_ids reuse analysis (Fig 1)"
mk_milestone "$KV" "Stage 2 · Tiering Simulator"       "Python 3-tier simulator, pluggable policies (Fig 2/3/4)"
mk_milestone "$KV" "Stage 3 · Real-system Validation"  "vLLM connector / LMCache hooks, calibrate simulator"
mk_milestone "$KV" "Stage 4 · Compaction"              "Cold-block quantization, capacity vs quality (Fig 5)"
mk_milestone "$KV" "Stage 5 · Sensitivity"             "HBM budget sweep (Fig 6) + cross-trace table"

if repo_has_issues "$KV"; then
  echo "  ! $KV already has issues; skipping issue creation."
else
  mk_issue "$KV" "[Stage 0A] Storage-tier microbenchmark (fio + read/O_DIRECT/mmap)" \
    "Compare read() / O_DIRECT / mmap with cold vs warm cache. Output a latency and bandwidth table for DRAM-hit / SSD-sequential / SSD-random to feed the tiering simulator." \
    "Stage 0 · Baseline & Measurement" "type:build" "phase:warmup"

  mk_issue "$KV" "[Stage 0B] Run SOTA offloading and build the TTFT measurement pipeline" \
    "Bring up vLLM --enable-prefix-caching, then vLLM native offloading (0.11+ --kv-offloading-size), then vLLM + LMCache (CPU+Disk). Run a same-prompt-twice TTFT sanity check and build the TTFT measurement harness." \
    "Stage 0 · Baseline & Measurement" "type:build" "phase:flagship"

  mk_issue "$KV" "[Stage 1] Mooncake trace hash_ids reuse analysis -> Fig 1" \
    "Download the Mooncake trace and analyze reuse distance and prefix-hit distribution. Produces Fig 1 and answers RQ1 (why LRU is suboptimal)." \
    "Stage 1 · Trace Characterization" "type:build" "phase:flagship"

  mk_issue "$KV" "[Stage 2] Python 3-tier (HBM/DRAM/SSD) simulator -> Fig 2/3/4" \
    "Trace-driven simulator with pluggable eviction / placement / prefetch policies. Produces Fig 2/3/4 and answers RQ2 (eviction) and RQ3 (placement + prefetch)." \
    "Stage 2 · Tiering Simulator" "type:build" "phase:flagship"

  mk_issue "$KV" "[Stage 3] Implement policy via vLLM connector / LMCache hooks; calibrate simulator" \
    "Pick 2-3 operating points and implement the policy on a real system; calibrate the simulator against measured numbers." \
    "Stage 3 · Real-system Validation" "type:build" "phase:flagship"

  mk_issue "$KV" "[Stage 4] Cold-block quantization: capacity vs quality -> Fig 5" \
    "Quantize cold KV blocks (FP16 -> INT4). Measure the capacity vs quality tradeoff. Produces Fig 5 and answers RQ4." \
    "Stage 4 · Compaction" "type:build" "phase:advanced"

  mk_issue "$KV" "[Stage 5] HBM budget sweep -> Fig 6 + cross-trace summary" \
    "Sweep the HBM budget to produce Fig 6 and a cross-trace summary table. Target finding: the tighter the HBM budget, the larger the gain over LRU." \
    "Stage 5 · Sensitivity" "type:build" "phase:flagship"

  mk_issue "$KV" "[Stretch] GDS / cuFile real-system path" \
    "Optional: explore GPUDirect Storage (cuFile) for the SSD-to-GPU path." \
    "" "type:build" "stretch" "phase:advanced"

  mk_issue "$KV" "Research questions and figures tracker (RQ1-4)" \
    "Umbrella issue linking research questions to figures. RQ1 -> Fig 1 (Stage 1); RQ2/RQ3 -> Fig 2/3/4 (Stage 2); RQ4 -> Fig 5 (Stage 4). See the project overview for details." \
    "" "type:build"
fi

# ===========================================================================
# Repo 2: Async-Checkpoint  (companion - write / training side)
# ===========================================================================
echo "== $AC =="

mk_label "$AC" "type:build" "1d76db" "Hands-on implementation"
mk_label "$AC" "stretch"    "fbca04" "Optional / nice-to-have"

mk_milestone "$AC" "Core (C1-C3)"      "Serialization, durability, sharded async write"
mk_milestone "$AC" "Hardening (C4-C5)" "Checksum integrity, object storage (stretch)"

if repo_has_issues "$AC"; then
  echo "  ! $AC already has issues; skipping issue creation."
else
  mk_issue "$AC" "[C1] safetensors-style serialization + mmap zero-copy lazy load" \
    "Serialize tensors as a JSON header + contiguous blob (safetensors-style). Implement mmap zero-copy lazy load of a single tensor." \
    "Core (C1-C3)" "type:build"

  mk_issue "$AC" "[C2] Durability: tmp file + fsync + atomic rename, with crash-injection recovery" \
    "Write to a temp file, fsync, then atomic rename. Inject crashes (kill mid-write) and verify recovery. Compare behavior with and without fsync." \
    "Core (C1-C3)" "type:build"

  mk_issue "$AC" "[C3] Sharded + async write; overlap CPU serialization with disk IO vs torch.save" \
    "Multi-threaded sharded write with async flush so serialization (CPU) overlaps disk IO. Compare wall-clock against torch.save." \
    "Core (C1-C3)" "type:build"

  mk_issue "$AC" "[C4] Per-shard checksum: corruption detection and skip" \
    "Add a checksum per shard; detect and skip corrupted shards on load." \
    "Hardening (C4-C5)" "type:build"

  mk_issue "$AC" "[C5] (stretch) Write to object storage (MinIO/S3)" \
    "Optional: write checkpoints to object storage (MinIO/S3) to touch distributed storage." \
    "Hardening (C4-C5)" "type:build" "stretch"
fi

echo "Done."
