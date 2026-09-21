# 轮次 02 · Scheduler 侧调用点

> 读的文件：`scheduler` · `schedule_policy` · `schedule_batch` · `mem_cache/common`

## 2.1 调用点已被 wrapper 收敛

Scheduler **不直接调** `cache_finished_req` / `cache_unfinished_req`，全部走 `mem_cache/common.py` 的门面。

| Wrapper | 位置 | 契约之外的额外职责 |
| --- | --- | --- |
| `maybe_cache_unfinished_req` | `common.py:156` | 仅加 `skip_radix_cache_insert` 短路 |
| `release_kv_cache` | `common.py:272` | ① Mamba 早分配路径的先行 free ② 计算 `owned_kv_len` ③ 调 `cache_finished_req` ④ `_release_overallocated_kv_indices`（spec decode / strip_thinking 超分配尾巴，按 allocator page 对齐）⑤ 非 Mamba-cache 时 `free_mamba_cache` ⑥ `req_to_token_pool.free(req)`（DSV4 NPU 子类顺带释放 c4/c128）⑦ `mark_kv_released` |
| `evict_from_tree_cache` | `common.py:163` | 经 `allocator.evict_to_free_tokens(tree_cache, n)` 反向驱动 |

::: tip 契约哨兵
`release_kv_cache` 前后两次 `assert (not req.kv.holds_kv) == req.kv.is_kv_released`：允许 `cache_finished_req` 内部提前释放（`StreamingSession` 正是如此），但实现必须自己把状态标对。
:::

## 2.2 六类调用的实际分布

| 类别 | 调用点 | 备注 |
| --- | --- | --- |
| match | `schedule_policy:174`（`match_prefix_for_req` 唯一入口）、`:277` fast-path、`:398` waiting_queue 树、`schedule_batch:1617` | `swa_reprefill_tail_tokens()` 在匹配前截断 key；`SGLANG_RADIX_FORCE_MISS` 走 `zero_match_result` |
| cache_unfinished | `scheduler:3532`(chunked=True)、`batch_result_processor:383 / 474` | 3 处 |
| cache_finished | `batch_result_processor:128 / 376 / 471 / 1253 / 1342`、`scheduler:3574 / 5431 / 5451 / 5463`、`dynamic_chunk_sizer:267` | 10 处，全部经 `release_kv_cache` |
| abort | `scheduler:3260 / 3359 / 5422 / 5458 / 5478` `_release_aborted_request`；`:3331` `tree_cache.finish(handle, ABORT)` | attempt 级取消 |
| retract / preempt | `schedule_policy:1454 preempt_to_schedule`；`schedule_batch:2238` `release_kv_cache(is_insert=False)` | 见下方时序陷阱 |
| flush | `scheduler:5148 flush_cache` → `tree_cache.reset()` + 三个 pool clear | 仅 `is_fully_idle()` 时执行 |

::: danger 时序陷阱（源码注释固化）
请求完成分两阶段：① `update_finish_state + release_kv_cache`（在 `process_batch_result`）② 从 batch 中 filter out（在 `get_next_batch_to_run`）。抢占发生在两阶段**之间**，因此 `running_batch` 里可能存在 KV 已释放的 req —— 必须用 `not r.finished()` 过滤，否则 double-free。
:::

## 2.3 锁生命周期：三种使用模式

```python
# A. 持久锁（准入）schedule_policy.py:979
req.lock_receipt = tree_cache.inc_lock_ref(req.last_node).to_dec_params()

# B. 临时锁（contextmanager）schedule_policy.py:1074 _lock_node
result = tree_cache.inc_lock_ref(last_node)
dec_lock_params = result.to_dec_params() if tree_cache.is_tree_cache() else None
...
tree_cache.dec_lock_ref(last_node, dec_lock_params) if dec_lock_params \
    else tree_cache.dec_lock_ref(last_node)

# C. SWA-only 释放 schedule_batch.py:3836
tree_cache.dec_swa_lock_only(...)   # ← 不在 BasePrefixCache 上
```

::: warning 调用方期待 > 基类契约
三类 duck-typed 缺口：**① 锁** `dec_swa_lock_only`（`schedule_batch:3836`）。**② 属性** `tree_cache.sliding_window_size`（`schedule_batch:3798 / 3862`，基类未声明，只有 `CacheInitParams` 有）。**③ 方法** `mamba_evictable_size`、`req_to_token_pool.mamba_allocator`、`ensure_session_generation` / `open_radix_session`、以及整组 storage API：`hicache_storage_pass_prefix_keys` / `prefetch_from_storage` / `check_prefetch_progress` / `attach_storage_backend` / `detach_storage_backend` / `clear_storage_backend`。

对照轮次 01 的 1.2 表：基类对 HiCache / storage 只暴露 4 个方法 + 3 个属性（`init_load_back`、`check_hicache_events`、`ready_to_load_host_cache`、`flush_pending_backups`），而 `scheduler.py:3145–3184` 与 `3953–4096` 这两段 prefetch 状态机**完全跑在契约之外**。留到轮次 10、12。
:::

## 2.4 异步不变式

::: tip load-back 全有或全无
`init_load_back` 返回 `None` 在 `schedule_policy:1287` 被固化为 `AddReqResult.OTHER`（重试准入，不是失败）；紧接一条硬断言：`0 < host_loaded_length < promised_host_hit` 即 `RuntimeError`。允许返回 0（回退重算），不允许部分提交。
:::

---

上一轮：[轮次 01 · BasePrefixCache 接口契约](./round-01-contract) · 下一轮：[轮次 03 · 建树与 match / split / insert](./round-03-tree-core)
