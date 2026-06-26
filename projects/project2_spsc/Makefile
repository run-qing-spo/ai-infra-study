CXX            := g++
# 测试构建:带 debug 符号 + audit hooks,不开 -O 避免优化掩盖并发问题
CXXFLAGS       := -std=c++17 -Wall -Wextra -g -Iinclude -DSPSC_TEST_HOOKS -pthread
# bench 构建:关 audit hooks + 开 -O2 测真实性能
BENCH_CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -Iinclude -pthread

GTEST_CFLAGS := $(shell pkg-config --cflags gtest 2>/dev/null)
GTEST_LIBS   := $(shell pkg-config --libs gtest_main 2>/dev/null || echo -lgtest -lgtest_main -pthread)

TEST_BIN      := build/test_bin
TEST_TSAN_BIN := build/test_tsan_bin
BENCH_BIN     := build/bench_bin

# 死锁硬超时:macOS 默认没装 GNU timeout,用 perl 的 alarm 兜底。
# alarm 设的 SIGALRM 在 exec 之后依旧有效,到点把进程直接打死。
# 防御场景:被测代码真死锁时,std::async + wait_for 救不了进程
# (~future 还会阻塞在 join),只能从外层强杀。
TEST_TIMEOUT  := 60
TSAN_TIMEOUT  := 180
TIMEOUT_RUN    = perl -e 'alarm shift @ARGV; exec @ARGV'

# 收集 test/ 下所有 test_*.cpp 作为测试源文件; bench 单独一个文件
TEST_SRC  := $(wildcard test/test_*.cpp)
BENCH_SRC := test/bench.cpp
HEADERS   := $(wildcard include/*.hpp)

.PHONY: all test test-tsan bench compdb clean

all: $(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(HEADERS) | build
	$(CXX) $(CXXFLAGS) $(GTEST_CFLAGS) $(TEST_SRC) -o $@ $(GTEST_LIBS)

build:
	mkdir -p $@

test: $(TEST_BIN)
	$(TIMEOUT_RUN) $(TEST_TIMEOUT) ./$(TEST_BIN)

$(TEST_TSAN_BIN): $(TEST_SRC) $(HEADERS) | build
	$(CXX) $(CXXFLAGS) $(GTEST_CFLAGS) -fsanitize=thread $(TEST_SRC) -o $@ $(GTEST_LIBS)

test-tsan: $(TEST_TSAN_BIN)
	$(TIMEOUT_RUN) $(TSAN_TIMEOUT) ./$(TEST_TSAN_BIN)

$(BENCH_BIN): $(BENCH_SRC) $(HEADERS) | build
	$(CXX) $(BENCH_CXXFLAGS) $(BENCH_SRC) -o $@

bench: $(BENCH_BIN)
	./$(BENCH_BIN)

# 重新生成 compile_commands.json,给 clangd / IDE 用
# 需要 pip install compiledb
# bench.cpp 不存在时只生成 test / test-tsan 的, 不让 compdb 卡死在缺文件
ifneq ($(wildcard $(BENCH_SRC)),)
COMPDB_TARGETS := all $(TEST_TSAN_BIN) $(BENCH_BIN)
else
COMPDB_TARGETS := all $(TEST_TSAN_BIN)
endif

compdb:
	$(MAKE) clean
	compiledb make $(COMPDB_TARGETS)

clean:
	rm -f $(TEST_BIN) $(TEST_TSAN_BIN) $(BENCH_BIN)
