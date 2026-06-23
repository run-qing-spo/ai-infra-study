CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -g -Iinclude -DLRU_TEST_HOOKS
GTEST_CFLAGS := $(shell pkg-config --cflags gtest 2>/dev/null)
GTEST_LIBS   := $(shell pkg-config --libs gtest_main 2>/dev/null || echo -lgtest -lgtest_main -pthread)

TEST_BIN      := build/test_bin
TEST_TSAN_BIN := build/test_tsan_bin
TEST_SRC := test/test.cpp test/test_mutex.cpp
HEADERS  := $(wildcard include/*.hpp)

.PHONY: all test test-tsan clean compdb

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
compdb:
	$(MAKE) clean
	compiledb make
# 	bear -- $(MAKE) all

clean:
	rm -f $(TEST_BIN) $(TEST_TSAN_BIN)
