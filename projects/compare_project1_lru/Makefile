CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -g -Iinclude

TEST_BIN := test/test_bin
TEST_SRC := test/test.cpp
HEADERS  := include/lru_base.hpp test/util.hpp

.PHONY: all test clean compdb

all: $(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(TEST_SRC) -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

compdb:
	$(MAKE) clean
	bear -- $(MAKE) all

clean:
	rm -f $(TEST_BIN)
