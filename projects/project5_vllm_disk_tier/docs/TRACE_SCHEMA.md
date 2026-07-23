# 请求级测量的 ID 与连接关系

本项目不把 vLLM 各层原生的 `request_id` 当成同一个值。vLLM 0.24.0 的
completion serving、AsyncLLM 和 EngineCore 会分别包装或随机化 ID；强行删前后缀
会掩盖一对多关系，并且在 `n > 1` 或多 prompt 请求下产生错误连接。

## 主键

- `run_id`：一次 serve lifecycle。runner 使用 `results目录名:group名`，跨多夜不重复。
- `req_id`：一次客户端请求，由 `(run_id, workload logical position)` 确定性生成。
- `(run_id, tier, job_id)`：一次 secondary-tier job。

`req_id` 形如 `kvt-<32位hex>`。客户端通过 `X-Request-Id` 传入；vLLM 0.24.0
的 `_base_request_id()` 明确优先使用这个 header。服务端保留以下映射：

```text
req_id
  └─ response_id                 cmpl-<req_id>
      └─ vllm_external_req_id    cmpl-<req_id>-0
          └─ vllm_internal_req_id  ...-<8位随机后缀>
              └─ ReqContext.req_id
                  └─ JobMetadata.req_context.req_id
```

## 数据粒度与连接键

| 文件 | 粒度 | 主键/连接键 | 能否请求级归因 |
|---|---|---|---|
| `group_<group>.json` | request | `req_id` | 是，客户端 TTFT |
| `request_map_<group>.jsonl` | request-ID mapping | `req_id`、native IDs | 是，连接 client 与 EngineCore |
| `tier_stats_<group>.sched.records.jsonl` | tier job | `req_id + job_id` | 是，fs/uring 对称 job sojourn |
| `tier_stats_<group>.records.jsonl` | uring engine job | `job_id` → scheduler ledger | 间接，可以回连 `req_id` |
| `tier_stats_<group>.json` | run snapshot | `run_id` | 否，只是整组计数/状态 |
| `iostat/pidstat/meminfo` | run time series | `run_id + wall-time window` | 否，只能作窗口级诊断 |
| vLLM `/metrics`、serve interval log | run/window aggregate | `run_id + scrape/window` | 否，不能伪装成请求级数据 |

聚合表不写虚假的 `req_id`。它们通过 `run_id` 和时间窗口关联到实验 cell，只作诊断
证据。请求机制结论只能来自 client、request map 和 scheduler/tier event 链。

## 第一阶段自动验收

运行：

```bash
python3 bench/trace_join_audit.py results_e2e_<timestamp>
```

验收器检查客户端 ID 唯一性、外部→内部映射、tier job 外键、run ID 一致性、
success 字段、单调时钟顺序，以及 uring C++ job 能否回连 scheduler ledger。任何
核心外键缺失都返回非零退出码，不能进入正式统计。
