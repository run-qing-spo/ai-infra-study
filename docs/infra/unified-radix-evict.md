# Eviction、LRU 与 lock_ref 的完整生命周期

## 4.0 本轮要解决的问题

上一轮讲到两种 KV 所有权变化：

```text
match：
    树仍然拥有 KV
    请求暂时取得使用权

insert：
    请求把新计算的 KV 转交给树
```

这自然引出两个问题：

1. 请求正在使用树中的 KV 时，怎样防止它被驱逐？
2. 请求使用结束后，缓存怎样选择应该回收哪些 KV？

对应的机制分别是：

```text
lock_ref
    表示某个缓存 component 当前是否被请求或异步操作保护

eviction
    在显存或 Host 内存不足时，从未被保护的缓存中选择受害者
```

看起来这可能是一套统一机制，但 Unified Cache 实际上同时存在：

- 多种 component；
- device 和 host 两个层级；
- 两套不同的驱逐顺序；
- 路径锁、窗口锁和单 component pin；
- component 之间的级联驱逐关系。

因此不能只看一个 `lock_ref` 数字，也不能把 eviction 简化成“找最旧节点然后删除”。

---

## 4.1 一个节点何时具备可驱逐资格

先区分两个概念。

### 缓存拥有

一段 KV 已经通过 insert 转交给树，意味着它不再由某个请求独占。

### 可以驱逐

树拥有这段 KV，并不代表它现在就可以释放。

如果某个正在执行的请求通过该节点复用 KV，节点对应的 component 会带有：

```python
lock_ref > 0
```

如果 Host 副本正在被 load-back、write-through 或请求使用，还可能受到：

```python
host_lock_ref > 0
```

的保护。

因此一段缓存 KV 的基本状态变化是：

```text
请求 insert KV
  → 树拥有 KV
  → 当前无人使用：evictable
  → 请求命中并加锁：protected
  → 请求结束并解锁：重新 evictable
  → 内存不足时被 eviction 回收
```

TreeCore 同时维护容量统计：

```python
component_evictable_size_
component_protected_size_
```

第一次加锁时，相关容量从 evictable 移到 protected；最后一次解锁时再移回来。

`lock_ref` 不只是阻止删除的布尔值，它还决定一段缓存容量应该被计入哪一类。

---

## 4.2 请求怎样取得树节点的保护权

在 Scheduler 完成匹配并准备让请求运行时，会执行：

```python
receipt = tree_cache.inc_lock_ref(req.last_node)
req.lock_receipt = receipt.to_dec_params()
```

在 Unified Tree 中，它最终进入：

```python
UnifiedTreeCore.inc_lock_ref(node_id, skip_lock_components)
```

TreeCore 不直接统一增加一个节点级计数，而是依次调用每个 component：

```python
component.acquire_component_lock(...)
```

不同 component 可以采取不同的锁定范围。

例如：

- FULL 通常从命中节点向 root 锁住整条路径；
- SWA 只需要保护覆盖尾部滑动窗口的连续路径片段；
- Mamba 主要保护匹配边界上的状态；
- 某些 component 可以通过 `skip_lock_components` 跳过。

因此“锁住一个节点”只是 Scheduler 看到的抽象。TreeCore 内部真正发生的是：

```text
以该节点为锚点，
分别按照各 component 的语义，
锁住一组可能不同的节点和资源。
```

---

## 4.3 为什么必须保存 lock receipt

`inc_lock_ref()` 返回的 receipt 会记录：

```text
node_id
本次跳过了哪些 component
SWA 锁对应的 UUID
Host SWA 锁对应的 UUID
其他 component 所需的释放信息
```

释放时不能只说：

```text
“把当前节点解锁”
```

而必须重放本次实际取得的保护权。

TreeCore 在释放前执行：

```python
_assert_receipt_anchor(node, params)
```

核心断言是：

```python
params.node_id == node.id
```

也就是说，从节点 A 获得的 receipt 只能释放节点 A 对应的锁。

如果 receipt 丢失，通常结果是锁没有被释放，形成容量泄漏；  
如果把 receipt 错配给另一节点，则可能释放其他请求仍在使用的 KV。

代码选择直接断言失败，阻止后一种更危险的静默破坏。

---

## 4.4 四组锁操作分别保护什么

### 4.4.1 `inc_lock_ref` / `dec_lock_ref`

这是请求复用 Prefix Cache 时的主锁。

```text
获取：
    inc_lock_ref(node_id, skip_lock_components)

释放：
    dec_lock_ref(node_id, receipt, skip_swa=False)
```

它会按每个 component 的规则保护对应资源。

正常请求的基本生命周期是：

```text
match
  → inc_lock_ref
  → 请求运行
  → cache_finished_req / retraction / abort
  → dec_lock_ref
```

### 4.4.2 `inc_host_lock_ref` / `dec_host_lock_ref`

这一组操作的是：

```python
host_lock_ref
```

用于保护 Host 层数据，例如 Host KV 正在被异步 load-back 使用时。

Device KV 和 Host KV 是两份不同资源，因此：

```text
device lock_ref == 0
```

并不自动表示 Host 副本可以回收。

Host eviction 还必须检查：

```text
host_lock_ref
write_through_pending_id
load_back_pending_id
```

### 4.4.3 `inc_full_pin` / `dec_full_pin`

这组 API 只保护 FULL device KV。

它适用于这样一种情况：

```text
系统只需要保证 Full KV 路径继续存在，
但不希望重新建立或延长 SWA 窗口锁。
```

`inc_full_pin()` 会沿 root path 增加 FULL 的保护，但不会取得一份完整的多 component receipt。

它和普通请求锁不是完全等价的。

### 4.4.4 `dec_swa_lock_only`

它没有对应的独立 `inc_swa_lock_only()`。

原因是它释放的不是一把单独获取的锁，而是提前释放：

```text
此前 inc_lock_ref() 获得的整体锁中的 SWA 部分
```

它还会连带释放驱逐优先级严格低于 SWA 的 component，目前典型是 Mamba。

后续完整释放时必须调用：

```python
dec_lock_ref(..., skip_swa=True)
```

让正常解锁过程跳过已经提前释放的：

```text
SWA
以及 SWA 以下优先级的 component
```

完整时序是：

```text
inc_lock_ref()
  ├── FULL 锁
  ├── SWA 锁
  └── Mamba 锁

请求的 SWA 窗口不再需要旧前缀
  → dec_swa_lock_only()
      ├── 提前释放 SWA
      └── 提前释放 Mamba

请求最终结束
  → dec_lock_ref(skip_swa=True)
      └── 只释放剩余的 FULL 等较高层锁
```

这正是上一轮发现的 Scheduler 契约外调用：

```python
tree_cache.dec_swa_lock_only(...)
```

它不是偶然的辅助函数，而是完整锁生命周期中的“部分提前释放”步骤。

---

## 4.5 为什么 SWA 锁可以比 FULL 更早释放

FULL KV 表示从 root 到匹配点的完整历史。

只要请求仍在使用这个缓存前缀，相关 FULL 路径通常必须继续存在。

SWA 不同。它只关心当前位置之前的一段滑动窗口。

随着请求继续 decode，早期 SWA 状态会逐渐移出窗口：

```text
旧时：
    [A B C D E] 是有效窗口

继续生成后：
          [D E F G H] 才是有效窗口
```

此时请求仍可能需要 FULL 前缀，但已经不再需要 A、B、C 对应的 SWA 保护。

所以 SWA 的锁生命周期可以短于请求整体的 FULL 锁生命周期。这是 `dec_swa_lock_only()` 存在的根本原因。

---

## 4.6 `delta` 为什么目前不能用来判断容量变化

接口类型中仍有：

```python
IncLockRefResult.delta
DecLockRefResult.delta
```

旧式 RadixCache 会用 delta 表示锁操作引起的 protected/evictable 容量变化。

但 Unified Tree 中，各 component 分别管理自己的路径和容量。当前 `dec_lock_ref()` 末尾明确有 TODO：

```text
delta 尚未从各 component 聚合
当前也没有调用方使用它
```

所以 Unified 路径返回的：

```python
IncLockRefResult.delta
DecLockRefResult.delta
```

通常是 `None`。

在 Unified Cache 中判断容量变化，应查看 component 级容量统计，不能依赖这个字段。

从接口审计角度看，它目前属于为兼容旧实现而保留、但在 Unified 路径中没有实际语义的字段。

---

## 4.7 Unified Cache 实际存在两套驱逐顺序

这是本轮最重要的结构性结论。

虽然所有 component 共享同一棵 token 树，但 FULL 与辅助 component 并不共享同一套受害者排序机制。

### FULL 的驱逐顺序

FULL 每次开始 device eviction 时，从：

```python
evictable_device_leaves
```

临时建立一个 heap。

堆的排序键大致是：

```text
(
    session_ref > 0,
    session_ref,
    eviction_strategy.get_priority(node),
)
```

前两个字段让没有 session 引用的节点优先被驱逐。  
第三个字段由配置的 eviction policy 决定。

FULL 的工作过程是：

```text
收集当前可驱逐叶节点
  → 建堆
  → 弹出优先级最低的叶节点
  → 跳过已经失效的堆条目
  → 驱逐该节点
  → 如果父节点因此成为可驱逐叶节点，将父节点压入堆
  → 继续
```

堆不是常驻结构，而是在一次 eviction walk 开始时，根据当时的叶集合重新建立。

### SWA、Mamba 等辅助 component 的驱逐顺序

辅助 component 使用节点中常驻的侵入式：

```text
UnifiedLRUList
```

驱逐时从 LRU 端开始，通过 cursor 向前推进：

```text
cursor_begin()
  → cursor_next()
  → 检查节点是否可驱逐
  → 遇到锁定状态则跳过或保留
  → 驱逐
  → cursor_end()
```

它们不调用 FULL 的 `eviction_strategy.get_priority(node)`。

所以两套顺序是：

| 对象 | 排序载体 | 实际策略 |
|---|---|---|
| FULL | 每轮临时构建的 heap | 可配置 eviction policy |
| SWA/Mamba 等 | 常驻侵入式链表 | 固定 LRU |

---

## 4.8 `--radix-eviction-policy` 实际控制范围

`evict_policy.py` 提供的八种策略只影响 FULL，例如：

- LRU；
- LFU；
- FIFO；
- FILO；
- SLRU；
- TLRU；
- 其他组合策略。

无论参数设置为哪一种，SWA 和 Mamba 仍按它们自己的 `UnifiedLRUList` 顺序驱逐。

所以：

```text
--radix-eviction-policy=lfu
```

并不表示整个 Unified Cache 都采用 LFU，而是：

```text
FULL：LFU
SWA：LRU
Mamba：LRU
```

同理，TLRU 只作用于 FULL 的 heap priority。

轮次 1 中看到的 registry 守卫：

```text
TLRU 只允许 UnifiedRadixCache
```

只能保证选择了支持 TLRU 的缓存实现，却不意味着 UnifiedRadixCache 内所有 component 都使用 TLRU。

更准确的配置语义应是：

```text
radix eviction policy
= Unified Cache 中 FULL component 的树叶驱逐策略
```

---

## 4.9 为什么 FULL 从“可驱逐叶节点”开始

Radix Tree 的内部节点连接多个仍有价值的后缀。

例如：

```text
        A
       / \
      B   C
```

如果直接删除 A，B 和 C 就会失去公共前缀和父节点。

因此 FULL 的物理节点删除通常从叶节点开始：

```text
先删 B 或 C
  → 当 A 不再有子节点时
  → A 才可能成为新的可驱逐叶节点
```

这就是 FULL 驱逐后把 parent 重新压回 heap 的原因。

不过“从叶开始”不代表内部节点上的某个辅助 component 不能单独被 tombstone。SWA、Mamba 可以在保留树拓扑的情况下释放自己的数据。

所以需要区分：

```text
删除整个树节点
```

和：

```text
只驱逐该节点上的一个 component
```

---

## 4.10 component 驱逐优先级不是受害者排序

代码中还有一组：

```python
component.eviction_priority(is_leaf)
```

它容易和 LRU、LFU 等策略混淆。

两者解决的是不同问题：

```text
LRU/LFU/TLRU：
    先选择哪个节点作为受害者

component eviction priority：
    已经决定驱逐某个节点上的 component 后，
    还必须连带驱逐哪些其他 component
```

component 的级联优先级是：

### 叶节点

```text
FULL  = 0
SWA   = 0
Mamba = 0
```

在叶节点上全部坍缩为同一优先级。

因为一旦整个叶节点即将删除，留住其中某个孤立 component 没有意义。

驱逐其中任何一个，都可能级联到其他 component，最终删除整个节点。

### 内部节点

```text
FULL  = 2
SWA   = 1
Mamba = 0
```

数值越高，越晚被连带驱逐。

---

## 4.11 为什么内部节点上 FULL > SWA > Mamba

### FULL 的价值最高

FULL 是树路径成立的基础，也是普通 Prefix Cache 命中的核心数据。

如果内部节点的 FULL 被驱逐，则依赖这段路径的辅助 component 通常也不能再形成完整命中，因此会级联：

```text
驱逐 FULL
  → 连带驱逐 SWA
  → 连带驱逐 Mamba
```

### SWA 高于 Mamba

SWA 是路径数据。

滑动窗口命中要求从 root 到匹配边界之间，相关窗口范围具有连续覆盖。

例如：

```text
A → B → C → D → E
```

即使最终在 E 处匹配，窗口也可能需要 C、D、E 上连续的 SWA 状态。

Mamba 状态则主要在匹配边界节点有意义。内部节点上的 Mamba 不负责维持一条连续的可达路径。

因此：

```text
驱逐内部节点的 Mamba：
    不需要连带驱逐 SWA 或 FULL

驱逐内部节点的 SWA：
    连带驱逐 Mamba

驱逐内部节点的 FULL：
    连带驱逐 SWA 和 Mamba
```

这就是优先级：

```text
FULL 2 > SWA 1 > Mamba 0
```

背后的语义，而不只是人为规定的数字。

---

## 4.12 级联驱逐为什么需要两次比较优先级

`_should_cascade_evict_component()` 会先根据当前节点是否为叶节点比较优先级。

第一层判断是：

```text
目标 component 的当前优先级
是否低于或等于触发者
```

叶节点上所有 component 都是 0，因此都会成为候选。

但这会带来一个歧义。

假设在叶节点上触发 Mamba eviction：

```text
表面优先级：
FULL = SWA = Mamba = 0
```

FULL 和 SWA 看起来都应该被连带驱逐。

然而某个 component 可能仍然有合法锁。叶节点优先级的“全部归零”只是为了表达：

```text
如果叶节点可以整体删除，所有 component 一起消失
```

它不应覆盖 component 的真实保护关系。

因此代码再次使用内部节点优先级复判：

```text
真实优先级 ≥ trigger 的 component
    如果有锁，这是合法 pin
    不进行级联驱逐

真实优先级严格低于 trigger 的 component
    理论上不应在高层已被驱逐后继续被锁住
    如果仍有锁，说明生命周期失配
    assert 失败
```

这个双重判断区分了两种外观相同的情况：

```text
较高价值 component 有锁：
    合法保护，应保留

较低价值 component 在上层依赖消失后仍有锁：
    资源关系断裂，应视为 bug
```

---

## 4.13 驱逐不是立即调用 allocator.free

component 的 `evict_component()` 不直接释放物理内存，而是把待释放的 indices 收集到：

```python
device_frees
host_frees
```

然后由 UnifiedRadixCache 或 Controller 统一排空。

原因与上一轮 insert action 类似：

- TreeCore 负责元数据和所有权判断；
- allocator/controller 负责真实内存操作；
- SWA 的释放可能仍需要读取 FULL value；
- 多个 component 的释放需要按安全顺序组织；
- shared page 可能需要合并后再释放。

特别是 FULL 的 `value = None` 可能被延迟到 SWA 处理完成之后，因为 SWA 的 free 逻辑还要借助 FULL 的索引。

因此正确顺序是：

```text
决定哪些 component 被驱逐
  → 各 component 收集待释放资源
  → 完成依赖 FULL value 的辅助清理
  → 把 FULL 标记为 tombstone
  → 更新树和可驱逐集合
  → 外层把 frees 交给 allocator
```

---

## 4.14 Host duplicate 回收与节点驱逐的区别

当 FULL 同时存在于 device 和 host：

```text
device 有一份
host 也有一份
```

Host 副本是重复副本，可以优先回收，而不影响设备命中。

这不是删除树节点，也不是驱逐 device FULL，只是：

```text
释放 FULL 的 Host 副本
```

回收前会实时检查：

```text
device FULL 仍存在
host FULL 仍存在
write_through_pending_id is None
load_back_pending_id is None
host_lock_ref == 0
```

所以即使 `full_host_duplicates` 中存在懒失效条目，也不会错误释放：

- 正在写入的 Host 副本；
- 正在被 load-back 读取的 Host 副本；
- 被 Host 锁保护的副本。

遍历时只收集待删除 ID，遍历结束后统一 `pop`，避免迭代过程中修改 dict。

真正删除树叶时，`_remove_leaf_from_parent()` 也会主动把节点从重复追踪表移除。

因此轮次 3 留下的 duplicate 问题已经闭合：

```text
索引允许暂时陈旧
但使用前实时复检
节点删除时主动清理
```

---

## 4.15 split 对两套驱逐顺序的影响不同

轮次 3 注意到 `_split_node()` 一方面声称：

```text
split 不访问原后缀
```

另一方面又刷新了：

```python
child.last_access_time
```

现在可以确认其影响范围。

### 对 FULL

FULL 的 heap priority 会读取 eviction strategy。

在 LRU 等依赖 `last_access_time` 的策略下，split 会让原 child 看起来更新，因此可能延后它被 FULL eviction 选中。

所以 split 对 FULL 驱逐顺序确实有影响。

### 对辅助 component

split 使用类似：

```python
insert_after(child, new_node)
```

把新公共前缀插在原 child 邻近位置，并没有把 child 移到链表 MRU 端。

因此 SWA、Mamba 的侵入式 LRU 顺序基本不受 child 时间戳刷新的影响。

### 对 FIFO/FILO

新父节点继承原 child 的：

```python
creation_time
```

因此以创建时间为依据的 FIFO/FILO 策略不会因为 split 把这条路径当作刚创建的新缓存。

总结如下：

| 驱逐方式 | split 的影响 |
|---|---|
| FULL + LRU 类时间策略 | child 被刷新，顺序可能变化 |
| FULL + FIFO/FILO | 继承 creation_time，基本中性 |
| SWA/Mamba LRU | child 链表位置不变 |

---

## 4.16 一次完整的“命中—保护—解锁—驱逐”流程

把本轮放回请求生命周期，可以得到：

```text
1. 请求 match_prefix
   找到 device prefix 和 last_node

2. 请求获准运行
   inc_lock_ref(last_node)
   各 component 按自身范围加锁

3. 请求使用缓存 KV
   对应容量计入 protected
   eviction 必须跳过这些资源

4. SWA 窗口前移
   可选：dec_swa_lock_only()
   提前释放 SWA 和更低优先级 component

5. 请求完成、抢占或取消
   dec_lock_ref(..., skip_swa=是否提前释放)
   剩余锁被释放

6. 最后一个引用消失
   对应容量从 protected 回到 evictable

7. allocator 空间不足
   UnifiedRadixCache 发起 eviction

8. 选择受害者
   FULL：按配置策略建立 heap
   辅助 component：按常驻 LRU cursor

9. component 级联判断
   决定同一节点上还应驱逐哪些状态

10. 收集 device_frees / host_frees
    更新 tombstone、LRU、叶节点集合和树结构

11. 外层将 indices 归还 allocator
```

这条路径中的核心不变量是：

```text
只要请求仍持有有效 lock receipt，
它所需的缓存 component 就不能被 eviction 回收。

一旦对应锁全部释放，
缓存可以重新成为驱逐候选，但仍由树拥有，
直到 eviction 真正把资源归还 allocator。
```

---

## 4.17 当前配置和抽象层的两个结论

### 驱逐策略配置只覆盖 FULL

当前参数名容易让人理解为全局策略，实际只控制 FULL。

后续若希望 SWA/Mamba 也支持 LFU、TLRU 等策略，需要修改 component 的受害者选择机制，而不只是 registry 守卫。

### `dec_swa_lock_only` 应进入正式接口或被封装

Scheduler 已经直接依赖它，但 `BasePrefixCache` 没有声明它。

这意味着当前的静态抽象仍不完整：

```text
Scheduler 的真实生命周期需求
>
BasePrefixCache 对外声明的生命周期协议
```

可以选择：

1. 把“部分提前释放”正式加入基类协议；
2. 由 UnifiedRadixCache 内部封装，不让 Scheduler 直接感知 SWA；
3. 增加明确的 capability API，替代直接 duck typing。

---

## 4.18 代码版本变化：registry 选择链减少一项

当前 `registry.py` 已删除：

```text
SGLANG_EXPERIMENTAL_CPP_RADIX_TREE
  → RadixCacheCpp
```

相关环境变量导入也已移除。

所以轮次 1 中记录的实现选择链应从八条修正为七条。

这也影响后续轮次 13 的对比范围：

```text
不再需要把 RadixCacheCpp 作为当前 registry 可选择实现纳入责任矩阵
```

不过仓库中某些旧实现或适配代码是否仍存在，是另一个问题；这里的结论仅指当前工厂选择路径已经不再进入该分支。

---

## 4.19 下一轮需要带着哪些认识继续

进入 `cache_unfinished_req`、`cache_finished_req` 和 component 编排时，应重点追踪：

1. insert 接收 KV 后，请求原来的 lock receipt 怎样释放？
2. 新插入节点何时变成 evictable，何时仍是 protected？
3. FULL、SWA、Mamba 分别接收请求的哪些 KV 或状态？
4. `prev_prefix_len` 与 `cache_protected_len` 怎样防止释放缓存已有前缀？
5. chunked prefill 后，旧节点锁如何转成新节点锁？
6. finished request 中，不能被树接收的尾部由谁释放？
7. rotation conflict 或 component 截断时，哪些区间仍属于请求？

下一轮的核心将从：

```text
树如何保护和驱逐自己已经拥有的 KV
```

转到：

```text
请求结束一个执行阶段时，
怎样把自己的 KV 准确地交给这些 component。
```