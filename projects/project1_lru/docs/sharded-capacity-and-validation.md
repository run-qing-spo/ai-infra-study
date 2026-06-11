# Tradeoff #3：ShardedLRUCache 容量契约与参数校验

**状态**：已采纳  
**涉及文件**：`include/lru/sharded_lrucache.hpp`、`tests/test_sharded_basic.cpp`

## 背景

`ShardedLRUCache` 将总容量拆分到多个 shard，每个 shard 内部维护一个独立的 `LRUCacheBase`。原实现使用固定 64 个 `Shard` 存储，但构造函数允许调用方传入任意 `num_shards`：

```cpp
explicit ShardedLRUCache(size_t total_capacity, size_t num_shards = kDefaultShards)
    : num_shards_(num_shards) {
    size_t per_shard = (total_capacity + num_shards - 1) / num_shards;
    for (size_t i = 0; i < num_shards; i++) {
        shards_[i].cache = LRUCacheBase<K, V>(per_shard);
    }
}
```

这带来两个问题：

1. `num_shards == 0` 会导致除零。
2. `num_shards > 64` 会写出 `std::array<Shard, 64>` 边界。

同时，按 `ceil(total_capacity / num_shards)` 分配容量会让实际容量超过调用方传入的 `total_capacity`。例如 `total_capacity = 1000`、`num_shards = 64` 时，每个 shard 容量为 16，总上限变成 1024。

## 决策

### 1. 参数采用窄契约

`ShardedLRUCache` 与 `LRUCacheBase` 保持一致：容量和分片数是调用方必须满足的前置条件。

- `total_capacity > 0`
- `1 <= num_shards <= 64`

违反前置条件时：

- Debug：`assert`
- Release：`std::terminate()`

这样可以避免非法参数静默进入除零、越界写或容量为 0 的错误状态。

### 2. `total_capacity` 表示严格总容量上限

`total_capacity` 的语义是整个 cache 的最大条目数，而不是“每 shard 上取整后的近似上限”。容量按以下方式分配：

```cpp
base = total_capacity / num_shards;
extra = total_capacity % num_shards;
capacity(i) = base + (i < extra ? 1 : 0);
```

例如：

| total_capacity | num_shards | 分配结果 | 实际总上限 |
|----------------|------------|----------|------------|
| 1000 | 64 | 前 40 个 shard 为 16，其余 24 个为 15 | 1000 |
| 128 | 64 | 每个 shard 为 2 | 128 |
| 3 | 2 | shard 0 为 2，shard 1 为 1 | 3 |

### 3. 分片数不能超过总容量

由于 `LRUCacheBase` 的容量必须大于 0，`ShardedLRUCache` 不能创建容量为 0 的 shard。因此 `num_shards` 还需要满足：

```cpp
num_shards <= total_capacity
```

如果调用方希望容量很小，应显式传入更少的 shard，例如：

```cpp
ShardedLRUCache<int, int> cache(3, 2);
```

默认 64 shard 适合容量至少为 64 的场景；小容量场景应减少 shard 数以保持契约清晰。

## 未采纳方案

### 继续使用 `ceil(total / shards)`

实现简单，但会让 API 名称与行为不一致。调用方传入 1000 时，合理预期是最多缓存 1000 条，而不是最多 1024 条。

### 允许空 shard

可以保留默认 64 shard 并让小容量 cache 中部分 shard 容量为 0，但这会要求 `LRUCacheBase` 支持 0 容量，违背已有错误处理策略，也让每个 shard 的行为出现特殊分支。

### 动态分配 shard 数组

将 `std::array<Shard, 64>` 改为动态数组可以支持任意 `num_shards`，但当前项目目标是比较全局锁与固定分片锁的性能。保持固定上限更简单，也更符合基准测试的设计。

## 测试要求

需要补充以下测试：

- 非法参数：`num_shards == 0`、`num_shards > 64`、`num_shards > total_capacity`
- 容量契约：实际 size 不超过 `total_capacity`
- 小容量场景：通过显式较少 shard 测试逐 shard 驱逐
- 分片配置：`num_shards()` 返回实际配置值
