---
title: 项目 1 · Thread-safe LRU Cache
---

# 项目 1：Thread-safe LRU Cache

> **定位**：这是整个 KV Cache 项目的**第一块基石**。KV Cache 的本质就是一个缓存系统，LRU 是最常见的淘汰策略。掌握线程安全的 LRU 实现是理解后续多级缓存、异步 I/O 等复杂机制的先决条件。

---

## 1. 概念理解

### 1.1 什么是 KV Cache？

**KV Cache (Key-Value Cache)** 是大模型推理中的核心数据结构，用于缓存 Transformer 每一层的注意力计算结果。

**直观理解**：
- 当你问模型 "北京的天气怎么样？" 时，模型需要处理 "北京"、"的"、"天气"、"怎么样" 这些 token
- 如果你紧接着问 "那上海呢？"，模型已经 "记住" 了前半段对话的 KV（"北京"、"的"、"天气"、"怎么样"），只需要计算 "上海" 这个新 token
- 这些被 "记住" 的中间结果，就是 KV Cache

**技术本质**：
- **Key Cache**：注意力机制中的 K 矩阵，用于计算注意力权重
- **Value Cache**：注意力机制中的 V 矩阵，用于加权求和
- 存储形式：`(layer_id, token_position, head_id, ...)` → `tensor` 的键值对

### 1.2 为什么需要 LRU？

大模型推理的显存是有限的（比如 4090 24GB），但对话的长度可能无限长。当显存满了之后，需要**淘汰一些旧的 KV 来腾出空间给新的**。

**LRU (Least Recently Used)** 是最直观的淘汰策略：
- "最近最少使用" 的 KV 最有可能不会再被用到
- 每次访问（get/put）一个 key，就把它移到队列头部
- 需要淘汰时，删除队列尾部

**为什么不是 FIFO？**
- 对话有 "回溯" 特征：用户可能会问 "刚才你说的那个..."
- FIFO 只看 "最早进来的"，LRU 看 "最久没被用的" —— 后者更符合对话模式

### 1.3 KV Cache 的并发特征

大模型推理服务通常需要处理**高并发请求**：
- 同一时间可能有 100 个用户在对话
- 每个用户的请求需要读写多个 KV Cache
- 如果缓存加锁粒度太粗，会严重降低吞吐

**并发模式**：
- **读多写少**：大多数时候是 `get(key)` 读取 KV，`put(key, value)` 只有当新 token 生成时才会发生
- **热点数据**：热门对话的 KV 会被频繁访问
- **跨线程**：请求处理线程可能是多个，需要共享同一个 LRU Cache

---

## 2. 要解决的核心问题

### 2.1 数据结构设计

**问题**：LRU 需要 O(1) 的 `get` 和 `put`，但常规数据结构无法同时满足：
- `std::list` 支持快速删除插入，但查找是 O(n)
- `std::unordered_map` 查找是 O(1)，但无法按访问顺序排序

**解决方案**：哈希表 + 双向链表组合
- `unordered_map<key, iterator>`：快速找到链表中的位置
- `list<pair<key, value>>`：维护访问顺序，头部是最近使用

```
get("A") 的流程：
1. map.find("A") → 找到迭代器
2. 把 iterator 指向的节点移到 list 头部
3. 返回 value
```

### 2.2 线程安全

**背景阅读**：详见 [多线程缓存设计综述](/algorithms/concurrent-cache)

**问题**：多个线程同时访问同一个 LRU Cache，需要保证：
- `get()` 和 `put()` 不会互相干扰
- 链表的移动操作是原子的
- 没有数据竞争（data race）

**实现方案对比**：

| 方案 | 描述 | 难度 | 本项目是否实现 |
|------|------|------|--------------|
| **全局 mutex** | 一把锁锁住 `map + list` | ⭐ 简单 | ✅ 是 |
| **Sharding** | 按 key hash 分成 N 个 shard | ⭐⭐ 中等 | ✅ 是（第二版） |
| **Read-Write Lock** | `shared_mutex`（不适合 LRU） | ⭐⭐ 中等 | ❌ 仅讨论 |

**为什么 `shared_mutex` 不适合 LRU？**

详见 [多线程缓存设计综述](/algorithms/concurrent-cache) → LRU 章节。简单原因：
- `get()` 需要修改链表（把节点移到头部）
- `shared_mutex` 的读锁不允许修改数据结构
- `get()` 必须升级为写锁 → 反而比 `std::mutex` 更慢

**本项目实施路径**：
1. **第一步**：全局 `std::mutex` 版本 + TSan 验证（建立正确性基线）
2. **第二步**：Sharding 版本（64 shards，对比性能提升）

### 2.3 内存管理

**问题**：
- LRU 淘汰时需要释放内存
- `list` 的节点和 `map` 的值是指向同一块内存，避免 double free
- 大模型 KV Cache 可能很大，需要考虑内存分配的性能

**解决方案**：
- `map` 的值存储的是 `list` 的迭代器，不是数据副本
- 淘汰时只从 `list` erase，`map` 中对应的 iterator 自动失效
- 使用 `std::list<value_t>` 避免频繁 new/delete

---

## 3. 与 KV Cache 的直接关联

### 3.1 数据形状

大模型 KV Cache 的 key 不是简单的字符串，而是一个复合键：
```
key = (request_id, layer_id, position_id, ...)
```

**为什么这么复杂？**
- `request_id`：区分不同的对话请求
- `layer_id`：Transformer 有多个层（比如 LLaMA-7B 有 32 层），每层一个 KV Cache
- `position_id`：token 在对话中的位置

**本项目简化**：先用 `uint64_t` 作为 key，理解机制后扩展到复合键

### 3.2 淘汰策略的选择

LRU 是 KV Cache 的基线策略，但实际系统中会用更复杂的策略：
- **W-TinyLFU**（项目 4）：兼顾频率和近期访问
- **Segmented LRU**：把 cache 分成 probation（考察区）和 protected（保护区）

**为什么本项目用 LRU？**
- LRU 是理解淘汰策略的起点
- 实现简单，便于验证线程安全
- 后续项目会逐步升级到更复杂的策略

---

## 4. 交付标准

### 4.1 功能要求

- [ ] 实现 `LRUCache` 类，支持 `get(key)` 和 `put(key, value)`
- [ ] 支持 `capacity` 参数，超过容量时自动淘汰最久未使用的数据
- [ ] `get` 命中时把 key 移到最近使用位置
- [ ] `put` 时如果 key 已存在，更新 value 并移到最近使用位置

### 4.2 并发要求

- [ ] 使用 `std::mutex` 保证线程安全
- [ ] 16 线程并发压测通过（吞吐 + p99 延迟数据）
- [ ] 通过 ThreadSanitizer（`-fsanitize=thread`），0 race
- [ ] 正确处理竞争条件（比如两个线程同时 `put` 同一个 key）

### 4.3 文档要求

- [ ] README.md 说明设计决策
- [ ] 测试代码在 `tests/` 目录
- [ ] 性能数据表（不同线程数下的吞吐和延迟）

---

## 5. 代码框架

```cpp
// lrucache.hpp
#include <mutex>
#include <list>
#include <unordered_map>

template <typename K, typename V>
class LRUCache {
public:
    explicit LRUCache(size_t capacity) : capacity_(capacity) {}

    std::optional<V> get(const K& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_map_.find(key);
        if (it == cache_map_.end()) {
            return std::nullopt;
        }

        // 把节点移到链表头部
        cache_list_.splice(cache_list_.begin(), cache_list_, it->second);

        return it->second->second;
    }

    void put(const K& key, const V& value) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = cache_map_.find(key);
        if (it != cache_map_.end()) {
            // key 已存在，更新 value 并移到头部
            it->second->second = value;
            cache_list_.splice(cache_list_.begin(), cache_list_, it->second);
            return;
        }

        // 如果超过容量，淘汰尾部
        if (cache_list_.size() >= capacity_) {
            auto last = cache_list_.back();
            cache_map_.erase(last.first);
            cache_list_.pop_back();
        }

        // 插入新节点
        cache_list_.emplace_front(key, value);
        cache_map_[key] = cache_list_.begin();
    }

private:
    size_t capacity_;
    std::list<std::pair<K, V>> cache_list_;  // 头部是最近使用
    std::unordered_map<K, typename std::list<std::pair<K, V>>::iterator> cache_map_;
    mutable std::mutex mutex_;
};
```

---

## 6. 测试验证

### 6.1 单线程测试

```cpp
TEST(LRUCache, BasicOperations) {
    LRUCache<int, int> cache(3);
    cache.put(1, 100);
    cache.put(2, 200);
    cache.put(3, 300);

    EXPECT_EQ(cache.get(1), 100);
    EXPECT_EQ(cache.get(2), 200);
    EXPECT_EQ(cache.get(3), 300);

    cache.put(4, 400);  // 淘汰 key=1
    EXPECT_EQ(cache.get(1), std::nullopt);
    EXPECT_EQ(cache.get(4), 400);
}
```

### 6.2 并发测试

```cpp
TEST(LRUCache, Concurrency) {
    LRUCache<int, int> cache(1000);
    const int num_threads = 16;
    const int ops_per_thread = 10000;

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&cache, i, ops_per_thread]() {
            for (int j = 0; j < ops_per_thread; j++) {
                int key = (i * ops_per_thread + j) % 2000;
                if (j % 2 == 0) {
                    cache.put(key, i * 1000 + j);
                } else {
                    cache.get(key);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(cache.size(), 1000);  // 没有泄漏
}
```

### 6.3 TSan 验证

```bash
g++ -fsanitize=thread -g -O1 -pthread tests/lrucache_test.cpp -o lrucache_test
./lrucache_test
# 预期：0 race
```

---

## 7. 扩展思考

### 7.1 为什么 `shared_mutex` 不适合 LRU？

`shared_mutex` 允许多个读者并发访问，写者独占。看起来很适合"读多写少"的场景，但 LRU 有个问题：
- `get()` 操作会修改链表（把节点移到头部）
- `shared_mutex` 的读者不允许修改数据结构
- 如果用 `shared_mutex`，`get()` 必须升级为写锁，反而比 `std::mutex` 更慢

### 7.2 真实 KV Cache 的锁策略

真实系统（如 vLLM、FlexKV）会用更精细的锁策略：
- 每个 request 一把锁（而不是整个 cache 一把锁）
- 每个 shard（segment）一把锁（比如 256 个 shard）
- 读写分离：读 cache 用无锁结构，写操作用版本控制

这些优化会在后续项目中逐步引入。

---

## 8. 参考资源

### 背景文档
- [多线程缓存设计综述](/algorithms/concurrent-cache) — 并发模式、Sharding、不同算法的并发方案
- [LRU/ARC/LFU 算法对比](/algorithms/lru-arc-lfu-comparison) — 各算法原理、性能对比、适用场景

### 系统设计
- **FlexKV 论文**：第 3.2 节 "Storage Engine" 描述了多级缓存设计
- **vLLM 源码**：`vllm/block_manager.py` 中的 KV Cache 管理
- **LevelDB/RocksDB**：它们的 LRU Table 实现是经典参考

### 工具
- **TSan 文档**：`https://github.com/google/sanitizers/wiki/ThreadSanitizerCppManual`

### 开源实现参考
- [Caffeine (Java)](https://github.com/ben-manes/caffeine) — W-TinyLFU + ConcurrentHashMap
- [ccache (Go)](https://github.com/karlseguin/ccache) — ARC + Sharding
- [Folly Cache (C++)](https://github.com/facebook/folly) — LRU + Sharding

---

*下一篇：项目 2 · Bounded SPSC Lock-free Queue（见 [项目总览](/projects/overview)）— 从无锁队列理解 producer-consumer 模式，这是异步 I/O 的基础。*