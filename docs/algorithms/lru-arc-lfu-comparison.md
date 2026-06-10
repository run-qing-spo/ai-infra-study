---
title: LRU / ARC / LFU 算法对比
---

# LRU / ARC / LFU 算法对比

缓存淘汰策略的选择是影响系统性能的关键因素。本文对比了主流缓存算法的原理、适用场景和性能特征。

---

## 一、算法列表

| 算法 | 全称 | 时间复杂度 | 空间复杂度 | 热度追踪 | 生产级使用 |
|------|------|----------|----------|---------|----------|
| **LRU** | Least Recently Used | O(1) | O(n) | 最近访问时间 | ✓ |
| **LFU** | Least Frequently Used | O(1) | O(n) | 访问频率 | - |
| **ARC** | Adaptive Replacement Cache | O(1) | O(2n) | 时间 + 频率（自适应） | ✓ |
| **W-TinyLFU** | Window TinyLFU | O(1) | O(n) | 滑动窗口频率 | ✓ (Caffeine) |
| **2Q** | Two Queues | O(1) | O(n) | 访问时间（双队列） | ✓ (Linux kernel) |

---

## 二、算法详解

### LRU (Least Recently Used)

**原理**：淘汰最近最少使用的项

**数据结构**：`HashMap<Key, Node*>` + `DoublyLinkedList`

**适用场景**：
- 时间局部性强的 workload（如 Web 缓存）
- 访问模式有明显的"热点"

**缺点**：
- 对扫描（scan）操作敏感（一次扫描会把 cache 全部刷空）
- 对循环访问（loop）的 cache miss 高

**变体**：
- **Segmented LRU**：分为 `protected` 和 `probation` 两段
- **LRU-K**：追踪过去 K 次访问时间

```cpp
// LRU 核心操作
get(key) {
    node = hashmap[key];
    if (node) {
        move_to_head(node);  // O(1) 指针操作
        return node.value;
    }
    return null;
}

put(key, value) {
    if (hashmap.contains(key)) {
        node = hashmap[key];
        node.value = value;
        move_to_head(node);
    } else {
        evict_if_needed();
        node = new Node(key, value);
        add_to_head(node);
        hashmap[key] = node;
    }
}
```

### LFU (Least Frequently Used)

**原理**：淘汰访问频率最低的项

**数据结构**：`HashMap<Key, Count>` + `HashMap<Count, Set<Key>>`

**适用场景**：
- 频率局部性强的 workload
- 热点数据长期稳定

**缺点**：
- 对突发流量敏感（新热点需要时间累积频率）
- 历史频率难以衰减（老热点会一直保留）

**变体**：
- **LFU with aging**：定期衰减频率计数
- **LFU with sliding window**：只统计最近窗口的频率

```cpp
// LFU 核心操作（简化版）
get(key) {
    freq = hashmap[key];
    freq_to_keys[freq].erase(key);
    freq++;
    freq_to_keys[freq].insert(key);
    hashmap[key] = freq;
}

put(key, value) {
    evict_min_freq_key();  // 淘汰频率最低的
    freq = 1;
    hashmap[key] = freq;
    freq_to_keys[freq].insert(key);
}
```

### ARC (Adaptive Replacement Cache)

**原理**：自适应地在 LRU 和 LFU 之间切换

**数据结构**：
- `T1`：最近访问且只访问一次（LRU 风格）
- `T2`：最近访问且访问多次（LFU 风格）
- `B1`：最近从 T1 淘汰的 ghost（用于判断是否应该增加 T1 容量）
- `B2`：最近从 T2 淘汰的 ghost（用于判断是否应该增加 T2 容量）

**核心思想**：
- 如果 ghost 在 `B1` 中被访问 → 增加 T2 容量（LFU 更适合）
- 如果 ghost 在 `B2` 中被访问 → 增加 T1 容量（LRU 更适合）

**适用场景**：
- workload 模式变化频繁
- 需要自适应的系统

**缺点**：
- 实现复杂（4 个列表 + 状态机）
- 并发实现难度高（需要全局 ghost 统计）

### W-TinyLFU (Window TinyLFU)

**原理**：基于频率的淘汰策略，使用 Count-Min Sketch 统计频率

**数据结构**：
- **Admission Filter**：Count-Min Sketch 统计近期访问频率
- **Main Cache**：分为 `Window LRU` + `Probation LRU` + `Protected LRU`

**核心思想**：
- 新 item 先经过 admission filter，频率足够高才进入 main cache
- Main cache 分为三段，平衡时间和频率

**适用场景**：
- 大容量缓存（GB 级别）
- 频率统计需要节省内存

**优点**：
- 频率统计内存占用极低（Sketch 是概率数据结构）
- 在 trace 测试中表现优异

**缺点**：
- 实现最复杂
- 依赖概率数据结构（有误差）

---

## 三、性能对比（trace-driven 测试）

### Trace 来源

| Trace | 描述 | 来源 |
|-------|------|------|
| **MSR** | Web cache trace | Microsoft Research |
| **OLTP** | Database workload | Florida大学 |
| **Wikipedia** | Wiki cache trace | Wikipedia logs |

### Hit Rate 对比

| 算法 | MSR trace | OLTP trace | Wikipedia trace | 实现难度 |
|------|----------|-----------|-----------------|---------|
| LRU | 35-45% | 25-35% | 40-50% | ⭐ |
| LFU | 40-50% | 35-45% | 45-55% | ⭐⭐ |
| ARC | 45-55% | 40-50% | 50-60% | ⭐⭐⭐ |
| W-TinyLFU | 48-58% | 42-52% | 52-62% | ⭐⭐⭐⭐ |
| 2Q | 42-52% | 35-45% | 48-58% | ⭐⭐ |

*数据来源：Caffeine Wiki、原始论文*

---

## 四、适用场景决策树

```
开始
 │
 ├─ workload 稳定？
 │   ├─ 是 → LFU（频率稳定）
 │   └─ 否 → 继续
 │
 ├─ 需要自适应？
 │   ├─ 是 → ARC
 │   └─ 否 → 继续
 │
 ├─ 内存充足？
 │   ├─ 是 → W-TinyLFU（但实现复杂）
 │   └─ 否 → 继续
 │
 └─ 实现复杂度敏感？
     ├─ 是 → LRU（项目 1-3）
     └─ 否 → 2Q 或 W-TinyLFU（项目 4+）
```

---

## 五、并发实现对比

| 算法 | 是否需要全局状态 | Sharding 难度 | 生产级方案 |
|------|-----------------|-------------|-----------|
| LRU | 否 | ⭐ 简单 | Sharding |
| LFU | 是（频率统计） | ⭐⭐ 中等 | Sharding + 全局原子 |
| ARC | 是（ghost list） | ⭐⭐⭐ 复杂 | Sharding + 全局 ghost |
| W-TinyLFU | 是（Sketch） | ⭐⭐⭐⭐ 很复杂 | Sharding + Count-Min Sketch |
| 2Q | 否 | ⭐⭐ 中等 | Sharding |

---

## 六、与项目的对应关系

| 阶段 | 项目 | 算法 | 侧重点 |
|------|------|------|--------|
| **Phase 1** | P1 | LRU | 线程安全基础 |
| **Phase 2** | P4 | LRU/LFU/ARC/W-TinyLFU | 策略对比与选择 |
| **Phase 3** | P5 | LRU | 多级缓存协同 |
| **Phase 4** | P6 | 任意 | 异步传输 |

---

## 七、推荐学习顺序

1. **LRU** → 理解基础淘汰机制
2. **2Q** → 理解分段思想
3. **ARC** → 理解自适应
4. **W-TinyLFU** → 理解概率数据结构

每个阶段都要关注：**单线程正确性** → **多线程安全性** → **性能优化**

---

## 八、Linux 内核的缓存置换方案

Linux 内核的页面回收（page reclaim）机制是经过几十年锤炼的生产级设计，值得深入学习。

### Two-List LRU（Active/Inactive LRU）

**原理**：将页面分为 **active** 和 **inactive** 两个 LRU 列表

| 列表 | 描述 | 淘汰顺序 |
|------|------|---------|
| **active** | 热点页面，最近被访问过 | 后淘汰 |
| **inactive** | 冷页面，很久没被访问过 | 先淘汰 |

**页面迁移逻辑**：
```
inactive 页面被访问 → 激活到 active 列表
active 页面很久没被访问 → 降级到 inactive 列表
需要回收时 → 优先从 inactive 列表淘汰
```

**为什么比单列表 LRU 好？**
- 避免一次扫描把 cache 全部刷空（scan immunity）
- 热点页面在 active 列表有保护期
- 减少锁竞争（active/inactive 可独立加锁）

**内核实现**：
- 源码：`mm/vmscan.c` 中的 `shrink_active_list()` 和 `shrink_inactive_list()`
- 文档：[Active/Inactive LRU Lists](https://www.kernel.org/doc/Documentation/vm/active_inactive_list.txt)

### CLOCK 算法

Linux 早期使用 CLOCK 算法（近似 LRU），现在主要用于某些特定场景。

**原理**：用循环链表 + reference bit 近似 LRU

| 特性 | 描述 |
|------|------|
| 实现 | 链表遍历 + reference bit |
| 复杂度 | O(n) 遍历，但摊销 O(1) |
| 优点 | 硬件支持好（MMU 提供访问位） |

**变体**：
- **CLOCK-PRO**：区分 cold/hot 页面，更接近真实 LRU
- **Second-Chance**：给页面第二次机会

### Working Set Refault Detection

Linux 5.x 引入了 working set 检测，用于识别"页面被回收后又立即访问"的模式。

**原理**：记录页面被回收的时间戳，如果 refault 时间间隔短，说明是 working set 页面。

**代码**：
- `workingset_age()` 和 `workingset_refault()` 函数
- 用于提升 cache hit rate

### 对项目的启发

| 内核设计 | 对项目的启示 |
|---------|------------|
| **Active/Inactive 分段** | 项目 4 可以实现 segmented LRU |
| **异步回收（kswapd）** | 项目 6 的异步传输可以参考 |
| **working set 检测** | 可以用于预取策略（提前把可能访问的数据拉上来） |

### 学习路径

1. **读源码**：`mm/vmscan.c` — 理解内核如何做页面回收
2. **读文档**：[Linux Kernel Documentation - VM](https://www.kernel.org/doc/Documentation/vm/)
3. **读文章**：[LWN.net - Memory management](https://lwn.net/Articles/832858/)

---

## 九、参考资源

### 论文
- [ARC: Adaptive Replacement Cache](https://ieeexplore.ieee.org/document/8667639) — 2003
- [W-TinyLFU: A Window TinyLFU Admission Policy](https://arxiv.org/abs/1512.00727) — 2016
- [2Q: A Fast and Approximate Memory Manager](https://dl.acm.org/doi/10.1145/1007710.1007761) — 1994

### Linux 内核
- **源码**：[mm/vmscan.c](https://elixir.bootlin.com/linux/latest/source/mm/vmscan.c) — 页面回收实现
- **文档**：[Active/Inactive LRU Lists](https://www.kernel.org/doc/Documentation/vm/active_inactive_list.txt)
- **文档**：[VM Overview](https://www.kernel.org/doc/Documentation/vm/mm.txt)
- **文章**：[LWN: Memory management improvements](https://lwn.net/Articles/832858/)
- **文章**：[Linux Kernel Memory Management](https://kernelnewbies.org/Linux_Kernel_Memory_Management)

### 开源实现
- [Caffeine (Java)](https://github.com/ben-manes/caffeine) — W-TinyLFU
- [ccache (Go)](https://github.com/karlseguin/ccache) — ARC
- [ConcurrentLinkedHashMap (Java)](https://github.com/ben-manes/concurrentlinkedhashmap) — LRU/2Q

---

*上一篇：[多线程缓存设计综述](./concurrent-cache.md) | 下一篇：[项目 1 实现](/projects/project1-lru)*