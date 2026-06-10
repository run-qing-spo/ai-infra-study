# Tradeoff #2：特殊成员函数与错误处理策略

**状态**：已采纳  
**涉及文件**：`include/lru/lrucache_base.hpp`、`include/lru/lrucache.hpp`、`include/lru/sharded_lrucache.hpp`

## 背景

`LRUCacheBase` 的原始实现只声明了一个 `explicit` 构造函数，其余五大特殊成员函数（拷贝构造、拷贝赋值、移动构造、移动赋值、析构函数）均依赖编译器隐式生成。同时，前置条件检查仅使用 `assert`，未使用异常。

本文记录两项决策的理由：

1. 拷贝删除 + 移动保留
2. `assert` + `std::terminate()` 双重守卫而非异常

---

## 决策 1：拷贝删除，移动保留

### 拷贝为何危险

`LRUCacheBase` 的核心数据结构：

```cpp
std::list<std::pair<K, V>> list_;                                    // front = MRU
std::unordered_map<K, typename std::list<std::pair<K, V>>::iterator> map_;
```

`map_` 中存储的是指向 `list_` 节点的**迭代器**。编译器隐式生成的拷贝构造函数执行逐成员拷贝：

1. `list_` 被拷贝 → 新链表，**新节点，新地址**
2. `map_` 被拷贝 → 迭代器**仍指向原 list 的节点**

```cpp
LRUCacheBase<int, int> a(10);
a.put(1, 100);

auto b = a;  // 隐式拷贝
// b.map_ 中的迭代器指向 a.list_ 的节点
// a 被修改或析构后 → b 中迭代器全部悬空 → use-after-free
```

这不是理论风险——在多线程场景中，`ShardedLRUCache` 的每个 shard 独立操作，一旦误拷贝后并发访问，悬空迭代器会导致数据竞争和内存损坏，且极难调试。

### 移动为何安全

`std::list` 的移动语义是**节点转移**（内部等价于 splice），不创建新节点：

```cpp
auto b = std::move(a);
// a.list_ 的节点物理上搬到了 b.list_（同一块内存）
// b.map_ 中的迭代器仍然指向这些节点 → 依然有效
```

C++ 标准 [list.cons] 保证：移动后，指向被移动 list 中元素的迭代器仍然有效，且现在指向新 list 中的同一元素。因此 `LRUCacheBase` 的移动语义是安全的。

### 各类的最终策略

| 类 | 拷贝 | 移动 | 原因 |
|----|------|------|------|
| `LRUCacheBase` | `= delete` | `= default` | 迭代器悬空风险；list 节点转移使移动安全 |
| `LRUCache` | `= delete` | `= default` | `std::mutex` 不可拷贝；`LRUCacheBase` 拷贝已删除 |
| `ShardedLRUCache` | `= delete` | `= delete` | `Shard` 含 `std::mutex`（不可拷贝/移动）且 `alignas(64)` + `std::array` 使整体不可移动 |

### ShardedLRUCache 为何移动也删除

`Shard` 结构体含 `std::mutex`（不可移动赋值），且 `alignas(64)` 使其不可被简单地逐字节搬移。`std::array<Shard, 64>` 的移动赋值要求元素可移动赋值，因此整个 `ShardedLRUCache` 不可移动。

如果未来需要移动语义，可将 `std::array` 替换为 `std::unique_ptr<Shard[]>` 或 `std::vector<Shard>`。

### 为何显式声明而非依赖隐式

- `LRUCacheBase`：隐式拷贝虽然因 `std::list`/`unordered_map` 可拷贝而被生成，但**语义错误**，必须显式 `= delete`
- `LRUCache`：`std::mutex` 不可拷贝使隐式拷贝已被编译器删除，但显式声明是**文档性**的——让读者无需追踪成员类型即可理解设计意图
- `ShardedLRUCache`：同上，显式声明移动删除让 `alignas` + `mutex` 的不可移动约束对读者可见

---

## 决策 2：`assert` + `std::terminate()` 而非异常

### 当前策略

```cpp
explicit LRUCacheBase(size_t capacity) : capacity_(capacity) {
    assert(capacity > 0 && "LRU capacity must be > 0");  // Debug: 立即崩溃 + 消息
    if (capacity == 0) {
        std::terminate();  // Release: 保证不会静默出错
    }
}
```

两层守卫各司其职：

| 层 | 构建模式 | 行为 | 目的 |
|----|---------|------|------|
| `assert` | Debug | 打印表达式、文件名、行号后 `abort()` | 开发阶段快速定位 bug |
| `std::terminate()` | Release | 调用 `std::terminate_handler`（默认 `abort()`） | 保证不静默进入 UB |

### 为何不用异常

1. **前置条件 ≠ 运行时错误**：`capacity <= 0` 是调用方的编程错误，不是可恢复的业务异常。传入 0 没有合理的恢复路径——一个容量为 0 的 LRU 缓存没有意义，创建它本身就是 bug。

2. **底层组件的性能契约**：`LRUCacheBase` 定位为纯数据结构，其操作需要 `O(1)` 的严格保证。异常路径引入隐藏的分支和控制流，与零开销抽象的目标冲突。

3. **异常安全连锁反应**：如果 `put` 抛出异常，`list_` 和 `map_` 的中间状态需要回滚。对于 hash map + 双向链表的联动结构，提供强异常安全保证需要大量 try-catch 包裹，代码复杂度急剧上升。

4. **构建环境兼容性**：嵌入式、游戏引擎、高频交易等场景常使用 `-fno-exceptions` 编译。no-exception 设计保证最大可移植性。

### 为何不用纯 `assert`（不加 `terminate`）

纯 `assert` 在 Release 构建（`-DNDEBUG`）下被完全移除。此时 `capacity = 0` 会滑过检查：

```cpp
// capacity_ == 0
if (list_.size() >= capacity_) {  // 0 >= 0 → true
    evict_lru();                   // 每次put都驱逐 → 缓存永远为空
}
```

不会崩溃，但**行为静默错误**——缓存总是空的，调用方无从得知。这是比崩溃更糟的结果。

### `std::terminate()` vs `throw` 的取舍

| 维度 | `std::terminate()` | `throw` |
|------|-------------------|---------|
| Release 开销 | 零（正常路径无分支） | 零（异常表不占运行时间）但增大二进制 |
| 构建兼容性 | 无限制 | 需要 RTTI + 异常支持 |
| 可恢复性 | 不可恢复（进程终止） | 可在调用方 catch |
| 语义 | "这是 bug，程序不该继续" | "这是异常情况，可能恢复" |
| 异常安全要求 | 无 | 要求所有操作提供异常安全保证 |

### 错误处理分层策略

本项目整体遵循以下分层：

```
┌──────────────────────────────────────────────────┐
│  可预期的缺失（key 不存在）                         │
│  → 返回值：std::optional<V> / bool               │
│  → 调用方有明确的处理路径                           │
├──────────────────────────────────────────────────┤
│  不可恢复的编程错误（capacity ≤ 0）                  │
│  → assert + std::terminate()                     │
│  → 调用方违反前置条件，程序不应继续                  │
├──────────────────────────────────────────────────┤
│  系统级资源耗尽（内存分配失败）                       │
│  → 不处理（由操作系统 / 全局 new handler 负责）      │
└──────────────────────────────────────────────────┘
```

这遵循 **窄契约（narrow contract）** 哲学：接口的前提条件由调用方保证，违反即终止，不做"优雅降级"。

---

## 相关决策

- [组合 vs 继承](composition-vs-inheritance.md)：`LRUCache` 通过组合持有 `LRUCacheBase`，拷贝/移动策略在组合关系下自然传播
- `explicit` 构造函数：阻止 `size_t` 到 `LRUCacheBase` 的隐式转换，与窄契约哲学一致——构造意图必须显式
