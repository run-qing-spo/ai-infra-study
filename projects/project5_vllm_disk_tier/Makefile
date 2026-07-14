# project5 — io_uring KV tier 引擎的 pybind 扩展。
#
# 只在 Linux 上编(io_uring + O_DIRECT)。Mac 上单测用 FakeEngine
# (test/test_manager.py), 不需要 .so。
#
# 依赖: liburing-dev, pybind11 (pip install pybind11)
#   apt install -y liburing-dev   # Ubuntu 22.04 自带 liburing 2.1, 够用

PY       ?= python3
UNAME_S  := $(shell uname -s)

EXT      := $(shell $(PY)-config --extension-suffix)
MODULE   := python/kv_uring_tier/_kvtier$(EXT)

CXX      ?= g++
CXXFLAGS := -O2 -g -std=c++20 -fPIC -Wall -Wextra
INCLUDES := -Icsrc \
            -I../project2_spsc/include \
            -I../project3_async_IO/include \
            $(shell $(PY) -m pybind11 --includes)

SRCS := csrc/pybind.cpp \
        csrc/kv_tier_engine.cpp \
        csrc/pool_tier_engine.cpp \
        ../project3_async_IO/src/uring_backend.cpp

.PHONY: all clean check

all: check $(MODULE)

check:
ifneq ($(UNAME_S),Linux)
	$(error "io_uring 引擎只能在 Linux 上编;Mac 上跑 make test 用 FakeEngine")
endif

$(MODULE): $(SRCS) csrc/kv_tier_engine.hpp csrc/pool_tier_engine.hpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -shared $(SRCS) -o $@ -luring

# 元数据面微基准(E1 冷组归因假说的因子分离实验, 见 bench/meta_probe.cpp)。
# 独立程序, 不依赖 liburing / pybind —— 它量的是 open() 路径, 不是 IO 引擎。
# 跑法需要 root: sudo bash bench/run_meta_probe.sh <数据目录>
meta_probe: check bench/meta_probe.cpp
	$(CXX) $(CXXFLAGS) bench/meta_probe.cpp -o meta_probe

# 单测跑的是策略层(manager 账本逻辑), 用 FakeEngine, 任何平台可跑
test:
	$(PY) -m pytest test/ -v

clean:
	rm -f python/kv_uring_tier/_kvtier*.so meta_probe
