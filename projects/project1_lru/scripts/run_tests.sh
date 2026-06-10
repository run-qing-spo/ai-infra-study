#!/usr/bin/env bash
# run_tests.sh — Run all tests including TSan validation.
# Usage: bash scripts/run_tests.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
BUILD="$ROOT/build"

# Build first
bash "$SCRIPT_DIR/build.sh" test_all

failed=0

run_test() {
    local name="$1"
    local binary="$BUILD/$name"
    echo ""
    echo "===== Running $name ====="
    if "$binary"; then
        echo "===== $name PASSED ====="
    else
        echo "===== $name FAILED ====="
        failed=1
    fi
}

run_test test_lru_basic
run_test test_sharded_basic
run_test test_lru_concurrent
run_test test_lru_concurrent_tsan

echo ""
if [ $failed -eq 0 ]; then
    echo "All tests passed!"
else
    echo "Some tests FAILED!"
    exit 1
fi
