# SGLang Prefix Cache 转换审计

这是一个逐轮推进的源码审读专栏，对象是 SGLang 的 `python/sglang/srt/mem_cache`，主线是 `BasePrefixCache` 这套抽象契约的「转换」完成到什么程度 —— 哪些异构性（SWA、Mamba、C128、HiCache）已经收敛进统一接口，哪些还平铺在参数对象里、或者干脆跑在契约之外。

每轮只读几个文件，结论回头对照轮次 01 的契约表。标「转换缺口」的条目是跨轮追踪项，后面的轮次要么证实要么销账。

基线 commit `76f9213a41`，分支 `main`。

## 进度

| 轮次 | 主题 | 主要文件 | 状态 |
| --- | --- | --- | --- |
| [01](./round-01-contract) | BasePrefixCache 接口契约 | `base_prefix_cache` · `registry` · `cache_init_params` · `chunk_cache` | 已完成 |
| [02](./round-02-scheduler) | Scheduler 侧调用点 | `scheduler` · `schedule_policy` · `schedule_batch` · `mem_cache/common` | 已完成 |
| [03](./round-03-tree-core) | 建树与 match / split / insert | `unified_cache/unified_tree_core.py` | 已完成 |
| [04](./round-04-eviction) | eviction / LRU / lock_ref | `unified_tree_core` · `evict_policy` · `components/full` · `utils` | 已完成 |
| 05 | 请求级生命周期 | — | 待读 |
| 06 | components | — | 待读 |
| 07 | memory_pool | — | 待读 |
| 08 | allocator | — | 待读 |
| 09 | 容量规划 | — | 待读 |
| 10 | HiCache | — | 待读 |
| 11 | host 池 | — | 待读 |
| 12 | L3 抽象 | — | 待读 |
| 13 | 旁路实现 | — | 待读 |
| 14 | external linker | — | 待读 |

## 跨轮追踪项

这几条是已经记下、但要等后面轮次才能结案的缺口：

- `InsertParams` / `MatchResult` 里 SWA / Mamba / C128 字段是平铺的，不是 `dict[ComponentType, ...]` —— 留到轮次 06 核对。
- `resolve_node_handle` / `root_node_handle` 的 docstring 自述是迁移期临时 API（`TODO(Jialin): Remove after the Unified Radix Cache split`）。
- registry 里 PureSWARadixCache、LMCRadixCache、`_flexkv_factory` 三条分支仍是独立实现 —— 对应轮次 13 的责任矩阵。
- Scheduler 的 prefetch 状态机完全跑在基类契约之外 —— 留到轮次 10、12。
- `node.backuped` / `node.evicted` 只看 FULL component，辅助 component 只剩 tombstone 的节点仍被判为「在设备上、已备份」。
- ~~`_split_node` 会刷新 `child.last_access_time`~~ —— 轮次 04 结案：影响仅限 FULL（堆键读时间戳），辅助 component 原位插入不受影响，`fifo` / `filo` 中性。
- ~~`full_host_duplicates` 的注销是懒的~~ —— 轮次 04 结案：`_can_reclaim_full_host_duplicate` 三项复检（含 `load_back_pending_id`）齐备，陈旧条目遍历后统一 pop。
- `evict_policy.py` 的 8 种策略只对 FULL 生效，SWA / Mamba 恒定 LRU —— 轮次 06 看 component 侧是否有意如此。
- `dec_swa_lock_only` 没有配对的 inc，靠 `skipped_lock_components` 兜底 —— 轮次 05 在请求级生命周期里核对配平。
