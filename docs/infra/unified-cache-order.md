# UnifiedRadixCache 的请求级提交路径

## 5.1 本轮要解决什么问题

模型执行后，新产生的 KV 暂时归请求所有。请求完成一个执行阶段时，系统需要决定这些 KV 的去向：

```text
可以复用的部分
    → 转交给 Radix Tree

已经被树中现有内容覆盖的部分
    → 作为重复 KV 释放

受页对齐或 component 约束而无法缓存的部分
    → 归还 allocator
```

如果请求已经结束，这次提交必须完成最终资源结算；如果请求尚未结束，例如处于 chunked prefill 中间阶段，提交后还必须让它继续执行。

因此，`UnifiedRadixCache` 在这里承担的不是简单的 `insert()` 包装，而是一次请求级提交事务：

> 协调请求、TreeCore、各 component、节点锁和 KV allocator，让请求持有的每项资源在提交后都有唯一且明确的归属。

---

## 5.2 两条入口与共同流水线

请求级提交有两个入口：

```python
cache_finished_req()
cache_unfinished_req()
```

前者处理已经结束的请求，后者处理仍需继续执行的请求。两条路径共享相似的整体结构：

```text
入口
  → Session 拦截
  → Disable 短路
  → Component prepare
  → 提交边界收敛
  → TreeCore insert
  → 请求级结算
  → Component cleanup
```

其中，Session 和 Disable 是入口守卫；真正的提交过程从 component prepare 开始。

### Session 拦截

两个入口首先调用：

```python
self.session.try_cache_finished_req(...)
self.session.try_cache_unfinished_req(...)
```

如果返回 `True`，说明当前请求的缓存生命周期已经由 session 层整体接管，普通提交路径不再继续执行。

这表明 session 不是 insert 之后附加的一项操作，而是可以替换整个请求级缓存流程的上层策略。

### Disable 短路

当 UnifiedRadixCache 被禁用时，两条路径的行为不同：

- Finished 请求直接释放本次负责范围内的 KV；
- Unfinished 请求不建树，只把当前请求行复制到 `req.prefix_indices`，让请求继续运行。

这也解释了：

```python
is_chunk_cache() == self.disable
```

对 Scheduler 而言，一个被禁用的 UnifiedRadixCache 表现得像 ChunkCache。这里的缓存形态由运行状态决定，而不完全由对象类型决定。

---

## 5.3 Component 如何参与提交编排

通过入口守卫后，UnifiedRadixCache 依次调用所有 component：

```python
prepare_for_caching_req(...)
```

这一轮只关注它们的统一编排语义，不展开 FULL、SWA、Mamba 各自准备了什么——具体数据语义留到轮次 6。

每个 component 可以：

- 向 `InsertParams` 写入自己的插入数据；
- 返回自己允许的缓存长度；
- 返回 `None`，表示对长度没有额外意见。

多个非 `None` 结果取最小值：

```text
effective_cache_len
    = min(各 component 给出的长度)
```

这个阶段是顺序无关的。它回答的是：

> 各 component 共同认可的最长 token 前缀是多少？

随后，每个 component 还会依次执行：

```python
effective_cache_len = comp.floor_cache_len(effective_cache_len)
```

这与前面的 `min` 语义不同：

```text
prepare_for_caching_req
    多个独立意见取共同下界

floor_cache_len
    在已有结果上依次施加合法化规则
```

第二步是链式变换，理论上与 component 注册顺序有关。如果某个 `floor_cache_len()` 不是单调且幂等的，交换 component 顺序可能改变最终结果。

这层请求级 hook 没有出现在 `BasePrefixCache` 中。基类只约定最终的 KV 区间归属，却没有声明 Unified Cache 在插入前还会让所有 component 共同协商提交边界。

---

## 5.4 Finished：完成最终所有权结算

`cache_finished_req()` 接收一个关键参数：

```python
owned_kv_len
```

结合 `cache_protected_len`，请求当前 KV 可以分成：

```text
[0, cache_protected_len)
    已经归树所有，请求只持有使用权

[cache_protected_len, owned_kv_len)
    本次调用必须完成结算

[owned_kv_len, kv_allocated_len)
    由外层 release_kv_cache() 处理
```

因此，Finished 路径的核心责任是：

> `[cache_protected_len, owned_kv_len)` 中的每个 KV slot，要么转交给树，要么释放，不能遗漏，也不能重复处理。

### 插入前截断与页对齐

Component 协商得到 `effective_cache_len` 后，请求的 token 和 KV 会先截断到该长度，再构造 page-aligned `RadixKey`。

只有页对齐后的部分进入 `insert()`。剩余部分包括两类：

```text
component 截断尾部
+
page alignment 产生的不齐尾部
```

它们都仍归请求所有，必须在插入后释放。

### 尾部释放为什么需要合并区间

Finished 路径中最容易出错的是尾部区间计算。

正常插入时，释放起点是：

```python
page_aligned_len
```

如果 insert 因 rotation conflict 整体拒绝尾部，则树没有接收新 KV，释放起点改为：

```python
min(cache_protected_len, len(kv_indices))
```

这里使用 `min`，是因为原来的 protected 前缀可能已经超过截断后的缓存长度，而 `free_kv_row()` 要求传入升序区间。

如果 component 截断尾与页不齐尾在边界相接，两者可能共享同一物理页。代码会把它们合并成一个区间后再交给 `free_kv_row()`，避免同一页面被释放两次。

这与 `free_kv_row()` 内部使用 `coalesce_ranges()` 解决的是同一类问题：逻辑区间可以分开计算，但物理页所有权只能结算一次。

---

## 5.5 Finished 请求为什么再次插入 Prompt

完成请求通常由两部分组成：

```text
较长且容易被复用的 Prompt
+
较短且高度请求特定的输出
```

如果只插入完整序列，它们可能形成同一个叶节点。将来驱逐短输出时，高价值 Prompt 也会一起被删除。

因此，在特定条件下，Finished 路径会再次插入：

```python
req.origin_input_ids
```

使 Prompt 成为独立的 Radix 节点。

该操作只在以下条件下发生：

- rotation tail 没有被拒绝；
- 当前只有 FULL component；
- Prompt 非空且短于完整插入 key。

第二次插入主要用于调整树拓扑，不代表又产生了一份 KV：

- `prev_prefix_len` 防止已有 KV 被当作重复分配再次释放；
- `chunked=True` 防止本请求刚创建的节点被额外记一次命中。

后一点会直接影响轮次 4 的驱逐策略：`hit_count` 是 LFU、SLRU 等策略的排序依据。如果这次纯拓扑插入也增加命中次数，就会系统性地高估每个 Prompt 节点的价值。

---

## 5.6 Unfinished：提交以后请求还要继续运行

Unfinished 路径更复杂，因为它不能在 insert 后直接释放请求资源。

阶段性 KV 进入树以后，请求必须切换到树最终采用的 canonical KV indices，并继续执行。

完整过程是：

```text
insert
  → 如果 rotation declined，保留请求原状态并返回
  → match_prefix
  → 得到树中的 canonical indices 和 last_node
  → 重写 ReqToTokenPool
  → 释放旧节点锁
  → 获取新节点锁
  → 更新请求缓存字段
  → component cleanup
```

### 为什么 insert 后还要重新 match

`insert()` 不保证树最终使用请求传入的原始 indices。

插入过程中可能：

- 与已有节点重叠；
- 释放重复 KV；
- 发生节点 split；
- 复用树中已经存在的 canonical value。

所以 insert 完成后，UnifiedRadixCache 会重新执行 `match_prefix()`，取得树最终认可的：

```text
device_indices
last_device_node
```

随后把请求行中原 protected boundary 之后的部分重写为这些 canonical indices。

### 请求的四项状态必须一起更新

重绑定完成后，以下字段必须作为一个整体翻新：

```python
req.prefix_indices
req.kv.cache_protected_len
req.last_node
req.lock_receipt
```

如果只更新其中一部分，就会出现：

- 请求行已经指向新 KV，但锁仍保护旧节点；
- `cache_protected_len` 与实际树前缀不一致；
- 请求结束时使用旧 receipt 解锁新节点。

因此，Unfinished 路径本质上是一笔“重绑定事务”。

### 锁迁移中的隐式前提

当前代码的顺序是：

```text
先 _dec_req_lock() 释放旧锁
再 inc_lock_ref() 获取新锁
```

两者之间存在一个短暂窗口：旧节点已经解锁，新节点尚未加锁。

这依赖一个跨模块前提：

```text
Scheduler 单线程执行这段事务，
且期间不会插入另一轮 eviction。
```

当前代码没有专门的断言保护这个前提。如果未来把其中一段改成可重入或异步执行，就需要重新检查锁迁移顺序。

---

## 5.7 Rotation decline 如何保持事务原子性

如果 insert 检测到 rotation-base 冲突，它会设置：

```python
rotation_tail_declined = True
```

TreeCore 在正式 WALK 之前就拒绝整段尾部，因此没有：

- 转移页面所有权；
- 释放重复 KV；
- 把请求行重绑定到另一套 rotation run。

Unfinished 路径此时直接保留原始 `prefix_indices` 和旧锁，让请求继续使用自己的页面。

Finished 路径则知道树没有接收尾部，于是释放 protected prefix 之后仍属于请求的范围。

两条路径虽然处理结果不同，但遵守同一个原则：

> 插入要么完成所有权转移，要么在任何转移发生前拒绝，不能留下半提交状态。

---

## 5.8 Cleanup 完成提交事务

无论是 Finished 还是 Unfinished，最后都会调用：

```python
cleanup_after_caching_req(...)
```

这一阶段的统一职责是：

```text
根据 InsertResult 确认 component 在 prepare 阶段产生的资源
究竟已经被树接收，还是仍需由请求侧清理。
```

UnifiedRadixCache 不理解每种附加资源的内部结构，只负责保证：

```text
prepare
  → insert 或提前退出
  → cleanup
```

构成完整配对。

即使发生以下情况，也仍然需要 cleanup：

- 有效缓存长度为零；
- insert 被拒绝；
- 缓存处于 disable 状态；
- 树中已经存在等价 component 状态。

具体每个 component 如何判断“已接收”以及怎样释放自己的临时资源，属于轮次 6。

---

## 5.9 分布式一致性也是提交契约的一部分

UnifiedRadixCache 的关键方法使用 `rank_consensus`，确保 tensor-parallel ranks 不会对树做出不同决策。

与本轮直接相关的约束包括：

```text
cache_finished_req
    req.rid、is_insert、owned_kv_len 必须一致

cache_unfinished_req
    req.rid、chunked 必须一致

insert
    各 rank 的 key 长度必须一致
    返回的 prefix_len 必须一致

match_prefix
    MatchPrefixParams 必须一致
    关键命中长度必须一致
```

`insert()` 只要求 `len(params.key)` 一致，而不要求整个 `params` 完全一致，因为不同 rank 上的 value tensor 可以不同；但树的拓扑长度和最终插入边界必须一致。

这是一层没有出现在 `BasePrefixCache` 中的实现契约。基类描述单实例下的资源所有权，而 UnifiedRadixCache 还必须保证所有 TP rank 以相同方式推进树结构和请求生命周期。

---

## 5.10 本轮需要维持的不变量

请求级提交的正确性可以归纳为以下不变量：

1. `[0, cache_protected_len)` 已属于树，不能作为请求新 KV 再次释放。
2. `[cache_protected_len, owned_kv_len)` 必须被树接收或归还 allocator。
3. TreeCore 的重复片释放与外层的尾部释放不能重叠。
4. 共享同一物理页的释放区间必须合并。
5. Unfinished 请求提交后必须仍拥有完整、可继续执行的 KV 映射。
6. `prefix_indices`、`cache_protected_len`、`last_node` 和 `lock_receipt` 必须同步更新。
7. Component prepare 产生的临时资源必须被树接收或由 cleanup 回收。
8. 所有 TP rank 必须得到一致的提交边界和树结构结果。

---

## 5.11 本轮结论

`UnifiedRadixCache` 是请求生命周期与 TreeCore 之间的事务协调层：

```text
Component hooks
    提供统一的提交约束和附加状态接口

TreeCore
    决定树如何接收、去重和组织前缀

UnifiedRadixCache
    协调提交边界、尾部释放、请求重绑定和锁迁移

Allocator
    接收最终不再归请求或树所有的物理资源
```

Finished 与 Unfinished 的本质区别是：

```text
Finished：
    提交后，请求资源必须完成最终结算

Unfinished：
    提交后，请求必须切换到树的 canonical 状态并继续运行
```

这一轮只说明 component 如何被编排进请求级事务。FULL、SWA、Mamba 各自提交什么、为什么对缓存长度提出不同限制，以及各自怎样完成资源转移，将在轮次 6 单独展开。