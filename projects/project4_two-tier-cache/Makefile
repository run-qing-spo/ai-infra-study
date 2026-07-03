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

SRCS     := src/dram_block_store.cpp src/ssd_block_store.cpp src/lru_policy.cpp src/cache.cpp src/tiered_cache.cpp
OBJS     := $(SRCS:%.cpp=$(OBJDIR)/%.o)

SMOKE    := bench/smoke.cpp
SMOKE_O  := $(OBJDIR)/bench/smoke.o
SMOKE_BIN := $(BUILD)/smoke

SMOKE_SSD    := bench/smoke_ssd.cpp
SMOKE_SSD_O  := $(OBJDIR)/bench/smoke_ssd.o
SMOKE_SSD_BIN := $(BUILD)/smoke_ssd

SMOKE_TIERED    := bench/smoke_tiered.cpp
SMOKE_TIERED_O  := $(OBJDIR)/bench/smoke_tiered.o
SMOKE_TIERED_BIN := $(BUILD)/smoke_tiered

.PHONY: all clean smoke smoke_ssd smoke_tiered

all: $(SMOKE_BIN) $(SMOKE_SSD_BIN) $(SMOKE_TIERED_BIN)

smoke: $(SMOKE_BIN)
	./$(SMOKE_BIN)

smoke_ssd: $(SMOKE_SSD_BIN)
	./$(SMOKE_SSD_BIN)

smoke_tiered: $(SMOKE_TIERED_BIN)
	./$(SMOKE_TIERED_BIN)

$(SMOKE_BIN): $(OBJS) $(SMOKE_O)
	$(CXX) $(LDFLAGS) -o $@ $^

$(SMOKE_SSD_BIN): $(OBJS) $(SMOKE_SSD_O)
	$(CXX) $(LDFLAGS) -o $@ $^

$(SMOKE_TIERED_BIN): $(OBJS) $(SMOKE_TIERED_O)
	$(CXX) $(LDFLAGS) -o $@ $^

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)
