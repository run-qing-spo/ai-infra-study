# Project 4: two-tier cache (P4 阶段先只构 DRAM 一层)
# 风格抄 P3 的 Makefile,obj 全丢到 build/obj/ 下避免目录撞文件名。

CXX      ?= g++
CXXSTD   := -std=c++17
WARN     := -Wall -Wextra -Wpedantic
OPT      ?= -O2
INC      := -Iinclude
CXXFLAGS := $(CXXSTD) $(WARN) $(OPT) $(INC)
LDFLAGS  :=

BUILD    := build
OBJDIR   := $(BUILD)/obj

SRCS     := src/dram_block_store.cpp src/lru_policy.cpp src/cache.cpp
OBJS     := $(SRCS:%.cpp=$(OBJDIR)/%.o)

SMOKE    := bench/smoke.cpp
SMOKE_O  := $(OBJDIR)/bench/smoke.o
SMOKE_BIN := $(BUILD)/smoke

.PHONY: all clean smoke

all: $(SMOKE_BIN)

smoke: $(SMOKE_BIN)
	./$(SMOKE_BIN)

$(SMOKE_BIN): $(OBJS) $(SMOKE_O)
	$(CXX) $(LDFLAGS) -o $@ $^

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)
