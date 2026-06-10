#!/usr/bin/env bash
# run_bench.sh — Run benchmark and save results to PERF_RESULTS.md.
# Usage: bash scripts/run_bench.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
BUILD="$ROOT/build"

# Build benchmark binary
bash "$SCRIPT_DIR/build.sh" bench

echo "===== Running benchmark ====="
"$BUILD/bench_throughput" 2>&1 | tee "$ROOT/PERF_RESULTS.md"
echo "===== Benchmark complete. Results saved to PERF_RESULTS.md ====="
