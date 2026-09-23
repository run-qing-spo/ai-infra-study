# 轮次 03 · 建树与 match / split / insert

> 读的文件：`unified_cache/unified_tree_core.py`（3151 行）

## 3.1 UnifiedTreeNode：异构性藏在定长数组里

| 字段 | 形状 | 语义与约束 |
| --- | --- | --- |
| `component_data` | `list[ComponentData]`，长度 `_NUM_COMPONENT_TYPES` | 按 int enum 下标寻址。**定长** —— 即便本树只挂 FULL，也分配全部槽位 |
| `children` | `dict`（非 defaultdict） | 源码注释：缺键读**必须抛异常**，绝不能在 arena 之外静默铸出未注册节点 |
| `lru_prev` / `lru_next` | 两个长度 `N×2` 的数组 | 侵入式双链表**内嵌在节点里**：每 component 一条 device LRU + 一条 host LRU |
| `id` | 类级 `counter` 自增 | 对外唯一暴露形式（NodeId）；arena `_node_arena: dict[NodeId, node]` 做反查 |
| `write_through_pending_id` / `load_back_pending_id` | `Optional[int]` | 两个方向的在途 DMA 标记，决定节点能否被当作 duplicate 回收 |
| `rotation_base` | `Optional[int]` | 逻辑页 KV sharding 的链常量。**host 侧镜像** —— 真值在 device tensor 上，读它会把 D2H 同步压到 alloc 路径 |
| `_tlru_cached_prefix_len` / `_tlru_history_len` | int，非 tlru 时恒 0 | 分支高水位，尾部裁剪后仍存活 |
| `hash_value` / `event_hash_value` | 两套独立哈希链 | 后者仅供外部 KV events 使用（namespace-aware） |

::: warning 对照轮次 01
`node.backuped` 与 `node.evicted` 两个 property 的定义**只看 FULL**（`component_data[FULL].host_value / .value`），但基类 `is_backuped()` 把它当成「节点是否已备份」的通用判定。辅助 component 只剩 tombstone 的节点在这两个 property 下仍算「在设备上、已备份」。
:::

## 3.2 reset() 建的是什么

- root：`priority = -sys.maxsize`、`key = RadixKey(array("q"), None)`、`component_data[FULL].value = []`（**空 list，不是 tensor**）、`hash_value = []`，且对每个 component `lock_ref = 1` —— root 永不可驱逐靠的是这个常驻锁。
- 两套 LRU：`lru_lists[ct]`（device）与 `host_lru_lists[ct]`（`use_host_ptr=True`），各自可带 session 谓词。`_session_lru_predicate` 对 FULL **恒返回 None** —— session ref 只保护辅助 component。
- 两个叶集合 `evictable_device_leaves` / `evictable_host_leaves`，与 LRU 并行维护（`_update_evictable_leaf_sets` 是唯一写入点）。
- `full_host_duplicates: dict[NodeId, node]` —— 两层都有 FULL 的冗余 host 副本，write_back 优先回收；用**插入序 dict** 保证 TP 各 rank 选出同一批受害者。配套 `write_back_duplicate_reclaim_digest`（42 位掩码，注释说明是为了 Rust 端 int64 不溢出、且能 all_reduce `[d, -d]` 做 TP 一致性校验）。
- `_ongoing_insert_walk_state` 也在 reset 里清空 —— insert 状态机的生命周期绑在 reset 上。

## 3.3 match_prefix：一次会改树的「读」

```text
match_prefix
  ├ key.maybe_to_bigram_view(is_eagle)      # eagle 双 token 视图
  ├ key.page_aligned(page_size)             # 两次 len==0 短路 → _empty_match_result
  ├ _match_prefix_helper(key)               # ← 这里可能 _split_node
  └ _match_post_processor(...)              # LRU + 时间戳 + component 收尾 + NodeId 化
```

- **validator 分双套只在 HiCache 下**：非 HiCache 时只有 `match_device_only=True` 一套，`best_match_node` 与 `best_match_device_node` 同步推进；HiCache 下 host-backed 节点也能 match，两者才分叉。
- **死节点剪枝**：`child.evicted and not child.backuped` 立即 break。
- **match 会 split**：`prefix_len < len(child.key)` 时调 `_split_node` 并把返回的 action 带出去 —— 这正是轮次 01 表中 `MatchResult.cache_actions` 的唯一来源，也是「match 不是纯读」的根据。
- **FULL 不走 LRU**：`_match_post_processor` 对非 FULL component 调 `refresh_lru(MATCH_END)`，而 FULL 靠从命中点向上回溯改写 `last_access_time`、每级递减 `0.00001` 来保持 root→leaf 的序。
- `last_host_node` 的取值：HiCache 下为 `best_match_node`（各 component 在 device+host 两侧达成共识的最深点），否则退化为 `best_match_device_node`。它是 `prefetch_from_storage` 的锚点。
- 出口做了一次统一的 **NodeId 化**：三个 node 字段在 `_replace` 里换成 `.id`。TreeCore 之外看不到节点对象。

`match_full_device_prefix` 是另一条只读路径：忽略辅助 component，只看 FULL 的 device value；第三个返回值 `pinned_len` 统计的是**最深节点的整长** —— 部分命中也会把整个节点钉住。

## 3.4 insert：可恢复三阶段状态机

insert 不是一次调用，而是 TreeCore 与 Cache 之间的**协程式来回**：TreeCore 走到一个 barrier 就把待执行的 action 吐出来挂起，Cache 执行完再 `resume`。

```python
# unified_radix_cache.py:584 的驱动循环
assert not tree_core.has_ongoing_insert()        # 单飞，再入即失败
try:
    step = tree_core.begin_insert(params)
    while True:
        self._apply_cache_actions(step.actions)
        if step.result is not None:
            assert not step.result.cache_actions   # 终态 result 必须 action-free
            return step.result
        step = tree_core.resume_insert()
finally:
    self._apply_cache_actions(tree_core.end_insert())   # 异常时排空，保证 free 到达 allocator
```

| 阶段 | 做什么 | 产出的 action |
| --- | --- | --- |
| `WALK` | 逐节点下行：touch → match → 必要时 split → *unevict 路径* 或 *overlap 路径* | `ReplaceWriteThroughOnNodeSplit`、`FreeDeviceKV(FullOnly)`、backup |
| `COMMIT` | 为剩余后缀建尾叶（`_add_new_node`），再跑各 component 的 `commit_insert_component_data` | component 自定 |
| `TAIL` | `refresh_lru(INSERT_END)` + 终局 backup 判定 | backup |

::: tip 挂起判据
只有当某一步产出了**非可延迟** action 才挂起。可延迟集合恰好三个（`_is_deferrable_action`）：`FreeDeviceKV`、`FreeDeviceKVFullOnly`、`ReplaceWriteThroughOnNodeSplit` —— 即「发出去就不管」的回收 / 改名类操作。其余（如 backup 提交）必须在树继续下行前落地。
:::

## 3.5 WALK 步里的三条分叉

- **evicted 节点 → `_unevict_node_on_insert`**：用本请求的新 KV 复活 FULL value（`value.clone()`），按 `lock_ref` 决定计入 protected 还是 evictable；随后**逐个**调非 FULL component 的 `recover_after_unevict`，因为辅助 component 可能仍是 tombstone，需要从同一片 slice 重建。
- **命中节点 → `update_component_on_insert_overlap`**：各 component 认领重叠 KV 槽的所有权，取所有 component 返回值的 `min` 作为 `consumed_from`。
- **重复片的释放**：`dup_start = max(0, prev_prefix_len - total_prefix_length)`，在 `[dup_start, consumed_from)` 上按本请求的 `swa_evicted_seqlen` 切成两段 —— 低于 eviction floor 的那段只能释放 full 侧（`FreeDeviceKVFullOnly`），其余走 `FreeDeviceKV`。

::: danger sharding 前置否决
`begin_insert` 一开头就跑 `_rotation_conflict`（`_insert_walk_step` 匹配逻辑的只读镜像）。跨 rotation base 嫁接会造出页 owner 不成一个循环段的路径，破坏 padded-allgather / `k // N` 的翻译契约。源码点名 insert 有**三处**页所有权转移（尾叶 `_add_new_node`、`_unevict_node_on_insert`、`update_component_on_insert_overlap`），所以判据提前到入口**整体否决**，顺带避免 walk 里的重复 free 跑起来 —— 被拒的请求留在自己的页上。出口即 `InsertResult.rotation_tail_declined=True`。
:::

## 3.6 _split_node 复制了什么

新父节点继承 `priority` / `hit_count` / `external_cache_stored` / `creation_time` / `load_back_pending_id` / `rotation_base`；`hash_value` 与 `event_hash_value` 各自经 `split_node_hash_value` 一分为二；随后每个 component 跑 `redistribute_on_node_split`。

- **tlru**：split 不增加分支深度 —— 新父取 `parent._tlru_cached_prefix_len + split_len` 并**继承 child 的 high-water**，child 自身深度不变。
- **在途 write-through**：若 child 有 `write_through_pending_id`，新父也打上同一 ack_id，并发出 `ReplaceWriteThroughOnNodeSplit` 让 cache 侧修正待发布列表。
- **LRU 位置**：新父用 `insert_after(child, ...)` 放在 child 旁边、同一 session 分区，保留后缀的 recency。
- **非对称**：split 明说「不访问后缀」，却仍把 `child.last_access_time` 刷新为当前 —— FULL 的驱逐序因此受 split 影响，而 LRU 侧不受。留到轮次 04 验证是否与 `evict` 的取序一致。

`_add_new_node` 侧：`value.clone()`（**拷贝索引张量，所有权真正落到树上**）、`component_evictable_size_[FULL] += len(value)`、仅在 `enable_storage or enable_external_cache_linker` 时才算 hash、最后 `kv_events.record_store`。两处新建节点都会调 `_update_evictable_leaf_sets`（自身 + 父）与 `_update_duplicate_tracking`。

::: warning 懒失效
`_update_duplicate_tracking` 的注释自述：**只在 duplicate 诞生处注册（ack / split / unevict），注销是懒的**，所以 `full_host_duplicates` 里可能有陈旧条目，消费侧必须实时复检（`_can_reclaim_full_host_duplicate`）。轮次 04 / 11 要沿这条线确认复检覆盖了 `load_back_pending_id`。
:::

---

上一轮：[轮次 02 · Scheduler 侧调用点](./round-02-scheduler) · 下一轮：[轮次 04 · eviction / LRU / lock_ref 生命周期](./round-04-eviction)
