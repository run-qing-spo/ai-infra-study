// test_main.cpp — single `main` for every test binary.
// Linked into each test executable by scripts/build.sh.

#define TEST_HELPERS_NO_MAIN
#include "test_helpers.hpp"

int main() { return run_all_tests(); }
