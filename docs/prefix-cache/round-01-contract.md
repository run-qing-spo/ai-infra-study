# 轮次 01 · BasePrefixCache 接口契约

> 读的文件：`base_prefix_cache` · `registry` · `cache_init_params` · `chunk_cache`

抽象基类已经完成**参数对象化改造**：所有多态方法走 dataclass 入参 / 出参，不再是散装 kwargs。这是转换最彻底的一层。

## 1.1 数据契约（DTO 层）

| DTO | 方向 | 通用字段 | 异构成分（未收敛） |
| --- | --- | --- | --- |
| `MatchPrefixParams` | in | `key: RadixKey` | `cow_mamba` / `req` —— Mamba 专用 |
| `MatchResult` | out | `device_indices`, `last_device_node`, `last_host_node`, `best_match_node` | `swa_host_hit_length`, `swa_branching_seqlen`, `mamba_host_hit_length`, `mamba_branching_seqlen`, `host_hit_length`, `full_kv_hit_length`, `cache_actions` |
| `InsertParams` | in | `key`, `value`, `chunked`, `priority`, `session_id` | `mamba_value` / `c128_value`(DSV4 NPU) / `prev_prefix_len`, `swa_evicted_seqlen`, `swa_branching_seqlen` / `rotation_base` |
| `InsertResult` | out | `prefix_len`, `total_len`, `last_device_node` | `mamba_exist`, `swa_branch_inserted`, `rotation_tail_declined`, `inserted_host_node`, `host_insert_dropped`, `adopted_ranges`, `cache_actions` |
| `EvictParams` / `EvictResult` | in / out | `num_tokens` | `swa_num_tokens` / `mamba_num` —— 三路独立计数 |
| `IncLockRefResult` → `DecLockRefParams` | 收据 | `node_id`, `delta` | `swa_uuid_for_lock`, `swa_uuid_for_host_lock`, `skipped_lock_components` |
| `InitLoadBackParams` | in | `best_match_node`, `host_hit_length`, `mem_quota`, `req` | — |
| `CacheRequestHandle` / `Outcome` | 生命周期 | `(rid, attempt_id)` frozen | — |

::: warning 转换缺口
`InsertParams` / `MatchResult` 里 SWA / Mamba / C128 字段是**平铺**的，不是 `dict[ComponentType, ...]`。全表唯一走 ComponentType 字典的只有 `InsertResult.adopted_ranges`。留到轮次 06 核对。
:::

## 1.2 方法契约：前置 / 成功 / 失败 / 所有权 / 异步

| 方法 | 抽象 | 前置条件 | 成功态 | 失败 / 降级态 | 所有权 | 异步 |
| --- | --- | --- | --- | --- | --- | --- |
| `match_prefix` | <Badge type="danger" text="abstract" /> | key 已含 extra_key / cache_salt / limit | 4 个 node handle + 各路 hit_length | ChunkCache 返回全 `None` + 空 tensor（约定的 miss） | **不转移**，须随后 inc_lock_ref | 同步；`cache_actions` 为待应用副作用 |
| `cache_finished_req` | <Badge type="danger" text="abstract" /> | `holds_kv`；`owned_kv_len` 由调用方传 | `[0, protected)` 存活；`[protected, owned)` 被 insert 或 free | — | **本调用全权负责** `[protected, owned)` | 同步（HiCache backup 可异步） |
| `cache_unfinished_req` | <Badge type="danger" text="abstract" /> | 分块 prefill 中途 | 更新 `req.prefix_indices` | ChunkCache 只 copy 不建树 | 保留，仅 rebind | 同步 |
| `evict` | <Badge type="danger" text="abstract" /> | — | 返回三路实际驱逐量 | ChunkCache 恒返回空 `EvictResult()` | 释放到 allocator | 同步 |
| `evict_for_alloc` | <Badge type="info" text="默认→evict" /> | — | 共享内存多 component 可提前停止 | — | 同上 | 同步 |
| `inc_lock_ref` | <Badge type="danger" text="abstract" /> | node 是 match 返回的 handle | 返回可重放收据 | ChunkCache `delta=0` | 取得保护权 | 同步 |
| `dec_lock_ref` | <Badge type="danger" text="abstract" /> | **必须传回 `to_dec_params()`** | 释放 | 收据丢失 → **欠释放（泄漏）**，而非误释他人锁 | 交还保护权 | 同步 |
| `init_load_back` | <Badge type="warning" text="raise" /> | HiCache 已挂 | `(indices, node)` | `None` = 重试准入；空 tensor = 仅辅助成功 / 回退重算 | indices 归 req | <Badge type="warning" text="跨调度轮" /> |
| `finish(handle, outcome)` | <Badge type="info" text="默认" /> | — | SUCCESS 不取消已提交异步工作 | 非 SUCCESS → `release_aborted_request` | — | 异步取消语义 |
| `finish_storage_prefetch_admission` 等 3 个 | <Badge type="info" text="默认 no-op" /> | — | — | 非 storage cache 空实现 | — | L3 异步 |
| `check_hicache_events`<br>`ready_to_load_host_cache` | <Badge type="warning" text="raise" /> | — | — | **基类 raise** —— 非 HiCache cache 被调用即崩 | — | 异步泵 |
| `flush_pending_backups` | <Badge type="info" text="默认 no-op" /> | — | 提交排队的 host backup | 无延迟 backup 者无事可做 | — | 异步泵 |
| `release_host_resources` | <Badge type="info" text="默认 no-op" /> | 优雅关停 | 幂等 | — | — | 同步但耗时敏感 |

## 1.3 能力探测：19 个方法代替 isinstance

- **形态** —— `is_chunk_cache` / `is_tree_cache` / `supports_swa` / `supports_mamba` / `supports_streaming_session` / `supports_fast_match_prefix`
- **容量** —— `evictable_size` 及 full / swa 变体、`protected_size` 及变体、`swa_transient_size`、`total_size`
- **Session** —— `release_session` / `release_radix_session` / `session_held_*` ×5
- **节点访问抽象** —— `resolve_node_handle` / `root_node_handle` / `is_backuped` / `is_root` / `get_last_hash_value` / `get_prefix_hash_values` / `rotation_base_of`

::: warning 转换未完成（源码自述）
`resolve_node_handle` 与 `root_node_handle` 的 docstring 明写 `TODO(Jialin): Remove after the Unified Radix Cache split` —— 这两个是为迁移期而存在的临时 API。
:::

## 1.4 registry 选择链：实际落到哪个实现

```text
# default_radix_cache_factory 的短路顺序
1. disable_radix && retraction_backup=="host_pool"   → UnifiedRadixCache
2. chunked_prefill && disable_radix                  → ChunkCache / PureSWAChunkCache / SWAChunkCache
3. SGLANG_EXPERIMENTAL_CPP_RADIX_TREE                → RadixCacheCpp
4. enable_unified_cache_external_linker              → UnifiedRadixCache + linker
5. hybrid_swa && full_tokens_per_layer == 0          → PureSWARadixCache
6. enable_lmcache                                    → LMCRadixCache
7. enable_flexkv                                     → _flexkv_factory
8. 默认                                               → UnifiedRadixCache
```

- `_create_unified_radix_cache` 已把异构性收敛为 `params.tree_components: tuple[ComponentType,...]`（FULL / +SWA / +MAMBA / +C128），硬件特化走 `component_registry_override`（MLX、DSV4 NPU）。
- 第 5、6、7 条分支仍是独立实现，**未收敛** —— 对应轮次 13 的责任矩阵。
- `create_tree_cache` 末尾 4 条 **UnifiedRadixCache-only 守卫**，等于显式声明这些特性只在统一树上存在：`hicache_host_memory_mode=buffer_only`、`enable_session_radix_cache`、`radix_eviction_policy=tlru`，外加 `StreamingSession` 装饰器包裹。

## 1.5 ChunkCache 作为契约下界

- `disable` 被改写成 `@property → True`，与 `PrefixCacheTrait` 的 `disable: bool` 字段在类型上冲突；源码注释 `TODO (csy): Using a prefix cache trait to replace this`。
- `PureSWAChunkCache.cache_finished_req` 是唯一手写 free 区间拼接处：需同时避开 `cache_protected_len`、`swa_evict_floor`、`swa_evicted_seqlen` 三条水位线 —— 全 SWA 模型双重释放的高发点。

---

下一轮：[轮次 02 · Scheduler 侧调用点](./round-02-scheduler)
