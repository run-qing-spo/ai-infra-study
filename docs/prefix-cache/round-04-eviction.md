# 轮次 04 · eviction / LRU / lock_ref 生命周期

> 读的文件：`unified_cache/unified_tree_core.py` · `evict_policy.py` · `unified_cache/components/full.py` · `unified_cache/utils.py`

::: danger 本轮核心发现
树里并存**两套互不相通的驱逐序**：FULL 走「每轮重建的堆 + `EvictionStrategy`」，辅助 component 走「常驻侵入式 LRU 链表 + 游标」。后果是 `evict_policy.py` 的 8 种策略**只对 FULL 生效**，SWA / Mamba 恒定 LRU 序，无论 `--radix-eviction-policy` 取什么值。
:::

| | FULL | SWA / Mamba 等辅助 |
| --- | --- | --- |
| 序的载体 | `heapq`，每次 `_evict_device_start` 从 `evictable_device_leaves` 现建 | `UnifiedLRUList` 侵入式双链表，常驻 |
| 排序键 | `(session_ref>0, session_ref, eviction_strategy.get_priority(node))` | 链表物理位置 |
| 策略可配 | 8 种 | 恒 LRU |
| 推进方式 | pop 堆顶，失效条目（已不在叶集）跳过；驱逐后把 parent 压回堆 | `cursor_begin / cursor_next / cursor_end` 从 tail 向 head 走，跳过 `lock_ref>0` |
| 时间戳来源 | `node.last_access_time`（match 回溯 / `_touch_node` / split） | 不读时间戳 |

## 4.1 EvictionStrategy 的 8 种实现

| 策略 | `get_priority` 返回 | 读到的节点字段 |
| --- | --- | --- |
| `lru` | `last_access_time` | 受 match 回溯**与 split** 影响 |
| `lfu` | `(hit_count, last_access_time)` | `hit_count` 由 `_inc_hit_count_and_check` 递增，split 时新父**继承** |
| `fifo` / `filo` | `±creation_time` | split 新父**继承 child 的 creation_time**，二者同龄 |
| `mru` | `-last_access_time` | 同 lru |
| `priority` | `(priority, last_access_time)` | `priority` 沿 walk 单调抬高（`max`），root 为 `-sys.maxsize` |
| `slru` | `(hit_count>=阈值, last_access_time)` | 两段式：试用段整体先于保护段 |
| `tlru` | `(-1 或 0, last_access_time)` | `_tlru_history_len` / `_tlru_cached_prefix_len` |

::: tip T-LRU 的实现取巧
预算 `budget = max(history_len + Q̂ - threshold, 0)`；`cached_without_this_node >= budget` 即 TEL-safe，**报告为无穷旧（-1 段）**。驱逐器因此先排干这批（论文 phase 1）再退回纯 recency（phase 2），两个驱逐循环都不需要知道 T-LRU 的存在。`L` 取**不收缩**的 `_tlru_history_len` 而非当前驻留长度 —— 否则被裁短的会话下一轮仍然超预算，T-LRU 会把它一路削到零，而不是停在 `threshold - Q̂`。
:::

## 4.2 UnifiedLRUList：一条链表干三件事

- **槽位隔离**：`_pt = component_type + (N if use_host_ptr else 0)`，所以同一节点上 device 与 host 指针永不冲突 —— 这就是节点里 `N×2` 数组的由来。
- **session 分区**：`[head..mid)` 放 session-referenced，`(mid..tail]` 放未引用，驱逐从 tail 走即自然先动未引用的。`mid` 与 `cursor` 是**哨兵节点**，对每个 component 预置 `lock_ref = host_lock_ref = 1`，保证永不被游标选中。
- **双指针重排**：`reset_node_and_parents_mru` 用 `prev_ref` / `prev_unref` 两个游标同时维护两个分区的插入位，一趟走完祖先链且两段各自有序。`reset_node_and_window_ancestors_mru` 是它的滑窗版，按 `len(node.key)` 累积到 `window_size` 即停。
- `_remove_node` 显式把 `lru_prev/next` 置 `None`，注释说明是为了**打断已驱逐节点之间的引用环**。

## 4.3 lock_ref：四对 API，一张收据

| 获取 | 释放 | 作用域 | 计数字段 |
| --- | --- | --- | --- |
| `inc_lock_ref(node_id, skip_lock_components)` | `dec_lock_ref(node_id, params, skip_swa)` | 全 component，沿 root 路径 | `lock_ref` |
| `inc_host_lock_ref` | `dec_host_lock_ref` | 全 component，host 侧 | `host_lock_ref` |
| `inc_full_pin` | `dec_full_pin` | **仅 FULL** | `lock_ref` |
| 无配对 inc | `dec_swa_lock_only` | SWA + 严格更低优先级 | `lock_ref` |

- **收据锚定**：`_assert_receipt_anchor` 断言 `params.node_id == node.id` —— 收据只能在它的获取节点上释放，否则会静默释放（或窃取）另一持有者的段。这条直接兑现了轮次 01 表里 `dec_lock_ref` 的失败语义。
- **释放顺序**：`_release_components` 走 `reversed(self.components)`，辅助先于 FULL，好让 FULL 的叶集刷新看到辅助的终值；但注释自陈「顺序对叶集不是 load-bearing」，因为辅助的 walk 也会刷新。
- **`dec_swa_lock_only` 不是独立锁**，而是 `inc_lock_ref` 所取整体锁的**部分提前释放**：放掉 SWA，再按 `eviction_priority(is_leaf=False)` 严格小于 SWA 的比较，连带放掉 Mamba；`params.skipped_lock_components` 保证当初没取的不会被放。配套地，后续 `dec_lock_ref(skip_swa=True)` 跳过「SWA 及以下」。这就是轮次 02 记录的 `schedule_batch:3836` 那个契约外调用的对侧实现。
- `inc_full_pin` 只钉 FULL 的理由写在 docstring 里：**SWA 段锁的收据在 window 重新 attach 后不存活**，所以不碰它。

::: warning 死字段
`dec_lock_ref` 里 `TODO: delta is not aggregated from components; no caller uses it yet` —— 轮次 01 契约表中的 `DecLockRefResult.delta` 与 `IncLockRefResult.delta` 在 unified 路径上恒为 `None`。真正的收据内容是 `node_id` + `swa_uuid_*` + `skipped_lock_components`。
:::

## 4.4 级联驱逐的优先级代数

```text
# eviction_priority(is_leaf)
叶节点：  full = swa = mamba = 0      # 坍缩 —— 驱逐任一即删除整节点
内部节点：full=2 > swa=1 > mamba=0
```

内部节点上 SWA 排在 Mamba 之前的理由（`base.py` docstring）：SWA 是**路径数据**，滑窗需要 root→匹配边界的连续覆盖；Mamba 只在匹配边界节点有意义，内部节点上不贡献任何东西。

::: tip 坍缩带来的双重判定
`_should_cascade_evict_component` 先用 `is_leaf` 后的优先级筛出候选，再用**真实（internal）优先级**复判：真实优先级 ≥ trigger 的 component，其上的锁是*合法 pin*，`return False` 跳过；真实优先级严格更低的，其上还有锁就是*真泄漏*，落到下面的 `assert cd.lock_ref == 0` 上炸掉。两种情况在叶上看起来一样，靠这一层区分开。
:::

`_cascade_evict` 末尾把 `FULL.value = None` 的 tombstone **延后到所有 component 释放之后** —— 注释说明 `free_swa` 反过来依赖 `Full.value`。代码里有两处设置点（trigger 是 FULL 时一处，`base_evicted` 时一处），效果重叠。

## 4.5 回填轮次 03 留下的两个尾巴

**① split 刷新 `last_access_time` —— 确认有影响，范围可界定。** 影响**仅限 FULL**：FULL 的堆键读 `last_access_time`，所以 split 后的 child 变「更新」，更晚被驱逐。辅助 component 不受影响，因为 `_split_node` 用 `insert_after(child, new_node)` 原位插入、不动 child 的链表位置。另注意 `fifo` / `filo` 下新父继承 `creation_time`，与 child 同龄，split 对这两种策略是中性的。

**② duplicate 懒失效 —— 已闭合。** `_can_reclaim_full_host_duplicate` 三项实时复检齐备：`write_through_pending_id`、`load_back_pending_id`、`host_lock_ref == 0`。调用方 `_reclaim_full_host_duplicates` 另在遍历中把 `value` 或 `host_value` 已为 `None` 的陈旧条目收进 `swept_ids`，**遍历结束后统一 pop**（不能在迭代中改 dict）。删除路径 `_remove_leaf_from_parent` 也会主动 `pop`，注释写「删除的节点不得作为幽灵留在 duplicate 跟踪里」。

## 4.6 本轮新增的观察项

- `drop_subtree_no_host`：write-back 模式下 D→H 备份因 host 内存压力失败时的兜底 —— 整棵子树丢弃，避免该 KV 在 host 腾出空间前一直不可驱逐。前置断言 `not node.backuped and write_through_pending_id is None`，且子树内任一节点带锁即整体放弃。
- `evict_device_leaf` 三分支：**未备份 + write-back** → 返回 `BackupKV` 交给 cache 执行后再 demote；**未备份 + write-through** → 直接删除；**已备份** → demote。第一条是 TreeCore 把控制权交还 Cache 的又一处 barrier，与轮次 03 的 insert 状态机同构。
- `_begin/_finish_tracking_unbacked_tokens` 是一对带 `assert` 的单飞计数器，统计 write-through 下被丢弃的无备份 token，最终经 `_record_dropped_tokens(reason="write_through_unbacked_eviction")` 上报。

---

上一轮：[轮次 03 · 建树与 match / split / insert](./round-03-tree-core) · 下一轮：轮次 05 · 请求级生命周期（待读）
