CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -g -Iinclude -DLRU_TEST_HOOKS
BENCH_CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -Iinclude
GTEST_CFLAGS := $(shell pkg-config --cflags gtest 2>/dev/null)
GTEST_LIBS   := $(shell pkg-config --libs gtest_main 2>/dev/null || echo -lgtest -lgtest_main -pthread)

TEST_BIN      := build/test_bin
TEST_TSAN_BIN := build/test_tsan_bin
BENCH_BIN     := build/bench_bin
TEST_SRC := test/test.cpp test/test_mutex.cpp
BENCH_SRC := test/bench.cpp
HEADERS  := $(wildcard include/*.hpp)

.PHONY: all test test-tsan bench clean compdb

all: $(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(HEADERS) | build
	$(CXX) $(CXXFLAGS) $(GTEST_CFLAGS) $(TEST_SRC) -o $@ $(GTEST_LIBS)

build:
	mkdir -p $@

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_TSAN_BIN): $(TEST_SRC) $(HEADERS) | build
	$(CXX) $(CXXFLAGS) $(GTEST_CFLAGS) -fsanitize=thread $(TEST_SRC) -o $@ $(GTEST_LIBS)

test-tsan: $(TEST_TSAN_BIN)
	./$(TEST_TSAN_BIN)

$(BENCH_BIN): $(BENCH_SRC) $(HEADERS) | build
	$(CXX) $(BENCH_CXXFLAGS) $(BENCH_SRC) -o $@ -pthread

bench: $(BENCH_BIN)
	./$(BENCH_BIN)

compdb:
	$(MAKE) clean
	compiledb make all $(TEST_TSAN_BIN) $(BENCH_BIN)
# 	bear -- $(MAKE) all

clean:
	rm -f $(TEST_BIN) $(TEST_TSAN_BIN) $(BENCH_BIN)
