# Unified Radix Tree 如何匹配、拆分和接收请求的 KV

## 3.0 本轮处在请求生命周期的哪个位置

上一轮已经建立了两条主路径。

请求准入前：

```text
Req
  → match_prefix_for_req()
  → UnifiedRadixCache.match_prefix()
  → UnifiedTreeCore.match_prefix()
  → 返回可复用的 KV indices 和节点句柄
```

请求完成或完成一段 chunked prefill 后：

```text
Req 持有新计算出的 KV
  → cache_finished_req() / cache_unfinished_req()
  → UnifiedRadixCache.insert()
  → UnifiedTreeCore.begin_insert()
  → 树接收可以缓存的 KV
```

因此，本轮讨论的是同一棵树的两个方向：

```text
match：树 → 请求
       请求从树中取得已有 KV 的使用权

insert：请求 → 树
        树从请求手中接收新 KV 的所有权
```

`split` 则是这两个方向都可能触发的结构调整。

---

## 3.1 为什么 Radix Tree 节点保存“一段 token”，而不是一个 token

Unified Radix Tree 是一棵压缩前缀树。

假设树中已经缓存：

```text
[10, 20, 30, 40, 50]
```

它不需要建立五个节点。一个节点可以直接保存整段：

```text
key   = [10, 20, 30, 40, 50]
value = 这五个 token 对应的 KV indices
```

现在另一个请求查找：

```text
[10, 20, 30, 99]
```

它只匹配到原节点的前三个 token。为了让 `[10, 20, 30]` 成为可独立复用的公共前缀，树需要把原节点拆成：

```text
公共前缀节点 [10, 20, 30]
              |
              └── 原后缀节点 [40, 50]
```

以后插入新请求的剩余部分时，还可以形成另一个分支：

```text
公共前缀节点 [10, 20, 30]
              ├── [40, 50]
              └── [99]
```

这就是 `_split_node()` 存在的原因。

它不是异常修复，而是压缩 Radix Tree 建立分叉点的正常操作。

---

## 3.2 一个 UnifiedTreeNode 保存什么

### 3.2.1 树结构

每个节点包含：

```python
node.parent
node.children
node.key
```

其中：

- `key` 是从父节点到当前节点的那段 token；
- `parent` 指向父节点；
- `children` 以子节点的首个查找单位为键。

`children` 刻意使用普通 `dict`，而不是 `defaultdict`。

原因是 TreeCore 同时维护一份节点 arena：

```text
NodeId → 当前仍有效的 UnifiedTreeNode
```

如果一次错误的读取可以自动创建子节点，这个节点便可能没有经过 `_new_node()` 注册，形成“树上存在、arena 中不存在”的幽灵节点。

所以缺失的 child 必须明确报错或被显式判断，不能在读取时静默创建。

### 3.2.2 一个 token 前缀可能对应多种缓存状态

Unified Cache 不只保存 Full Attention KV，还可能保存：

- FULL KV；
- SWA，即 Sliding Window Attention 状态；
- Mamba state；
- C128 等硬件特化状态。

节点没有为每一种模型形态定义不同的 Python 类，而是使用定长数组：

```python
node.component_data[ComponentType.FULL]
node.component_data[ComponentType.SWA]
node.component_data[ComponentType.MAMBA]
...
```

`component_data` 的长度固定为 `_NUM_COMPONENT_TYPES`。  
即使当前配置只有 FULL，节点仍会分配所有 component 的槽位。

这样做的收益是：

- `ComponentType` 可以直接作为整数下标；
- 不需要频繁查字典；
- 节点布局和访问方式统一。

代价是：

- 未启用的 component 也占有槽位；
- 节点的真实状态不能只看一个统一的 `value`；
- “这个节点是否存在于设备或 Host”必须明确是对哪个 component 而言。

### 3.2.3 `backuped` 和 `evicted` 只描述 FULL

当前两个便捷属性是：

```python
node.backuped
node.evicted
```

它们的实际定义是：

```text
backuped = FULL 在 Host 上存在
evicted  = FULL 不在设备上
```

它们并不汇总 SWA、Mamba 等辅助 component 的状态。

因此可能出现：

```text
FULL：设备上存在，Host 上存在
SWA：只剩 tombstone
```

此时节点仍会被视为：

```text
evicted  = False
backuped = True
```

这对 TreeCore 的 Full-KV 主路径是合理的，但抽象边界存在歧义：

```python
BasePrefixCache.is_backuped(node)
```

名字看起来像是在询问整个节点，实际回答的只是 FULL 是否具有 Host 副本。

后续分析涉及异构 component 时，必须避免把这个结果解释成“所有状态都已经备份”。

### 3.2.4 LRU 链表直接嵌入节点

节点包含：

```python
lru_prev
lru_next
```

它们不是一对普通指针，而是长度为：

```text
component 数量 × 2
```

的数组。

每个 component 有两条独立链：

```text
device LRU
host LRU
```

因此同一个节点可以同时处在多个淘汰顺序中：

```text
FULL device LRU
FULL host LRU
SWA device LRU
SWA host LRU
Mamba device LRU
Mamba host LRU
```

这是侵入式链表：前后指针直接存在节点中，不需要为每条 LRU 额外创建包装对象。

不过 FULL 是一个特例。它的主要驱逐顺序依靠：

```python
node.last_access_time
```

辅助 component 才主要通过各自的侵入式 LRU 更新顺序。

### 3.2.5 `rotation_base` 为什么需要 Host 镜像

在 logical-page KV sharding 下，一个逻辑页属于哪个 rank，由类似下面的关系决定：

```text
owner(P) = (rotation_base + P) % shard_size
```

理论上，可以从设备上的 FULL KV indices 反推出这个 base。但读取设备 tensor 会触发设备到 Host 的同步。

而 `rotation_base` 会被分配路径频繁读取。若每次读取都产生同步，内存分配的快路径就会被阻塞。

因此节点额外保存一个 Host 侧整数镜像：

```python
node.rotation_base
```

它不是另一份独立真相，而是为避免在分配路径读取设备 tensor 而维护的控制面信息。

---

## 3.3 reset() 建立的不是空树，而是一组全局不变量

`UnifiedTreeCore.reset()` 会重建：

- 节点 arena；
- 根节点；
- 每个 component 的 device LRU；
- 每个 component 的 host LRU；
- 可驱逐叶节点集合；
- component 容量计数；
- Host 重复副本追踪表；
- 空匹配结果；
- insert 状态机。

### 3.3.1 根节点为什么永远不会被驱逐

根节点代表空前缀：

```text
key = []
```

它没有实际 token，但所有路径都从它开始。

实现没有在 eviction 中到处编写：

```python
if node is root:
    continue
```

而是在 reset 时给每个已启用 component 设置：

```python
root.component_data[ct].lock_ref = 1
```

也就是说，根节点通过普通的“被引用节点不可驱逐”规则获得永久保护。

这是一个重要设计选择：

```text
根节点的不可驱逐性
不是 eviction 的特殊分支
而是锁不变量的自然结果
```

### 3.3.2 Session 引用不保护 FULL

如果启用了 session radix cache，LRU 可以通过 `session_ref` 判断一个节点是否被 session 引用。

但是 `_session_lru_predicate()` 对 FULL 返回 `None`：

```text
FULL 不通过 session_ref 获得额外 LRU 保护
辅助 component 可以受到 session_ref 保护
```

这说明 session 引用在这里不是一把覆盖整个节点的统一锁，而是 component 级策略。

FULL 的生命周期仍主要由普通 `lock_ref`、树结构和自身 eviction 规则控制。

### 3.3.3 为什么重复 Host 副本使用插入有序 dict

某些节点的 FULL 同时存在于设备和 Host：

```text
device：有一份
host：也有一份
```

此时 Host 副本是可优先回收的冗余副本。

TreeCore 使用：

```python
full_host_duplicates: dict[NodeId, UnifiedTreeNode]
```

追踪它们。

这里依赖 Python `dict` 的插入顺序，目的不是展示顺序，而是让 tensor-parallel 的不同 rank 按相同顺序选择回收对象。

如果各 rank 选择不同节点作为受害者，分布式状态便会分叉。

代码还维护一个 42 位的 reclaim digest。每次回收都会更新摘要，并可通过类似：

```text
[digest, -digest]
```

的 int64 all-reduce 检查各 rank 是否一致。

选择 42 位是为了让后续乘法更新仍留在安全的 int64 范围内，并兼容 Rust 实现。

---

## 3.4 match_prefix 的输入和输出

请求进入 TreeCore 时，查询键已经由上层构造成 `RadixKey`：

```python
RadixKey(
    token_ids=request_tokens,
    extra_key=req.extra_key,
    cache_salt=req.cache_salt,
    limit=...,
)
```

TreeCore 会先处理：

- EAGLE bigram view；
- page alignment；
- 空 key 快速返回。

随后从 root 开始逐段匹配 child。

输出包含三个容易混淆的节点：

```text
best_match_node
best_match_device_node
last_host_node
```

它们之所以可能不同，是因为 HiCache 允许树中存在：

```text
token 前缀匹配
但 KV 只在 Host 上、不在设备上
```

### 非 HiCache 模式

只有设备匹配有意义，因此：

```text
best_match_node == best_match_device_node
```

二者同步向下推进。

### HiCache 模式

TreeCore 同时维护两组 validator：

```text
普通 validator：
    允许根据 device + host 状态判断 component 是否匹配

device validator：
    只接受当前已经在设备上的状态
```

因此可能出现：

```text
设备可立即使用到第 64 个 token
Host 上实际命中到第 128 个 token
```

这时：

```text
best_match_device_node → 64-token 节点
best_match_node        → 128-token 节点
```

上层据此分别生成：

- 可以立即放入 `req.prefix_indices` 的设备 KV；
- 需要经过 load-back 的 Host 命中长度。

---

## 3.5 为什么 match_prefix 不是纯读操作

一般会直觉地认为：

```text
match = 查询
insert = 修改
```

但压缩 Radix Tree 不完全如此。

假设树中有一个节点：

```text
child.key = [10, 20, 30, 40, 50]
```

请求查询：

```text
[10, 20, 30, 99]
```

`_match_prefix_helper()` 发现：

```python
prefix_len < len(child.key)
```

这意味着请求只匹配到了节点内部，而不是节点边界。

TreeCore 会立即调用：

```python
_split_node(child.key, child, prefix_len)
```

把公共前缀变成一个正式节点。

因此 `match_prefix()` 可能修改：

- 父子关系；
- 节点 key；
- component data 的分布；
- LRU 链；
- NodeId arena；
- 正在进行的 write-through 对应关系。

这也是 `MatchResult.cache_actions` 存在的原因。

当前 match 路径中，这类 action 的主要来源就是 split。例如某个节点正处于异步 write-through 中，split 后异步任务原先引用的单个节点必须被替换为“新公共父节点 + 后缀子节点”。

TreeCore 不能直接执行设备或 Host 数据搬运，因此只返回 action：

```text
TreeCore 修改树结构
  → MatchResult.cache_actions
  → UnifiedRadixCache._apply_cache_actions()
  → 真正执行缓存侧副作用
```

所以：

```text
match_prefix 是逻辑查询
但不是结构上的只读操作
```

---

## 3.6 match 如何更新“最近使用”信息

匹配完成后，TreeCore 会更新驱逐顺序。

### FULL

FULL 不通过 component LRU 的 `refresh_lru(MATCH_END)` 更新，而是从最终命中节点向 root 回溯：

```python
node.last_access_time = current_time
parent.last_access_time = current_time - 0.00001
grandparent.last_access_time = current_time - 0.00002
...
```

这样能够保持：

```text
root → leaf
```

方向上的时间顺序。

也就是说，越接近实际命中末端的节点越“新”。

### SWA、Mamba 等辅助 component

非 FULL component 调用：

```python
component.refresh_lru(
    LRURefreshPhase.MATCH_END,
    matched_node,
    root_node,
)
```

它们可以根据自己的有效区间、窗口和状态形态，决定应该刷新哪些节点。

因此 Unified Tree 虽然共享树拓扑，但不同 component 的驱逐顺序并不是一套统一结果。

---

## 3.7 split 到底修改了什么

假设原节点为：

```text
parent
  └── child: [A, B, C, D, E]
```

在前三个 token 处分裂后：

```text
parent
  └── new_node: [A, B, C]
        └── child: [D, E]
```

`_split_node()` 会：

1. 创建并注册 `new_node`；
2. 让它继承原 child 的父节点；
3. 把 child 改成后缀；
4. 把 child 挂到 new_node 下；
5. 拆分 hash 链；
6. 让各 component 重新分配自己的数据；
7. 继承 `rotation_base`；
8. 处理正在进行的 load-back/write-through 标识；
9. 更新 LRU 和可驱逐叶集合。

这里存在一处需要在下一轮继续验证的非对称行为。

注释说：

```text
split 不算访问原后缀
```

所以在侵入式 LRU 中：

```python
insert_after(child, new_node)
```

新公共前缀被放到 child 邻近位置，尽量保留后缀原有顺序。

但函数末尾仍执行：

```python
child.last_access_time = get_and_increase_time_counter()
```

因此：

- 辅助 component 的 LRU 顺序基本保留；
- FULL 使用的 `last_access_time` 却把 child 刷新了。

如果 FULL eviction 直接依赖这个时间戳，split 可能让并未真正命中的后缀显得更“新”。

这需要结合下一轮 eviction 的实际选取逻辑确认，暂时不能只根据注释判断其正确性。

---

## 3.8 insert 为什么不是一次普通函数调用

从上层看，请求完成后会调用：

```python
UnifiedRadixCache.insert(params)
```

但 TreeCore 不能独立完成所有工作。

原因是 insert 过程中可能需要：

- 释放设备 KV；
- 备份 KV 到 Host；
- 修改异步 write-through；
- 更新 component 的外部资源；
- 等待某项缓存动作真正落地后才能继续修改树。

TreeCore 负责元数据和拓扑；UnifiedRadixCache 及其 controller 负责真实内存和搬运。

因此 insert 被实现为可暂停的状态机：

```text
begin_insert()
  ↓
TreeCore 向下走树
  ↓
遇到 barrier，返回 actions 并暂停
  ↓
UnifiedRadixCache 执行 actions
  ↓
resume_insert()
  ↓
继续走树
  ↓
返回 InsertResult
  ↓
end_insert()
```

整个过程中只允许存在一个正在进行的 insert walk。

若 insert 尚未结束又再次进入，会触发：

```text
concurrent insert walks
或
re-entrant insert
```

所以这里的“单飞”是 TreeCore 实例级的，不是单个请求级的。

---

## 3.9 insert 的三个阶段

### WALK：沿已有路径前进

WALK 尝试复用已有节点，并处理：

- 节点内部分叉；
- 已驱逐节点复活；
- 已存在 KV 与请求 KV 的重叠；
- 重复 KV 的释放；
- hit count 和备份触发。

### COMMIT：提交新的尾部

当树中不存在下一个匹配 child 时，剩余 token 成为新叶节点。

TreeCore 在这一阶段：

- 建立尾部节点；
- 把 FULL KV 所有权交给树；
- 让 SWA、Mamba 等 component 附加自己的状态；
- 生成最终的 `last_device_node`；
- 记录各 component 实际接收的区间。

### TAIL：完成尾部维护

最后阶段负责：

- 刷新辅助 component 的 LRU；
- 判断是否需要发起 Host backup；
- 产生终结 action；
- 结束本次 insert 状态机。

---

## 3.10 哪些 action 会让 insert 暂停

TreeCore 把 action 分成两类。

### 可以延迟批量执行的 action

当前只有三个：

```text
FreeDeviceKV
FreeDeviceKVFullOnly
ReplaceWriteThroughOnNodeSplit
```

它们可以理解为：

- 释放已经确定不再被树使用的 KV；
- 只释放重复区间中的 FULL 部分；
- 更新异步 write-through 所引用的节点结构。

这些 action 发出后，不需要其结果来决定下一步如何走树，因此可以继续推进，直到下一个真正的 barrier 再批量执行。

### 必须先执行才能继续的 action

例如某些 Host backup 或 component 操作。

它们的结果会影响后续树状态，或者要求真实缓存状态先与元数据一致。因此 `_advance_insert()` 会：

```text
返回 actions
暂停 walk
等待 UnifiedRadixCache 执行
然后 resume
```

这里所谓“协程式”不是 Python `async/await`，而是显式保存 `_InsertWalkState` 的可恢复状态机。

---

## 3.11 为什么 finally 中还要调用 end_insert

UnifiedRadixCache 的驱动代码大致是：

```python
try:
    step = begin_insert(params)
    while True:
        apply(step.actions)
        if step.result is not None:
            return step.result
        step = resume_insert()
finally:
    apply(end_insert())
```

`finally` 是资源安全网。

假设 TreeCore 已经判定一段重复 KV 应被释放，并把 `FreeDeviceKV` 放入 pending actions；随后某个 component hook 抛出异常。

如果直接退出：

- TreeCore 已经修改了一部分所有权元数据；
- allocator 却没有收到 free；
- 这些 KV slot 可能永久泄漏。

`end_insert()` 会取出尚未发出的 pending actions，并清除进行中的 walk。`finally` 再确保这些 action 到达缓存执行层。

它不能保证任意异常都可以继续服务，但能避免“树已经放弃所有权、allocator 却不知道”的静默泄漏。

---

## 3.12 WALK 中的第一条分支：复活已驱逐节点

树中可能保留一个 token 节点，但其 FULL 设备 KV 已被驱逐：

```text
树拓扑：仍存在
FULL device value：None
```

这类节点仍有价值，因为：

- Host 可能还有副本；
- 后代或辅助 component 仍依赖这段拓扑；
- 新请求可能恰好重新计算了同一段 token。

当 insert 走到这个节点时，如果请求手中有新计算出的 KV，就可以用请求的 KV 复活它：

```python
_unevict_node_on_insert(...)
```

所有权变化是：

```text
插入前：
    这段新 KV 属于请求

复活后：
    FULL component 接收这段 KV
    这段 KV 转为树所有
```

随后每个辅助 component 会收到：

```python
recover_after_unevict(...)
```

因为 FULL 已经恢复，不代表 SWA 或 Mamba 自动恢复。它们可能仍是 tombstone，也可能需要从请求提供的数据中重建自己的状态。

---

## 3.13 WALK 中的第二条分支：与未驱逐节点重叠

另一种情况是树中已经存在完整设备 KV，而请求手里也带着同一 token 范围的新 KV。

例如：

```text
树已有：     token 0～63 的 KV
请求也持有：token 0～95 的 KV
```

其中前 64 个位置发生重叠。

每个 component 会通过：

```python
update_component_on_insert_overlap(...)
```

决定它对请求提供的重叠 KV 做什么。

这里的“claim ownership”不是简单地说“整个节点都归 component”。更准确的含义是：

```text