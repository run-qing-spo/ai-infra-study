# Tradeoff #1：组合 vs 继承 —— LRUCache 与 LRUCacheBase

**状态**：已采纳（组合）  
**涉及文件**：`include/lru/lrucache_base.hpp`、`include/lru/lrucache.hpp`、`include/lru/sharded_lrucache.hpp`

## 背景

项目将 LRU 缓存拆成两层：

- **`LRUCacheBase`**：非线程安全的纯数据结构（hash map + 双向链表），`O(1)` get/put
- **`LRUCache` / `ShardedLRUCache`**：在线程安全包装层上加锁

`LRUCache` 的实现方式是持有一个 `LRUCacheBase` 成员，每个 public 方法先加锁再委托：

```cpp
std::optional<V> get(const K& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return base_.get(key);
}
```

另一种自然写法是 public 继承：

```cpp
class LRUCache : public LRUCacheBase<K, V> { ... };
```

本文说明为何选择组合。

## 决策

**采用组合（has-a）**：`LRUCache` 私有持有 `LRUCacheBase<K, V> base_`，不暴露基类接口。

## 理由

### 1. 不存在 is-a 关系

`LRUCache` 不是一种 `LRUCacheBase`。前者保证线程安全，后者明确**非**线程安全。若用继承：

```cpp
LRUCache<int, int> cache(100);
LRUCacheBase<int, int>* p = &cache;  // 隐式向上转型
p->get(key);  // 无锁访问，线程安全保证失效
```

这违反 Liskov 替换原则：子类不能在任何使用基类的上下文中安全替换基类。

### 2. 防止接口泄漏

继承时，基类的 public 方法会出现在子类接口上。调用方即使子类重写了 `get`，仍可通过作用域解析绕过锁：

```cpp
cache.LRUCacheBase::get(key);  // 绕过 mutex_
```

组合下 `base_` 是 `private` 成员，外部无法直接触及未加锁的实现。

### 3. 锁的生命周期边界清晰

组合让每个操作的语义固定为 **加锁 → 委托 → 解锁**。`LRUCache` 完全掌控 `mutex_` 的生命周期，`LRUCacheBase` 对并发一无所知。

继承不会自动破坏正确性（基类内部如 `put` 调 `evict_lru` 不涉及虚函数），但「哪些路径已加锁」在概念上更模糊，增加维护时的心智负担。

### 4. 为 ShardedLRUCache 铺路

`ShardedLRUCache` 持有 **多个** `LRUCacheBase` 实例（每个 shard 一个），各自配独立的 `mutex`：

```cpp
struct alignas(64) Shard {
    LRUCacheBase<K, V> cache{1};
    std::mutex mutex;
};
```

「一个对象内含多个基类子对象、每个子对象独立加锁」用继承无法表达，组合是天然建模方式。`lrucache_base.hpp` 头部注释也明确了这一分层意图：

> Thread-safe wrappers (LRUCache, ShardedLRUCache) layer locking on top.

## 对比

| 维度 | 继承 | 组合（当前选择） |
|------|------|------------------|
| 语义 | LRUCache is-a LRUCacheBase（不成立） | LRUCache has-a LRUCacheBase |
| 接口安全 | 基类方法可通过 `Base::method` 绕过锁 | `base_` 私有，锁不可绕过 |
| 多实例 | 无法表达 shard 场景 | 可持有任意多个 `LRUCacheBase` |
| 职责边界 | 锁与数据结构概念交织 | 加锁层与数据层正交分离 |
| 测试 | 基类逻辑需通过子类或友元测 | `LRUCacheBase` 可独立单测，无需锁 |

## 模式归类

本质是**装饰器 / 策略分层**：

- `LRUCacheBase`：数据结构策略（LRU 驱逐、链表维护）
- `LRUCache`：并发策略（全局互斥锁）
- `ShardedLRUCache`：并发策略（分片锁）

两层正交组合，而非通过继承耦合。

## 未采纳的替代方案

### private 继承

`class LRUCache : private LRUCacheBase<...>` 可阻止向上转型，但仍会把基类 public 方法带入 `LRUCache` 的接口（只是不可转型为基类指针）。`LRUCacheBase::get` 仍可从 `LRUCache` 对象上直接调用，锁依然可被绕过。组合在封装上更彻底。

### 在基类内加锁

把 `std::mutex` 放进 `LRUCacheBase` 会让核心数据结构与并发策略耦合，`ShardedLRUCache` 需要 per-shard 锁而非单一全局锁，基类无法同时服务两种并发模型。当前三层（Base → V1 → V2）分离也避免了 V2 中的双重加锁问题。

## 相关决策

- 三层分离（Base → V1 → V2）：见项目 [README.md](../README.md#设计决策)
- 为何 `get()` 不用 `shared_mutex`：读操作会修改链表（splice 到 MRU），共享锁不适用
