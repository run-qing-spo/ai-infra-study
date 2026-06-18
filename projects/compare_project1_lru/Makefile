CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -g -Iinclude
GTEST_CFLAGS := $(shell pkg-config --cflags gtest 2>/dev/null)
GTEST_LIBS   := $(shell pkg-config --libs gtest_main 2>/dev/null || echo -lgtest -lgtest_main -pthread)

TEST_BIN := build/test_bin
TEST_SRC := test/test.cpp
HEADERS  := include/lru_base.hpp

.PHONY: all test clean compdb

all: $(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(GTEST_CFLAGS) $(TEST_SRC) -o $@ $(GTEST_LIBS)

test: $(TEST_BIN)
	./$(TEST_BIN)

compdb:
	$(MAKE) clean
	bear -- $(MAKE) all

clean:
	rm -f $(TEST_BIN)
