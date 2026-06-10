---
title: 多线程缓存设计综述
---

# 多线程缓存设计综述

## 背景

在生产环境中，缓存几乎总是被多线程并发访问。单线程的缓存实现（如简单的 `std::mutex + list`）无法满足高并发场景的需求。

本文总结了**多线程场景下缓存淘汰算法的标准做法**，涵盖 LRU、LFU、ARC、W-TinyLFU 等主流算法。

---

## 一、通用并发模式

所有缓存算法在多线程下都要解决同一个核心问题：**如何在保证正确性的同时减少锁竞争**

### 三种主流模式

| 模式 | 原理 | 优势 | 劣势 | 适用场景 |
|------|------|------|------|---------|
| **全局锁** | 一把 mutex 锁整个 cache | 简单，正确性易保证 | 锁竞争严重 | 低并发、小容量 |
| **分片锁（Sharding）** | 按 key hash 分成 N 个 shard，每 shard 一把锁 | 降低竞争，可扩展 | 跨 shard 统计难（如 LFU） | 读多写少、高并发 |
| **分段锁（Segment Lock）** | 类似 `ConcurrentHashMap` 的设计 | 动态扩容，均衡负载 | 实现复杂 | Java 标准库风格 |

### Sharding 代码示例（最常用）

```cpp
class ShardedCache {
    static constexpr int NUM_SHARDS = 64;
    std::array<std::mutex, NUM_SHARDS> mutexes;
    std::array<LRUCache, NUM_SHARDS> shards;

    int shard_id(const Key& key) {
        return std::hash<Key>{}(key) % NUM_SHARDS;
    }

    Value get(const Key& key) {
        int id = shard_id(key);
        std::lock_guard<std::mutex> lock(mutexes[id]);
        return shards[id].get(key);
    }

    void put(const Key& key, const Value& value) {
        int id = shard_id(key);
        std::lock_guard<std::mutex> lock(mutexes[id]);
        shards[id].put(key, value);
    }
};
```

### 参数选择建议

| 参数 | 推荐值 | 理由 |
|------|--------|------|
| **Shard 数量** | 2× 线程数（16 线程 → 32 或 64） | 平衡锁竞争和内存开销 |
| **Hash 函数** | `std::hash`、CityHash、xxHash | 快速且均匀分布 |

---

## 二、不同算法的并发方案差异

### LRU

**核心挑战**：`get` 需要移动链表节点 → 不能用读锁（`shared_mutex` 反而更慢）

| 方案 | 描述 | 难度 | 适用场景 |
|------|------|------|---------|
| **全局 mutex** | 一把锁锁住 `map + list` | ⭐ 简单 | 低并发、项目入门 |
| **Sharding** | 按 key 分片，每 shard 独立 LRU | ⭐⭐ 中等 | 生产环境 |
| **Read-Write Lock** | 如果 `get` 只读不移动（非严格 LRU） | ⭐⭐ 中等 | 读多写少 |
| **读写分离** | 写操作异步化，`get` 先读 read buffer | ⭐⭐⭐ 复杂 | 极高并发 |

**为什么 `shared_mutex` 不适合 LRU？**

`shared_mutex` 允许多个读者并发访问，写者独占。但 LRU 的 `get()` 操作会修改链表（把节点移到头部），意味着：
- `get()` 必须升级为写锁
- 反而比 `std::mutex` 更慢（锁升级的开销）

### ARC (Adaptive Replacement Cache)

**核心挑战**：需要维护 `T1`、`T2`、`B1`、`B2` 四个列表 + 全局 ghost 统计

| 方案 | 描述 | 难度 | Hit rate |
|------|------|------|----------|
| **全局锁** | 一把锁维护所有 list + ghost 统计 | ⭐ 简单 | 最高 |
| **Sharding + 全局 ghost** | list 按片分，ghost 统计全局（需原子更新） | ⭐⭐⭐ 复杂 | 略低 |
| **分区 ARC** | 每个 shard 独立维护 ARC（失去跨片自适应） | ⭐⭐ 中等 | 明显降低 |

**开源实现**：
- [ccache (Go)](https://github.com/karlseguin/ccache) — ARC + sharding
- [Go 1.18+ Generic ARC](https://github.com/Code-Hex/go-generics-cache) — 泛型 + sharding

### LFU / W-TinyLFU

**核心挑战**：需要全局频率统计 → 分片后如何统计全局热度？

| 方案 | 描述 | 难度 | 准确度 |
|------|------|------|--------|
| **全局锁** | 一把锁维护 frequency counter | ⭐ 简单 | 最高 |
| **Count-Min Sketch + Sharding** | 用概率数据结构统计全局热度，list 按片分 | ⭐⭐⭐ 复杂 | 概率近似 |
| **局部 LFU** | 每个 shard 独立统计（失去全局热度感知） | ⭐⭐ 中等 | 明显降低 |

**W-TinyLFU 标准实现**：
- **Caffeine (Java)**：使用 [Count-Min Sketch](https://en.wikipedia.org/wiki/Count%E2%80%93min_sketch) 做全局频率估计 + `ConcurrentHashMap` 做 sharding
- [论文链接](https://arxiv.org/abs/1512.00727) (2016)

---

## 三、不同算法的并发复杂度对比

| 算法 | 并发实现难度 | 是否需要全局状态 | 推荐方案 |
|------|------------|-----------------|---------|
| **LRU** | 低 | 否 | Sharding（最简单） |
| **LFU** | 中 | 是（频率统计） | Sharding + 全局原子计数 |
| **ARC** | 中 | 是（ghost list） | Sharding + 全局 ghost |
| **W-TinyLFU** | 高 | 是（Sketch） | Sharding + Count-Min Sketch |
| **2Q** | 中 | 部分 | Sharding（类似 LRU） |

**结论**：
- **LRU 最适合分片**：每个 shard 独立运行，不依赖全局状态
- **LFU/ARC/W-TinyLFU 难分片**：需要全局热度/ghost 统计，会增加跨 shard 开销

---

## 四、实现方案对比

### Java 生态

| 项目 | 算法 | 并发方案 | 特点 |
|------|------|---------|------|
| [Caffeine](https://github.com/ben-manes/caffeine) | W-TinyLFU | ConcurrentHashMap + Count-Min Sketch | 高性能，官方推荐 |
| [Guava Cache](https://github.com/google/guava) | LRU/LFU | ConcurrentHashMap | 简单易用 |
| [ConcurrentLinkedHashMap](https://github.com/ben-manes/concurrentlinkedhashmap) | LRU | Segmented LRU | 可配置淘汰策略 |

### C++ 生态

| 项目 | 算法 | 并发方案 | 特点 |
|------|------|---------|------|
| [Folly Cache](https://github.com/facebook/folly) | LRU/LRU-2 | Sharding | Facebook 开源 |
| [C++ Redis](https://github.com/redis/redis) | LRU | 全局锁 + lazy free | 简单但高效 |

### Go 生态

| 项目 | 算法 | 并发方案 | 特点 |
|------|------|---------|------|
| [ccache](https://github.com/karlseguin/ccache) | ARC | Sharding | 高性能 |
| [BigCache](https://github.com/allegro/bigcache) | LRU | Sharding | 无 GC 压力 |

---

## 五、推荐学习路径

### 入门阶段（项目 1）
1. 实现全局 `std::mutex` 版本的 LRU
2. 通过 ThreadSanitizer 验证无 race
3. 用 16 线程压测，建立性能基线

### 进阶阶段（项目 1-2）
1. 实现 Sharding 版本的 LRU
2. 对比全局锁 vs Sharding 的吞吐 + p99 延迟
3. 理解 cache line 对齐（`alignas(64)`）避免 false sharing

### 高级阶段（项目 4-6）
1. 研究 Caffeine 的 W-TinyLFU 实现
2. 实现 Count-Min Sketch + Sharding
3. trace-driven 验证不同策略的 hit rate

---

## 六、参考资源

### 论文
| 论文 | 核心贡献 |
|------|---------|
| [W-TinyLFU: A Window TinyLFU Admission Policy](https://arxiv.org/abs/1512.00727) | Count-Min Sketch + frequency-aware 淘汰 |
| [ARC: Adaptive Replacement Cache](https://ieeexplore.ieee.org/document/8667639) | 自适应 LRU/LFU 切换 |

### 书籍
| 书名 | 章节 |
|------|------|
| *Java Concurrency in Practice* | 第 15 章（并发缓存） |
| *The Art of Multiprocessor Programming* | 第 10-12 章（并发数据结构） |

### 博客
- [Caffeine Wiki](https://github.com/ben-manes/caffeine/wiki) — 高性能 Java cache 设计
- [Ben Manes Blog](https://ben-manes.com/) — W-TinyLFU 作者的技术博客

---

## 七、与项目的对应关系

| 文档 | 对应项目 | 内容 |
|------|---------|------|
| **本文档** | P1, P4, P5 | 多线程缓存设计模式 |
| [LRU/ARC/LFU 对比](./lru-arc-lfu-comparison.md) | P4, P5 | 淘汰策略选择与 trade-off |
| [项目 1 设计](/projects/project1_lru/README) | P1 | Thread-safe LRU 实现 |

---

*下一篇：[LRU/ARC/LFU 算法对比](./lru-arc-lfu-comparison.md)*