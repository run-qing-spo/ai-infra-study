# LRU 单线程版实现 —— 学习笔记总结

本文档把从零写 `lrucache_base<K, V>` 过程中遇到的 C++ 概念和数据结构权衡整理成一份可回顾的笔记。

---

## 一、数据结构设计

### 1.1 LRU 本质

固定容量缓存：超容时**淘汰最久未访问**的条目。核心操作要 O(1)：

- `get(key)`：命中返回 value，且把节点提到"最新"位置；
- `push(key, value)`：插入；满了就先淘汰最旧。

要做到 O(1)，标准组合是：**hash map（key → 节点位置）+ 双向链表（维护访问顺序）**。光遍历链表查找是 O(N)，规模一上来就崩。

### 1.2 数组模拟链表

链表节点不在堆上 `new`，而是放在 `std::vector<Node>` 里，节点之间用**下标**而非指针连接：

```cpp
struct Node {
    K key;
    std::shared_ptr<V> value;
    int32_t next;   // 池下标
    int32_t prev;
};
```

收益：
1. **整块内存连续**：所有节点挤在一片连续区域，cache 局部性显著优于堆散落；
2. **下标比指针小**：`int32_t` 占 4 字节，指针 8 字节，cache line 能塞更多；
3. **无 `new/delete` 开销**：vector 一次分配，节点级别零分配。

注意：LRU 的访问跳转本就不连续（沿 `next` 跳到任意下标），所以"完美顺序访问"做不到，但**连续内存 + 小下标**已经比堆散落好得多。

### 1.3 Free list：复用 `next` 字段

空闲槽和已用槽**共享同一个 Node 结构**，靠 `_head_free` 和 `_head_used` 区分。空闲链表只需单向（栈式 push/pop 头部）：

- 分配：`idx = _head_free; _head_free = pool[idx].next;` —— O(1)
- 释放：`pool[idx].next = _head_free; _head_free = idx;` —— O(1)

零额外结构、零碎片化逻辑。比"区间型 free list（存 start+length）"简单得多，性能等价。

### 1.4 双哨兵：消除边界分支

`used` 链表两端各放一个永远存在的 sentinel Node：

```
head_sentinel ⇄ A ⇄ B ⇄ C ⇄ tail_sentinel
```

物理布局：

```cpp
pool.reserve(x + 2);
// pool[0..x-1] : 真实节点
// pool[x]      : head_sentinel
// pool[x+1]    : tail_sentinel
```

收益：
1. **任何真实节点的 prev/next 都指向有效 Node**（最坏指向哨兵），所有 `pool[next]` / `pool[prev]` 都合法访问，**零越界**；
2. **插入/删除无 `if` 分支**：
   ```cpp
   // 摘除（无论 idx 在头/尾/中间）
   pool[pool[idx].prev].next = pool[idx].next;
   pool[pool[idx].next].prev = pool[idx].prev;
   ```
3. **`eraseFromTail` 直接 O(1)**：`pool[tail_sentinel].prev` 就是最后一个真实节点。

> **Free list 不需要哨兵**：单向链表只判一次"空"就够。哨兵的回报集中在双向链表的边界处理。

### 1.5 Sentinel 是什么

**故意挑选一个不可能是合法值的特殊标记**，表示"这里没东西"。

- **方案 A**：用一个"出界下标"（如 `_capacity`）当 null —— 简单但每次访问要前置 `if`，否则越界 UB；
- **方案 B**：放一个**真实存在的占位节点**当哨兵 —— 多花 1~2 个槽，换来零分支访问。

写双向链表/树时，方案 B 几乎总赢。

---

## 二、C++ 模板基础

### 2.1 模板参数声明

```cpp
template<V>           // ✗ 缺关键字
template<typename V>  // ✓
template<class V>     // ✓ 等价于上面，更老的写法
```

### 2.2 Injected class name

在类模板**定义体内部**，类名自动指代"当前实例化的类型"：

```cpp
template<typename V>
class lrucache_base {
    lrucache_base();        // = lrucache_base<V>()，编译器自动补
    ~lrucache_base();       // 同样无需写 <V>
};
```

类体**外面**必须写完整：

```cpp
template<typename V>
lrucache_base<V>::lrucache_base() { ... }
```

注意：写"半个"模板参数（比如双参类模板里只写一个）会让类型名残缺，**编译失败**。要么全写，要么用 injected name 一个不写。

---

## 三、`std::vector` 的内存语义

### 3.1 `reserve` vs `resize`

| 操作 | 分配内存？ | 构造对象？ | `size()` 变化 | 需要默认构造？ |
|---|---|---|---|---|
| `vector<T> v;` | 否 | 否 | 0 | ✗ |
| `v.reserve(n)` | **仅分配 raw 内存** | 否 | 不变 | ✗ |
| `v.emplace_back(args...)` | 必要时扩容 | 是（用 args 直接构造） | +1 | ✗ |
| `v.push_back(prototype)` | 必要时扩容 | 是（拷贝/移动） | +1 | ✗ |
| `vector<T> v(n);` | 是 | **默认构造 n 个** | n | **✓ 必须** |
| `v.resize(n)` | 必要时扩容 | **默认构造**新元素 | n | **✓ 必须** |
| `v.resize(n, prototype)` | 必要时扩容 | 拷贝构造 | n | ✗ |

### 3.2 推荐写法

LRU 池初始化：每个槽 `next/prev` 都不同，所以最干净的是：

```cpp
pool.reserve(x + 2);
for (int i = 0; i < x; ++i) {
    pool.emplace_back(i + 1, i - 1);   // 直接 in-place 构造
}
pool.emplace_back(tail_sentinel, head_sentinel);   // head sentinel
pool.emplace_back(tail_sentinel, head_sentinel);   // tail sentinel
```

避免：

- `pool.emplace_back(Node(i+1, i-1))` —— 多构造一个临时 Node，然后再移动一次；
- `pool.resize(x, Node(0, 0))` 再循环改字段 —— 拷贝一次 + 写一次。

---

## 四、成员初始化

### 4.1 初始化列表 vs 函数体

```cpp
class Foo {
    const int x;
    int y;
public:
    Foo(int a, int b)
        : x(a),       // ← 初始化（initialization）
          y(b)        // ← 初始化
    {
        // 函数体：此时成员已"出生"，这里写 x = ? 是赋值（assignment），编译失败
    }
};
```

关键概念：

| | 时机 | const 能用？ |
|---|---|---|
| 初始化（initializer list） | 对象诞生的那一刻 | ✓ const 必须在这里 |
| 赋值（function body） | 对象已存在之后 | ✗ const 禁止 |

### 4.2 `const` 成员

**必须**在初始化列表里设值。LRU 里的好用例：

```cpp
private:
    const int32_t _capacity;
    const int32_t _head_sentinel;
    const int32_t _tail_sentinel;
```

构造后再也不变，加 `const` 防误改，编译器也能更激进优化（常量传播）。

### 4.3 初始化顺序的隐藏坑

初始化列表的**执行顺序按成员声明顺序**，**不是你写的顺序**：

```cpp
class Foo {
    int a;
    int b;
public:
    Foo() : b(1), a(b) {}   // 实际先 a 再 b，a 拿到 b 的垃圾值！
};
```

规则：**让成员声明顺序反映依赖关系**，初始化列表按声明顺序写，一目了然。

### 4.4 函数体里能用之前初始化的成员吗

能。函数体执行时初始化列表已经全部跑完，所有成员已就绪：

```cpp
explicit lrucache_base(int32_t x) : _capacity(x), ... {
    // 这里可以直接用 _capacity，不必再用参数 x
    pool.reserve(_capacity + 2);
}
```

---

## 五、所有权与智能指针

### 5.1 三种所有权模型

| 模式 | 签名 | 谁负责 delete |
|---|---|---|
| cache 拥有 | `push(K, V)` | cache 在淘汰/析构时 |
| 用户拥有 | `push(K, V*)` | 用户 |
| 共享拥有 | `push(K, shared_ptr<V>)` | refcount 归零时自动 |

裸指针参数**不携带所有权信息**，是 ownership 模糊的源头。

### 5.2 选 `shared_ptr<V>` 的理由

LRU 场景下：
- cache 可以**自由淘汰**而不用顾忌用户手里的指针；
- 用户拿到 `get(key)` 后即使 cache 淘汰了，他手里那份**仍然安全**（refcount 撑着）；
- "shared_ptr 自带 null 状态"，`get` 返回 `shared_ptr<V>` 就够，**不要再裹 optional**。

代价：refcount 是原子操作，比裸指针拷贝贵；对绝大多数 LRU 场景可忽略。

### 5.3 控制块（control block）

```
sp1 sp2 sp3                    ← 多个 shared_ptr 变量
  \  |  /
   ┌─────────────────────────┐
   │ control block           │
   │   refcount(strong): 3   │  ← shared 计数
   │   weak_count: 0         │  ← weak 计数
   │   deleter               │
   │   ptr → V               │
   └─────────────────────────┘
                ↓
            ┌───────┐
            │   V   │
            └───────┘
```

- 每个 `sp` 变量独立析构，析构时**原子地 `--refcount`**；
- refcount=0 → 销毁 V；
- refcount=0 且 weak_count=0 → 销毁控制块本身。

**这是 RAII + 引用计数，不是 GC**：完全确定性，没有扫描线程，没有 stop-the-world。

### 5.4 裸指针与 shared_ptr 互转的坑

```cpp
V* raw = new V;
std::shared_ptr<V> sp1(raw);
std::shared_ptr<V> sp2(raw);   // ★ 致命：两个独立控制块，双 delete
```

**规则**：
- 一个对象一旦进入 shared_ptr 世界，**只能通过 shared_ptr 传递**；
- 从裸指针构造 shared_ptr **必须只 wrap 一次**；
- 推荐**从 `std::make_shared<V>(...)` 开始**：少一次堆分配（对象和控制块合并），完全避免裸指针存在。

### 5.5 参数传递语义

```cpp
// (1) 按值取：sink 语义，函数会"存下"它
void push(std::shared_ptr<V> p);
push(sp);              // 拷贝构造：refcount +1
push(std::move(sp));   // 移动构造：refcount 不变，sp 变空

// (2) const 引用：只看不存
void peek(const std::shared_ptr<V>& p);
peek(sp);              // 不动 refcount

// (3) 右值引用：强制要求 rvalue
void sink(std::shared_ptr<V>&& p);

// (4) 拿裸指针看一眼
void inspect(const V* v);
inspect(sp.get());
```

**Sink 参数惯用法**（C++ Core Guidelines）：

```cpp
void push(const K& key, std::shared_ptr<V> value) {
    // ...
    pool[idx].value = std::move(value);   // 内部 move 进存储位置
}
```

- 调用方 `push(k, sp)` → 拷贝构造形参 +1，move 进 Node 不变。**净 +1**（合理：cache 多一份引用）；
- 调用方 `push(k, std::move(sp))` → 移动构造形参不变，move 进 Node 不变。**净 0 次原子操作**。

一份代码同时支持两种调用风格。

### 5.6 循环引用与 `weak_ptr`

```cpp
struct Node {
    std::shared_ptr<Node> next;
};
auto a = std::make_shared<Node>();
auto b = std::make_shared<Node>();
a->next = b;   // b.refcount = 2
b->next = a;   // a.refcount = 2
// 离开作用域：局部 a/b 析构各 -1，refcount 都停在 1
// → 互相撑着对方活着 → 永久泄漏
```

为什么 `a` 局部析构时不传递销毁 `a->next`？因为：

> `a->next` 是堆上对象 A 的**成员**，A 的成员只在 A 被销毁时才析构。A 被销毁的条件是 A.refcount=0。循环让这个条件永不成立，销毁链条**根本没启动**。

**解法：环里至少一条边换成 `weak_ptr`**：

- `weak_ptr` 不参与 refcount（强引用计数）；
- 不能直接解引用，要 `.lock()` 拿一个 `shared_ptr`（对象死了返回空）；
- 控制块里有独立的 `weak_count`，weak_ptr 让控制块活着但不让 V 活着。

判定原则：
- **拥有关系**（必须让被指对象活着）→ `shared_ptr`；
- **观察关系**（"知道它在那里，但不负责它的生死"）→ `weak_ptr`；
- **临时使用**（栈上传递不存）→ 裸指针/引用。

### 5.7 `shared_ptr` 的 null 与 `optional` 的 null

| | `optional<T>` | `shared_ptr<T>` |
|---|---|---|
| 判空 | `if (opt) / has_value()` | `if (sp) / sp == nullptr` |
| 存储 | T 内嵌在 optional 里 | T 在堆上，sp 只是指针+控制块引用 |
| 拷贝 | 拷贝 T | refcount +1 |
| 多方共享 | 不支持 | 核心能力 |

**`optional<shared_ptr<T>>` 是冗余**：两种"空"语义重复。`get` 返回 `shared_ptr<V>` 即可，nullptr 表示没命中。

### 5.8 `reset()`

```cpp
sp.reset();    // 等价于 sp = nullptr，最直接
```

干三件事：
1. 若管着对象：原子 `--refcount`；
2. 若 refcount=0：调 deleter 销毁 V，必要时销毁控制块；
3. 自己变空。

**LRU `erase` 里要主动 `reset(value)`**：否则旧 shared_ptr 滞留在槽里，等下次复用才会真正释放。对持有重资源的 V（文件句柄、大块内存），这是"生命周期泄漏"。

```cpp
void eraseByIdx(int32_t idx) {
    pool[pool[idx].prev].next = pool[idx].next;
    pool[pool[idx].next].prev = pool[idx].prev;
    pool[idx].value.reset();          // ★ 立即释放 V
    pool[idx].next = _head_free;
    _head_free = idx;
    key2idx.erase(pool[idx].key);
}
```

---

## 六、`unordered_map` 存在性检查

| 写法 | 行为 | 用途 |
|---|---|---|
| `m.find(k) != m.end()` | 一次查找返回迭代器 | **同时要"存在性"和"值"** |
| `m.contains(k)` (C++20) | 一次查找返回 bool | **只判存在** |
| `m.count(k)` | 0 或 1（唯一键） | C++20 前替代 `contains` |
| `m[k]` | **找不到会插入默认值** | **绝不能用于探测** |

### 反模式

```cpp
if (m[key] != 0) { ... }    // ✗ 副作用：偷偷插入 m[key]=0
```

### 推荐：一次查找拿迭代器

```cpp
auto it = key2idx.find(key);
if (it == key2idx.end()) return nullptr;
int32_t idx = it->second;
// 用 idx 干活
```

避免 `contains` + `[]` 的**双查找**。

---

## 七、错误处理：`assert` vs 异常

### 7.1 `assert(expr)`

- `#include <cassert>`；
- Debug 编译：表达式为 false 时 `abort` 并打印 `file:line`；
- Release 编译（定义了 `NDEBUG`）：**完全消失**，零运行时开销。

```cpp
#ifdef NDEBUG
    #define assert(expr) ((void)0)
#else
    #define assert(expr) /* 真检查 */
#endif
```

**副作用绝不能写进 assert**：

```cpp
assert(initialize_something());   // ✗ release 下不会执行
```

### 7.2 选择标准

| | assert | exception |
|---|---|---|
| 用于 | **程序员的不变量**（理论上不会触发） | **可能发生的错误**（用户输入、IO 失败） |
| Release | 消失 | 仍然有效 |

LRU 里的典型用法：

```cpp
// 构造时校验容量 —— 用户传错了，是运行时错误
explicit lrucache_base(int32_t x) : ... {
    if (x <= 0) throw std::invalid_argument("capacity must be positive");
}

// 内部 idx 合法性 —— 写错了才会触发，是不变量
void eraseByIdx(int32_t idx) {
    assert(idx >= 0 && idx < _capacity && "idx must be a real node");
    // ...
}
```

### 7.3 用 `&& "msg"` 给 assert 带上下文

assert 默认的报错信息很干（只有表达式文本 + 文件行号）：

```cpp
assert(idx >= 0 && idx < _capacity);
```

触发时输出大概是：

```
assertion failed: idx >= 0 && idx < _capacity, file lru_base.hpp, line 80
```

读到的人还得回去翻代码才能猜"为什么这条不变量重要"。**惯用法**是用 `&&` 拼一个字符串字面量上去：

```cpp
assert(idx >= 0 && idx < _capacity && "idx must point to a real node, not a sentinel");
```

触发时多出一行你写的提示：

```
assertion failed: idx >= 0 && idx < _capacity && "idx must point to a real node, not a sentinel", file lru_base.hpp, line 80
```

#### 原理

- C++ 里**字符串字面量是非空指针**，转 bool 永远是 `true`；
- `expr && true` 的值等于 `expr`，所以**判断结果不变**；
- 但表达式文本里多了那个字符串，`assert` 宏会把整条表达式 stringify 输出 —— 你的人话说明就被打出来了。

#### 用法注意

```cpp
// ✓ 字符串放最后，符合直觉
assert(p != nullptr && "node must be allocated");

// ✓ 多个条件都写明白
assert(_head_free != _head_sentinel && _head_free != _tail_sentinel && "free list points to sentinel");

// ✗ 别写成或：字符串永远是 true，整条断言变成必过
assert(p != nullptr || "msg");   // BUG：永真，等于关了

// ✗ 不要用 std::string、不要拼接：assert 需要编译期常量串
assert(cond && std::string("oops"));   // 编译失败
assert(cond && ("err: " + name));      // 编译失败
```

#### 跟异常的对照

| | assert + `&& "msg"` | exception |
|---|---|---|
| 信息来源 | 编译期字面量 | 运行时构造的字符串（`std::string("...") + ...`） |
| 能带运行时变量？ | ✗ 字符串必须是编译期常量 | ✓ `what()` 可拼任意内容 |
| Release 是否生效 | 否（NDEBUG 时消失） | 是 |
| 适合 | 调试期解释不变量 | 用户/外部错误的可读说明 |

想在 abort 信息里塞运行时变量（比如"idx=42 超出 _capacity=10"），assert 做不到，得用：

```cpp
if (idx >= _capacity) {
    throw std::out_of_range("idx=" + std::to_string(idx)
                            + " >= capacity=" + std::to_string(_capacity));
}
```

或者写一个项目自己的 `CHECK` 宏（参考 glog / Abseil），可以同时做"运行时判断 + 流式拼接信息"。

### 7.4 未捕获异常的输出

C++ 标准只规定"调 `std::terminate` → `std::abort`"，**不保证**打印什么。但常见运行时都给了"善意"：

- GCC/libstdc++：自动打类型名 + `what()`：
  ```
  terminate called after throwing an instance of 'std::invalid_argument'
    what():  capacity must be positive
  ```

想要稳定的输出：

```cpp
int main() {
    try {
        run();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Unknown exception\n";
        return 1;
    }
}
```

测试框架（GTest / Catch2）已经替你做了这件事，`EXPECT_THROW` 也会打 `what()`。

---

## 八、设计经验沉淀

### 8.1 实现时容易踩的坑

1. **`push` 不处理"key 已存在"** → 旧槽变"幽灵节点"，hash 表查不到但占着 used 链表一格，容量持续偏小；
2. **`erase` 三件事漏一**：链表摘除、还槽给 free list、删 hash 表项 —— 漏哪个都会埋雷；
3. **不 `reset` value** → 旧 V 滞留到槽被复用；
4. **`eraseFromTail` 用 O(N) 扫尾** → 抹掉了 LRU 的 O(1) 卖点（双哨兵后用 `pool[tail].prev` O(1) 拿到）；
5. **`get` 用 `erase + push` 模拟"移到头"** → 触发 hash 表/free list/shared_ptr 多次抖动，提取 `moveToHead(idx)` 简洁又高效；
6. **每次 `pool[next]` 没判哨兵** → 单 sentinel 方案下越界 UB；双哨兵直接消掉这类分支。

### 8.2 接口设计的几个小决定

- 内部辅助：`eraseByIdx(int32_t)` / `moveToHead(int32_t)` 走下标路径，避开 hash 查询；
- 公开 `erase(const K&)` 走 key 路径（用户语义），内部转发到 `eraseByIdx`；
- `get` 返回 `shared_ptr<V>`，nullptr 即未命中。

### 8.3 命名一致性

LRU 这版命名两种风格混用了：`_head_free` (前缀下划线) vs `used_head_sentinel_` (后缀下划线)。选一种贯彻（C++ Core Guidelines 偏好后缀）。**注意**：`_大写字母` 和 `__双下划线` 是保留命名，不能用。

---

## 九、下一步可以做的扩展

1. **线程安全版本**：`std::mutex` 大锁 → 分片锁（Sharded LRU）→ 无锁尝试；
2. **TTL**：每个 entry 加过期时间，惰性删除或定期清理；
3. **统计**：hit / miss / eviction 计数，对调参有用；
4. **批量接口**：`get_multi` / `push_multi` 摊销 hash 开销；
5. **测试**：边界（容量 1、容量满后 push、重复 key push、不存在 key get）、生命周期（shared_ptr 在 evict 后是否真的释放）、性能基准。

---

## 附：最终结构速览

```cpp
template<typename K, typename V>
class lrucache_base {
public:
    explicit lrucache_base(int32_t capacity);    // throws on capacity <= 0
    ~lrucache_base() = default;                  // 智能指针自动收尾

    void push(const K& key, std::shared_ptr<V> value);   // sink by value
    std::shared_ptr<V> get(const K& key);                 // nullptr = miss
    void erase(const K& key);

private:
    struct Node {
        K key;
        std::shared_ptr<V> value;
        int32_t next;
        int32_t prev;
        Node(int32_t n, int32_t p);
    };

    const int32_t _capacity;
    const int32_t _head_sentinel;     // = _capacity
    const int32_t _tail_sentinel;     // = _capacity + 1
    int32_t _head_free;

    std::vector<Node> pool;           // size = _capacity + 2
    std::unordered_map<K, int32_t> key2idx;

    void eraseByIdx(int32_t idx);
    void moveToHead(int32_t idx);
};
```
