# 项目 1：Thread-safe LRU Cache

详细设计文档：

- 本地路径：[docs/projects/project1_lru/README.md](../../docs/projects/project1_lru/README.md)
- 在线阅读：[项目 1 文档](https://run-qing-spo.github.io/ai-infra-study/projects/project1_lru/README.html)

---

## 目录结构

```
projects/project1_lru/
├── include/lru/
│   ├── lrucache_base.hpp       # 非线程安全的 LRU 核心数据结构
│   ├── lrucache.hpp            # V1: 全局 mutex 版本
│   └── sharded_lrucache.hpp    # V2: 64-shard 分片版本
├── tests/
│   ├── test_helpers.hpp        # 最小测试框架
│   ├── test_lru_basic.cpp      # V1 单线程功能测试
│   ├── test_sharded_basic.cpp  # V2 单线程功能测试
│   └── test_lru_concurrent.cpp # V1+V2 并发测试（含 TSan 验证）
├── benchmarks/
│   ├── bench_helpers.hpp       # 计时器、延迟统计工具
│   └── bench_throughput.cpp    # 多线程吞吐 + 延迟基准测试
├── scripts/
│   ├── build.sh                # 构建脚本
│   ├── run_tests.sh            # 运行全部测试 + TSan
│   └── run_bench.sh            # 运行基准测试 → PERF_RESULTS.md
├── build/                      # 编译产物（.gitignore）
├── PERF_RESULTS.md             # 基准测试结果
└── README.md                   # 本文件
```

## 设计决策

### 三层分离：Base → V1 → V2

- **`LRUCacheBase<K,V>`**：纯数据结构（hash map + doubly linked list），无任何锁
- **`LRUCache<K,V>`（V1）**：组合 `LRUCacheBase` + `std::mutex`，每个方法加 `lock_guard`
- **`ShardedLRUCache<K,V>`（V2）**：64 个 `Shard`，每个 Shard = `LRUCacheBase` + `std::mutex`，`alignas(64)` 避免伪共享

这样避免了 V2 中双重加锁的问题（shard mutex + 全局 mutex）。

### Header-only 模板

LRUCache 是模板类，必须在实例化时可见定义。Header-only 还简化了构建——每个测试/基准只需一个 `g++` 命令。

### 自定义测试/基准框架

- `test_helpers.hpp`：~100 行，提供 `TEST`/`EXPECT_EQ`/`ASSERT_EQ` 宏（命名对齐 gtest，便于将来迁移）
- `bench_helpers.hpp`：~130 行，提供 `ScopedTimer` + `LatencyCollector` + markdown 表格输出
- 原因：googletest 和 google benchmark 未安装；学习项目自建更透明

### 为什么不用 `shared_mutex`？

`get()` 需要修改链表（把节点移到头部），`shared_mutex` 的读锁不允许修改数据结构，`get()` 必须升级为写锁，反而比 `std::mutex` 更慢。详见设计文档。

## 前置条件

- **g++ 11+**（支持 C++17）
- **bash**（用于构建脚本）

## 构建与运行

```bash
cd projects/project1_lru

# 构建全部
bash scripts/build.sh all

# 仅构建测试
bash scripts/build.sh test_all

# 仅构建基准测试
bash scripts/build.sh bench

# 清理
bash scripts/build.sh clean
```

### 运行测试

```bash
bash scripts/run_tests.sh
```

这会运行所有功能测试、并发测试和 ThreadSanitizer 验证。

### 运行基准测试

```bash
bash scripts/run_bench.sh
```

结果会输出到终端并保存到 `PERF_RESULTS.md`。

## 性能结果

见 [PERF_RESULTS.md](PERF_RESULTS.md)。
