# SGLang Prefix Cache 技术分析：先理解请求生命周期，再阅读缓存接口

## 0. 这次分析究竟要回答什么

Prefix Cache 并不是一个孤立的数据结构。它处于请求调度、模型执行和 KV 显存管理三者的交界处。

阅读相关代码时，真正需要回答的是：

1. 一个请求进入 Scheduler 后，会经历哪些阶段？
2. 每个阶段会读取或修改请求上的哪些字段？
3. token、KV slot、请求映射表和 Radix Tree 节点之间是什么关系？
4. 一段 KV 在什么时候属于请求，什么时候转交给 Prefix Cache？
5. 正常完成、分块 Prefill、抢占、取消和 HiCache 回载分别走什么路径？

只有先建立这张地图，`match_prefix`、`cache_unfinished_req`、`cache_finished_req`、`inc_lock_ref` 等接口才有明确含义。

---

## 1. 四种容易混淆的数据

先区分四个完全不同的概念。

### 1.1 Token ID

例如：

```text
[1, 314, 1599, 42]
```

它表示请求的文本内容。

Prefix Cache 使用 token 序列作为检索键。两个请求只有在 token 序列以及 `extra_key`、`cache_salt` 等命名空间信息兼容时，才可能复用同一段缓存。

相关字段主要是：

```python
req.origin_input_ids
req.output_ids
req.full_untruncated_fill_ids
```

### 1.2 KV slot index

例如：

```text
[1042, 1043, 2108, 2109]
```

它不是 token，而是这些 token 的 KV 状态存放在设备 KV 池中的位置。

Prefix Cache 的树节点保存的 `value`，核心上就是这样一组 KV slot index。

因此：

```text
树的 key   = token 序列
树的 value = 与这些 token 对应的 KV 存储位置
```

### 1.3 ReqToTokenPool 中的一行

每个正在占用 KV 的请求会获得一个 `req_pool_idx`。它指向 `ReqToTokenPool.req_to_token` 中的一行：

```text
请求中的第 0 个 token → KV slot 1042
请求中的第 1 个 token → KV slot 1043
请求中的第 2 个 token → KV slot 2108
...
```

它是“请求逻辑位置”到“设备 KV slot”的映射。

需要注意：

- `req_pool_idx` 不是 KV slot；
- `prefix_indices` 不是请求池的行号；
- `prefix_indices` 是从 Prefix Cache 匹配得到的 KV slot 序列；
- 请求被释放时，请求池中的行和设备 KV slot 是两类不同资源。

### 1.4 Radix Tree 节点

Radix Tree 用 token 前缀组织已经可以复用的 KV。

树节点负责表达：

- 哪一段 token 是缓存键；
- 这一段 token 对应哪些 KV slot；
- 节点的父子关系；
- 节点是否正在被请求引用；
- 节点是否允许被驱逐；
- 在 HiCache 模式下，内容位于设备、Host 还是外部存储。

`req.last_node` 是请求当前匹配或锁定的树节点句柄。UnifiedRadixCache 中它通常是 `NodeId`，其他实现可能直接保存节点对象。

---

## 2. 请求上的缓存状态

缓存相关状态分散在 `Req` 和 `ReqKvInfo` 中。

### 2.1 匹配结果：保存在 Req 上

`match_prefix_for_req()` 调用 `tree_cache.match_prefix()` 后，会写入：

```python
req.prefix_indices
req.last_node
req.last_host_node
req.best_match_node

req.host_hit_length
req.swa_host_hit_length
req.mamba_host_hit_length

req.num_matched_prefix_tokens
```

这些字段描述“发现了什么”：

- `prefix_indices`：已经在设备上的匹配 KV；
- `last_node`：设备命中的末端树节点；
- `last_host_node`：Host 命中的末端节点；
- `best_match_node`：跨层级比较后的最佳匹配节点；
- `host_hit_length`：命中但仍需从 Host 搬回设备的长度。

设备命中和 Host 命中不能混为一谈。前者可以直接参与计算，后者还要经过显存准入和异步搬运。

### 2.2 KV 所有权状态：保存在 ReqKvInfo 中

最重要的字段是：

```python
req.kv.req_pool_idx
req.kv.cache_protected_len
req.kv.kv_committed_len
req.kv.kv_allocated_len
```

可以把请求当前的一行 KV 映射理解成三个区间：

```text
0                 cache_protected_len       kv_committed_len       kv_allocated_len
|--------------------------|-------------------------|----------------------|
      Cache 已拥有                 请求已写入                已分配但可能尚未提交
```

更准确地说：

- `[0, cache_protected_len)`  
  已经受到 Prefix Cache 保护。请求可以使用，但不能把这些 KV 当成自己的普通尾部直接释放。

- `[cache_protected_len, kv_committed_len)`  
  请求自己产生并且已经写入有效内容的 KV。

- `[kv_committed_len, kv_allocated_len)`  
  已经预留，但可能因为 speculative decoding、页分配或执行时序尚未形成最终有效内容。

正常结束时，只有其中一部分适合插入 Prefix Cache。剩余部分必须归还 allocator。

`req.kv.holds_kv` 由 `req_pool_idx is not None` 判断。  
`req.kv.is_kv_released` 则要求已分配长度等状态已经清零。

两者必须保持一致，否则意味着请求映射行和实际 KV 所有权已经脱节。

---

## 3. 一次普通请求的完整生命周期

下面先忽略 SWA、Mamba 和 HiCache，只看普通 Full KV 路径。

### 阶段一：请求进入 waiting queue

`Req` 创建时持有：

- 原始 token；
- 采样参数；
- 请求标识 `rid`；
- 缓存命名空间信息；
- 一个尚未持有 KV 的 `ReqKvInfo`。

此时通常还没有：

- `req_pool_idx`；
- KV slot；
- 有效的 `prefix_indices`；
- 树节点锁。

### 阶段二：在 Prefix Cache 中查找前缀

Scheduler 的统一入口是：

```python
match_prefix_for_req(tree_cache, req)
```

它先构造 `RadixKey`：

```python
RadixKey(
    token_ids=...,
    extra_key=req.extra_key,
    limit=...,
    cache_salt=req.cache_salt,
)
```

再调用：

```python
tree_cache.match_prefix(MatchPrefixParams(...))
```

匹配结果被写回请求。

假设请求有 100 个输入 token，其中前 72 个命中设备缓存：

```text
输入 token：       [0 ........................................ 99]
设备缓存命中：     [0 ........................ 71]
仍需 Prefill：                                  [72 ........ 99]
```

此时：

```text
len(req.prefix_indices) = 72
```

但匹配本身通常只是在树中找到 KV，并没有自动保证这些节点永远不被驱逐。

### 阶段三：锁住将要复用的树节点

请求真正获得调度准入时，Scheduler 会调用：

```python
result = tree_cache.inc_lock_ref(req.last_node)
req.lock_receipt = result.to_dec_params()
```

锁的作用不是锁住 Python 对象，而是把相关缓存节点从“可驱逐”状态转成“受保护”状态。

为什么需要返回 receipt？

因为 Unified Cache 可能同时涉及：

- Full KV 节点；
- SWA 节点；
- Mamba 状态；
- Host 节点。

加锁时具体锁了哪些部分，不能只靠之后再次查看节点来推断。receipt 记录了本次实际取得的保护权，释放时应原样交回：

```python
tree_cache.dec_lock_ref(req.last_node, req.lock_receipt)
```

因此锁收据是资源所有权凭证，不只是辅助元数据。

### 阶段四：建立请求到 KV slot 的映射

Scheduler 为请求分配：

1. `ReqToTokenPool` 中的一行；
2. 未命中部分所需的设备 KV slot。

命中的 `prefix_indices` 和新分配的 KV slot 随后被写入同一请求行：

```text
请求逻辑位置：  0 ... 71 | 72 ... 99
KV 来源：       Prefix Cache | 新分配
```

从模型执行的视角看，它们最终都通过请求池中的同一行访问。

但从所有权视角看，两段仍然不同：

```text
[0, 72)    由 Prefix Cache 保护
[72, 100)  当前由请求负责
```

因此：

```python
req.kv.cache_protected_len = 72
```

### 阶段五：执行 Prefill 和 Decode

模型只需要计算未命中的部分。

执行过程中：

- `kv_allocated_len` 表示已经给请求预留到哪里；
- `kv_committed_len` 表示真正完成写入的 KV 到哪里；
- `output_ids` 持续增长；
- 请求可能经过一次或多次 chunked prefill；
- decode 每生成一个 token，都可能继续扩展请求自己的 KV 尾部。

Prefix Cache 不会在每个 decode step 后立即建立一批永久树节点。请求通常先持有新产生的 KV，等到合适的边界再缓存或释放。

### 阶段六：请求完成

所有完成路径最终通过：

```python
release_kv_cache(req, tree_cache, is_insert=True)
```

它不是简单地“free 全部 KV”，而是执行所有权结算。

首先计算：

```python
owned_kv_len = req.owned_kv_len()
```

然后调用：

```python
tree_cache.cache_finished_req(
    req,
    is_insert=True,
    owned_kv_len=owned_kv_len,
)
```

UnifiedRadixCache 会把可缓存的请求尾部插入树中，并释放不能缓存的部分。

随后 `release_kv_cache()` 继续处理：

- speculative decoding 等原因造成的超分配尾部；
- Mamba 独立状态；
- DSV4 的附加状态页；
- `ReqToTokenPool` 中的请求行；
- 请求上的 KV 持有状态。

所以 `cache_finished_req()` 和 `release_kv_cache()` 的职责不同：

```text
cache_finished_req
    结算“可成为缓存的有效 KV”

release_kv_cache
    完成请求级资源的最终清理
```

---

## 4. 分块 Prefill 为什么需要 cache_unfinished_req

长 Prompt 可能不能一次完成 Prefill。

例如一个 10,000-token 请求，被分成多轮执行：

```text
第 1 轮：0    ～ 2047
第 2 轮：2048 ～ 4095
第 3 轮：4096 ～ ...
```

第一轮结束时，请求还没有完成，因此不能走 `cache_finished_req()`。但已经计算出的前缀可能需要：

- 被放入树中供其他请求复用；
- 重新绑定为当前请求的受保护前缀；
- 释放旧锁并取得新节点锁；
- 更新 `prefix_indices` 和 `cache_protected_len`。

这个中间结算入口就是：

```python
maybe_cache_unfinished_req(req, tree_cache, chunked=True)
```

它最终调用：

```python
tree_cache.cache_unfinished_req(req, ...)
```

这里的“unfinished”是“请求未完成”，不是“KV 无效”。

对于 UnifiedRadixCache，它会把已完成的前缀插入树并更新请求的缓存锚点。  
对于 ChunkCache，它不建立共享 Radix Tree，主要是复制或重新绑定请求自己的 KV 索引。

因此这两个实现虽然共享接口，语义能力并不相同。

---

## 5. 抢占、取消和正常完成不是同一路径

### 5.1 正常完成

```text
模型生成结束
  → release_kv_cache(is_insert=True)
  → 有效部分插入 Prefix Cache
  → 其余资源释放
  → finish(handle, SUCCESS)
```

成功结束时，已经提交的异步缓存工作不应被取消。

### 5.2 抢占或 retraction

显存不足时，正在运行的请求可能被撤回等待队列。

典型路径是：

```text
running request
  → 保存或放弃必要的请求状态
  → release_kv_cache(is_insert=False)
  → 不把请求尾部当作正常完成结果插入树
  → 请求以后重新准入
```

这里 `is_insert=False` 很重要。它表示这次释放是调度上的撤回，不是一次可以正式提交到缓存的完成事件。

代码还必须处理一个危险窗口：

```text
请求的 KV 已经释放
但请求对象暂时仍在 running_batch
```

因此后续遍历不能只以“对象还在 batch 中”为依据再次释放，否则会 double-free。

### 5.3 Abort

取消路径除了释放设备 KV，还需要取消或清理：

- Host load-back；
- storage prefetch；
- 尚未完成的异步缓存 attempt；
- 与 `(rid, attempt_id)` 绑定的临时状态。

因此引入了：

```python
CacheRequestHandle(rid, attempt_id)
CacheRequestOutcome
tree_cache.finish(handle, outcome)
```

非成功结果会进入 `release_aborted_request()`；成功结果则保留已经提交的缓存工作。

---

## 6. HiCache 为生命周期增加了什么

普通设备命中可以立即使用；Host 或外部存储命中则不行。

层级缓存路径大致是：

```text
树中发现 Host/L3 命中
  → 记录 host_hit_length
  → Scheduler 判断设备空间是否允许回载
  → init_load_back()
  → 异步搬运
  → 事件完成
  → KV slot 写入请求映射
  → 请求获得准入
```

`init_load_back()` 的返回值不是普通的成功/失败布尔值：

- `None`：本轮不能准入，之后重试；
- 空 tensor：可能表示无需装载、辅助状态成功，或退回重新计算；
- 非空 indices：装载到设备后的 KV slot。

Scheduler 还规定了一个重要不变式：

```text
已承诺回载的 Full KV，要么全部提交，要么一个都不提交。
```

不能只提交其中一部分，否则：

- token 前缀长度；
- 请求行中的 KV 映射；
- 树节点记录的命中长度；
- `cache_protected_len`

会彼此不一致。

这也是 HiCache 相关代码容易出现时序 bug 的根本原因：一次“匹配”被拆成了发现、准入、提交和取消四个阶段。

---

## 7. BasePrefixCache 应该怎样理解

`BasePrefixCache` 不是“Radix Tree 的抽象基类”，而是 Scheduler 与所有前缀缓存实现之间的生命周期协议。

它统一了以下事件。

### 7.1 match_prefix：发现可复用状态

输入是 token 键及请求上下文，输出可能包括：

- 设备 KV indices；
- 设备和 Host 节点；
- Full、SWA、Mamba 的命中长度；
- 后续需要执行的 cache actions。

它只负责发现匹配，不代表请求已经获得长期保护权。

### 7.2 inc_lock_ref / dec_lock_ref：取得和交还保护权

Scheduler 在请求使用缓存前加锁，离开运行状态或完成后解锁。

返回的 receipt 必须进入请求生命周期，不能丢失。

### 7.3 cache_unfinished_req：阶段性提交

用于 chunked prefill 等“请求仍会继续，但已有一段 KV 可以结算”的场景。

### 7.4 cache_finished_req：最终缓存结算

该方法必须处理：

```text
[cache_protected_len, owned_kv_len)
```

区间中的每一个 KV slot：

- 要么插入缓存并转移所有权；
- 要么释放；
- 不能遗漏；
- 不能再次释放已有缓存拥有的前缀。

### 7.5 evict：从缓存回收空间

Eviction 回收的是没有被锁保护的缓存内容。

它和释放请求自己的 KV 不同：

- 请求释放处理 request-owned KV；
- eviction 处理 cache-owned、但当前可驱逐的 KV。

### 7.6 init_load_back 与异步接口

这些接口把 Host/L3 缓存的异步生命周期暴露给 Scheduler。

这部分目前并没有完全收敛到 `BasePrefixCache`：Scheduler 仍然直接访问若干具体实现的方法和属性，因此抽象层仍处于迁移状态。

---

## 8. 为什么要有多种 Prefix Cache 实现

`registry.py` 根据运行配置选择实现。

选择的大意是：

```text
禁用共享 Radix + chunked prefill
  → ChunkCache 或 SWA ChunkCache

纯 SWA 模型
  → PureSWARadixCache

启用 LMCache
  → LMCRadixCache

启用 FlexKV
  → FlexKVRadixCache

默认情况
  → UnifiedRadixCache
```

这不是简单的性能开关。不同实现对生命周期事件的能力不同。

### ChunkCache

它是最小实现：

- 不提供真正的跨请求 Radix Tree 复用；
- `match_prefix()` 通常表现为 miss；
- `inc_lock_ref()` 基本为空操作；
- 仍必须正确管理请求自己的 KV 生命周期。

因此它可以帮助区分：

```text
哪些接口是“树算法需要的”
哪些接口是“任何请求 KV 生命周期都必须实现的”
```

### UnifiedRadixCache

它同时编排：

- Full KV；
- Sliding Window Attention；
- Mamba state；
- 特定硬件状态；
- Host cache；
- 外部存储；
- Session cache。

因此这里的 tree node 不再只代表一种 KV value，而是多个 component 在同一 token 前缀上的组合状态。

---

## 9. 当前抽象层尚未收敛的地方

现有代码已经把主要操作放入 `BasePrefixCache`，但 Scheduler 仍直接依赖一些具体实现能力，例如：

```python
dec_swa_lock_only(...)
sliding_window_size
mamba_evictable_size()
ensure_session_generation(...)
open_radix_session(...)
prefetch_from_storage(...)
check_prefetch_progress(...)
attach_storage_backend(...)
```

这意味着当前迁移状态是：

```text
主生命周期已经接口化
特殊缓存能力仍有一部分依靠 duck typing
```

分析后续代码时，不能仅检查“子类是否实现了抽象方法”，还要检查：

1. Scheduler 是否访问了基类未声明的成员；
2. 某条配置分支是否保证实际对象一定具备该成员；
3. 辅助实现是否在同一生命周期事件上保持相同语义；
4. 异步失败或取消时，是否仍能完成资源结算。

---

## 10. 后续阅读顺序

接下来不再按文件孤立讲解，而按请求生命周期逐段深入：

1. `unified_tree_core.py`  
   `match` 如何沿 token 前缀查找；节点为什么会 split；insert 如何处理已有前缀。

2. 锁与 eviction  
   一次请求怎样把节点从可驱逐集合移入受保护集合，又怎样归还。

3. `cache_unfinished_req` 与 `cache_finished_req`  
   请求自己的 KV 如何转移给树，未转移部分如何释放。

4. Full/SWA/Mamba components  
   为什么相同 token 前缀对应的状态长度和回收规则可能不同。

5. 内存池与 allocator  
   KV slot 最终由谁分配、由谁释放；页对齐和共享页面怎样改变区间语义。

6. HiCache  
   Host/L3 命中如何经过准入、异步搬运、完成和取消。

7. 旁路实现  
   LMC、FlexKV、PureSWA 和旧 RadixCache 是否遵守同一生命周期协议。

后续每一轮都会固定回答四个问题：

```text
请求进入本模块时带着什么状态？
本模块读取和修改哪些数据结构？
KV 或锁的所有权发生了什么变化？
请求接下来可能走向哪些路径？
```

这样分析的中心将从“类和方法清单”转成“请求及其资源如何穿过系统”。