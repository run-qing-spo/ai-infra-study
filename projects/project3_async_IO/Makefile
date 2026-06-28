# Project 3: async IO storage backend
# 平台:Linux(io_uring 内核 API + liburing 用户态库)
# 依赖:liburing-dev (Ubuntu: sudo apt install liburing-dev)

CXX      ?= g++
CXXSTD   := -std=c++17
WARN     := -Wall -Wextra -Wpedantic
OPT      ?= -O2
INC      := -Iinclude
CXXFLAGS := $(CXXSTD) $(WARN) $(OPT) $(INC) -pthread
LDFLAGS  := -pthread
LDLIBS   := -luring

BUILD    := build
SRCS     := src/sync_backend.cpp src/uring_backend.cpp
OBJS     := $(SRCS:%.cpp=$(BUILD)/%.o)
BENCH    := bench/bench.cpp
BENCH_O  := $(BUILD)/bench/bench.o
TARGET   := $(BUILD)/bench

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS) $(BENCH_O)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)
