#!/usr/bin/env bash
# build.sh — Build script for Thread-safe LRU Cache project.
# Usage: bash scripts/build.sh <target>
# Targets: test_basic | test_concurrent | test_tsan | bench | test_all | all | clean

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
BUILD="$ROOT/build"
SRC_TESTS="$ROOT/tests"
SRC_BENCH="$ROOT/benchmarks"
INCLUDE="-I$ROOT/include"

CXX="${CXX:-g++}"
COMMON_FLAGS="-std=c++17 -Wall -Wextra -pthread"
OPT_FLAGS="-O2"
TSAN_FLAGS="-O1 -g -fsanitize=thread"

mkdir -p "$BUILD"

TEST_MAIN="$SRC_TESTS/test_main.cpp"

build_test_basic() {
    echo "[BUILD] test_lru_basic"
    "$CXX" $COMMON_FLAGS $OPT_FLAGS $INCLUDE "$SRC_TESTS/test_lru_basic.cpp" "$TEST_MAIN" -o "$BUILD/test_lru_basic"
    echo "[BUILD] test_sharded_basic"
    "$CXX" $COMMON_FLAGS $OPT_FLAGS $INCLUDE "$SRC_TESTS/test_sharded_basic.cpp" "$TEST_MAIN" -o "$BUILD/test_sharded_basic"
}

build_test_concurrent() {
    echo "[BUILD] test_lru_concurrent"
    "$CXX" $COMMON_FLAGS $OPT_FLAGS $INCLUDE "$SRC_TESTS/test_lru_concurrent.cpp" "$TEST_MAIN" -o "$BUILD/test_lru_concurrent"
}

build_test_tsan() {
    echo "[BUILD] test_lru_concurrent_tsan (TSan)"
    "$CXX" $COMMON_FLAGS $TSAN_FLAGS $INCLUDE "$SRC_TESTS/test_lru_concurrent.cpp" "$TEST_MAIN" -o "$BUILD/test_lru_concurrent_tsan"
}

build_bench() {
    echo "[BUILD] bench_throughput"
    "$CXX" $COMMON_FLAGS $OPT_FLAGS $INCLUDE "$SRC_BENCH/bench_throughput.cpp" -o "$BUILD/bench_throughput"
}

do_test_all() {
    build_test_basic
    build_test_concurrent
    build_test_tsan
}

do_all() {
    do_test_all
    build_bench
}

do_clean() {
    echo "[CLEAN] Removing $BUILD"
    rm -rf "$BUILD"
}

case "${1:-}" in
    test_basic)     build_test_basic ;;
    test_concurrent) build_test_concurrent ;;
    test_tsan)      build_test_tsan ;;
    bench)          build_bench ;;
    test_all)       do_test_all ;;
    all)            do_all ;;
    clean)          do_clean ;;
    *)
        echo "Usage: bash scripts/build.sh <target>"
        echo "Targets: test_basic | test_concurrent | test_tsan | bench | test_all | all | clean"
        exit 1
        ;;
esac
