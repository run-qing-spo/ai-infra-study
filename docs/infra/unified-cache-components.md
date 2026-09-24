# 异构 Component 的状态语义

## 6.1 Unified Cache 统一了什么

Unified Radix Tree 统一的是 token 前缀的拓扑，而不是所有缓存状态的表示方式。

对于同一段 token 前缀，不同模型结构需要保存的状态并不相同：

```text
FULL
    保存每个 token 对应的完整 Attention KV

SWA
    保存滑动窗口范围内的 Attention KV

Mamba
    保存特定 token 边界上的 recurrent checkpoint
```

它们都能以 token 前缀为索引，但在以下方面存在本质差异：

- 状态是覆盖一段区间，还是只对应一个边界；
- 什么条件下可以命中；
- 节点 split 后状态应该怎样分配；
- 请求使用时要锁住整条路径、一个窗口，还是一个节点；
- 状态被驱逐后，树节点是否仍然有意义。

`TreeComponent` 的作用，就是在共享的 Radix Tree 上保留这些差异。

---

## 6.2 TreeComponent 是状态语义接口

TreeCore 负责共享机制：

```text
沿 token key 查找
创建和拆分节点
协调 insert 状态机
维护节点关系
发起锁和驱逐流程
```

Component 则负责回答与具体状态有关的问题：

```text
这个节点对当前 component 是否可以命中？
split 后 value 应该怎样处理？
insert 时请求的哪些资源被树接收？
应该锁住哪些节点？
驱逐时释放什么？
```

这些能力分布在四组 hook 中。

### 匹配

```python
create_match_validator()
finalize_match_result_in_tree_core()
finalize_match_result_in_cache()
```

用于定义 component 的命中条件，并把自己的命中信息加入统一的 `MatchResult`。

### 插入

```python
prepare_for_caching_req()
floor_cache_len()
update_component_on_insert_overlap()
recover_after_unevict()
commit_insert_component_data()
```

用于把请求态转换成树状态，并处理已有节点、tombstone 和新叶节点。

### 生命周期

```python
acquire_component_lock()
release_component_lock()
cleanup_after_caching_req()
```

用于保护请求即将使用的状态，并在请求提交或结束后完成资源结算。

### 回收

```python
evict_component()
eviction_priority()
```

用于定义 component 能否单独驱逐、需要释放哪些资源，以及与其他 component 的级联关系。

因此，Component 不是节点上一个被动的数据槽，而是该类状态完整生命周期的实现者。

---

## 6.3 ComponentData：相同容器，不同语义

每个 `UnifiedTreeNode` 都为所有 component 预留一个 `ComponentData`：

```text
ComponentData
├── value
├── host_value
├── lock_ref
├── host_lock_ref
├── session_ref
└── metadata
```

这些字段提供统一的存储框架：

- `value` 表示设备侧状态；
- `host_value` 表示 Host 侧状态；
- 两类 lock ref 控制驱逐资格；
- `session_ref` 表示会话复用价值；
- `metadata` 保存 component 特有信息。

但 `value` 的含义并不统一：

```text
FULL.value
    一段 FULL KV pool indices

SWA.value
    一段独立的 SWA pool indices

Mamba.value
    一个 checkpoint slot
```

所以“节点存在 value”只能在指定 component 的上下文中解释。

---

## 6.4 FULL：与 token key 对齐的区间状态

FULL 是整个 Radix Tree 的基础数据。

一个 FULL 节点可以理解为：

```text
node.key   = 一段 token
FULL.value = 这段 token 对应的 KV indices
```

两者长度对齐，因此 FULL 可以沿 root 到匹配节点连续拼接，形成请求的 `prefix_indices`。

### 匹配

设备匹配要求：

```python
FULL.value is not None
```

在 HiCache 下，如果设备 value 已被驱逐、但存在 Host 副本，这个节点仍可成为更深的 Host 匹配边界。

因此 FULL 同时决定：

- 可以直接使用的设备前缀；
- 需要从 Host load-back 的前缀。

### Split

节点在长度 `k` 处分裂时，FULL value 也按相同位置切开：

```text
原节点：
    key   = [A B C D E]
    value = [a b c d e]

分裂后：
    新父节点：
        key   = [A B C]
        value = [a b c]

    原子节点：
        key   = [D E]
        value = [d e]
```

这说明 FULL 是严格的区间状态。

### 锁与驱逐

请求复用 FULL 前缀时，需要保护从匹配节点到 root 的完整路径，因此 FULL 使用 path lock。

FULL 被驱逐时，基础设备路径随之消失。内部节点可以保留为 Host-backed tombstone；叶节点则可能被整体删除。

所以 FULL 在 component 依赖中具有最高优先级。

---

## 6.5 SWA：滑动窗口范围内的路径状态

SWA 也保存 KV indices，但它的有效范围不是完整前缀，而是匹配边界之前的一个滑动窗口。

它使用独立的 SWA pool，因此：

```text
SWA.value != FULL.value
```

即使两者对应相同 token，它们也可能指向不同的物理存储。

### 命中不是“当前节点有 value”就足够

SWA 匹配需要从候选边界向 root 回溯，累计出一个连续窗口。

如果中途遇到：

```text
SWA.value is None
且没有 Host 副本
```

连续性就会中断。

因此，SWA validator 是带状态的：它记录自最近缺口以来已经覆盖了多少 token，只有覆盖长度达到 `sliding_window_size`，该位置才是有效的 SWA 匹配边界。

FULL 可能命中得更深，但如果相应位置缺少完整 SWA 窗口，联合匹配必须停在更浅的位置。

### Tombstone

SWA 可以从内部节点单独驱逐，而保留 FULL：

```text
FULL.value = 有效
SWA.value  = None
```

此时节点仍存在，FULL 前缀仍可使用，但该节点的 SWA 状态形成 tombstone。

后续请求重新计算出该范围的 SWA 状态时，insert 可以恢复 tombstone，而不必重新创建整条 token 路径。

### Split 与窗口边界

如果一个节点跨越 SWA 的有效边界：

```text
节点前半段：已经滑出窗口
节点后半段：仍位于窗口内
```

SWA 会要求 TreeCore 在边界处分裂：

```text
窗口外父节点
    SWA tombstone

窗口内子节点
    重建 SWA value
```

因此，同一次 split 对 FULL 和 SWA 的含义不同：

- FULL 按长度切成两个连续区间；
- SWA 可能只在其中一侧保留真实数据。

### 锁

SWA 不需要保护从 root 开始的完整路径，只需要锁住覆盖当前窗口的连续路径段。

因此它使用 window lock，并通过 component UUID 记录这次窗口锁的边界。释放时沿祖先方向行走，直到找到对应 UUID，而不是一直走到 root。

---

## 6.6 Mamba：绑定在 token 边界上的状态快照

Mamba 与 FULL、SWA 的差异最大。

它的 value 不是逐 token KV 序列，而是：

```text
执行到某个 token 边界时的 recurrent state checkpoint
```

所以它更接近一个“点状态”。

### 匹配

Mamba 只要求最佳匹配边界节点存在可用 checkpoint：

```python
node.component_data[MAMBA].value is not None
```

它不需要像 FULL 那样拼接整条路径，也不需要像 SWA 那样累计一个连续窗口。

如果 FULL 命中得更深，但更深处没有对应的 Mamba checkpoint，联合匹配只能退回最近的有效 Mamba 边界。

### 请求使用：Copy-on-Write

请求不能直接在树中的 checkpoint 上继续执行，否则会修改共享状态。

匹配完成后，Mamba component 会为请求准备自己的 active slot，并记录：

```text
树中的 checkpoint
    → 请求 active slot 的 COW 来源
```

之后模型在请求自己的状态上继续运行，树中的 checkpoint 保持只读和可复用。

### Split

如果一个带有 Mamba checkpoint 的节点被拆成公共前缀和后缀，checkpoint 留在原后缀节点：

```text
新公共父节点：
    Mamba.value = None

原后缀节点：
    保留 Mamba checkpoint
```

因为原 checkpoint 表示执行完整个原节点后的状态，不能简单地切分，也不能声称它对应较短的新父前缀。

### 锁与驱逐

请求只会读取匹配边界上的 checkpoint，因此 Mamba 使用 point lock，只保护该节点，而不是祖先路径。

Mamba checkpoint 也可以从内部节点独立驱逐，不影响 FULL 和 SWA 的基本可用性。

---

## 6.7 三种状态如何形成共同匹配边界

Unified Tree 的匹配不能只满足 FULL。

对于启用的所有 component，TreeCore 会分别执行 validator：

```text
FULL：
    当前路径是否具有连续 KV

SWA：
    当前边界之前是否具有完整滑动窗口

Mamba：
    当前边界是否存在 checkpoint
```

`best_match_node` 表示所有 component 都认可的最深边界。

与此同时，`MatchResult` 还会保留各 component 的附加信息：

```text
device_indices
    FULL 可直接复用的设备 KV

full_kv_hit_length
    FULL 实际匹配深度

swa_branching_seqlen
    FULL 比可复用 SWA 更深时的候选分支边界

mamba_branching_seqlen
    FULL 比最近 Mamba checkpoint 更深时的候选边界

host_hit_length
swa_host_hit_length
mamba_host_hit_length
    各 component 需要从 Host 恢复的状态
```

因此，Unified Match 不是把三种命中长度简单取最小值，而是在共享树遍历中分别验证，再形成一个所有 component 均可使用的请求边界。

---

## 6.8 Insert 时的所有权语义

三个 component 对 insert 的理解也不同。

### FULL

FULL 接收一段与 token key 对齐的 KV indices。

如果树中已经存在相同前缀，请求提供的重复 KV 可以释放；如果节点的设备 FULL 已被驱逐，则请求的新 KV 可以用于复活该节点。

### SWA

SWA 只接收仍处于有效窗口内的范围。

它可能：

- 对窗口外区间保持 tombstone；
- 在窗口边界处分裂节点；
- 用请求的新 SWA 状态恢复已有 tombstone；
- 只接收一个节点的后半段。

### Mamba

Mamba 向目标边界节点提交一个 checkpoint。

如果目标节点没有 Mamba 状态，树接收这个 checkpoint；如果已有等价状态，则在 `InsertResult` 中标记：

```python
mamba_exist = True
```

请求准备出的 checkpoint 没有发生所有权转移，必须在 cleanup 中释放。

`InsertResult.adopted_ranges`、`swa_branch_inserted` 和 `mamba_exist` 的共同作用，是让请求侧知道：

```text
哪些资源已经归树所有，
哪些仍然需要自行清理。
```

---

## 6.9 三种锁对应三种读取方式

虽然 `inc_lock_ref(last_node)` 只有一个公共入口，但各 component 实际锁住的范围并不相同：

```text
FULL
    Path lock
    保护 root 到匹配边界的完整前缀

SWA
    Window lock
    保护匹配边界之前的滑动窗口

Mamba
    Point lock
    只保护匹配边界上的 checkpoint
```

因此，`last_node` 只是共同的锁锚点，不表示三种 component 锁住了完全相同的节点集合。

这也是 lock receipt 必须保存 component 特有信息的原因。统一入口负责发起操作，实际保护范围仍由 component 语义决定。

---

## 6.10 三种回收规则

不同状态的独立性决定了不同的回收方式。

| Component | 独立驱逐后的结果 |
|---|---|
| FULL | 基础设备前缀消失；可能形成 Host-backed tombstone或删除叶节点 |
| SWA | SWA value 变为 `None`，FULL 和树拓扑可以继续存在 |
| Mamba | checkpoint slot 被释放，FULL 和 SWA 可以继续存在 |

但这种独立性不是完全对称的。

内部节点上的依赖顺序是：

```text
FULL > SWA > Mamba
```

因此：

```text
驱逐 Mamba
    不要求驱逐其他 component

驱逐 SWA
    Mamba 可能失去联合匹配价值，随之驱逐

驱逐 FULL
    SWA 和 Mamba 都可能失去基础路径，随之驱逐
```

这正是轮次 4 中 component eviction priority 的语义来源。

---

## 6.11 对比总结

| 维度 | FULL | SWA | Mamba |
|---|---|---|---|
| 状态形态 | Token 区间 | 滑动窗口区间 | 边界快照 |
| 树中 value | FULL KV indices | SWA KV indices | Checkpoint slot |
| 命中条件 | 连续前缀存在 | 边界前窗口连续 | 边界节点有状态 |
| Split | 按 key 长度切分 | 按窗口边界重分配 | 状态留在后缀节点 |
| 请求使用 | 直接复用 indices | 复用窗口路径 | COW 到请求 slot |
| 锁范围 | Root 到边界 | 尾部窗口 | 单个边界节点 |
| 独立驱逐 | 基础驱逐对象 | 可以 | 可以 |

这张表说明：三个 component 唯一真正共享的是 token 前缀和节点锚点，其 value、有效范围和生命周期并不相同。

---

## 6.12 必须维持的不变量

异构状态共享同一棵树，需要保证：

1. 节点 key 可以共享，但不同 component 的 value 不能相互解释。
2. Component 状态必须与准确的 token 边界对应。
3. 联合匹配只能停在所有启用 component 都有效的位置。
4. Split 后，每个 component 必须按照自己的状态语义重新分配数据。
5. 请求只应锁住该 component 真正会读取的范围。
6. Insert 准备出的资源必须被树接收或由请求侧释放。
7. 驱逐一个 component 后，不能留下已经失去使用基础的依赖状态。
8. Host 副本和 Device 状态必须分别判断有效性与保护关系。

---

## 6.13 本轮结论

Unified Cache 的“Unified”不是把所有缓存状态变成同一种 value，而是让它们共享：

```text
token 前缀
树节点
匹配流程
插入事务
生命周期入口
```

具体状态语义仍由 Component 保留：

```text
FULL
    是与 token key 对齐的区间状态

SWA
    是围绕匹配边界的窗口路径状态

Mamba
    是绑定在特定 token 边界上的点状态
```

`TreeComponent` 因此是 Unified Tree 的语义层：TreeCore 提供统一结构，Component 决定这类状态在结构中何时有效、如何转移、怎样保护以及何时释放。