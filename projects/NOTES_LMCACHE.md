# LMCache 解读笔记（cheat sheet）

> 用途：未来回顾、面试故事弹药库。每条尽量"一句话结论 + 代码位置 + 可能被追问的点"。
> 不是教程，是已经懂了之后的速查。

---

## A. Python 关键概念（写 C++ 的人容易踩的）

### A.1 异常 vs 错误
- **同一个东西**。所有 `XxxError` 都是 `Exception` 子类。没有 C 文化的"错误码 vs 异常"二元论。
- 处理只有一套：`try / except / raise`。

### A.2 self、属性可见性
- `self` 等价于 C++ 的 `this`，但必须显式写——Python 方法本质是普通函数，第一参数是实例。
- 写：`self.x = 1` **永远成功**（往 `obj.__dict__` 加键）
- 读：`self.never_set` **抛 `AttributeError`**——不是返回 None
- `_xxx` 是约定 private，解释器不阻止，社区约定别碰

### A.3 async 关键字 vs asyncio 库
- `async`/`await` 是**语言关键字**（语法）
- `asyncio` 是**库**（事件循环实现 + 原语）
- 关系：`async def` 产出协程对象，需要事件循环驱动；asyncio 是事实标准的那个事件循环

### A.4 装饰器
- `@dec` 等价于 `def f(): ...; f = dec(f)`
- LMCache 里见过的：
  - `@_lmcache_nvtx_annotate` —— NVTX 区间标记，给 Nsight 看，**只观测不改语义**
  - `@torch.inference_mode()` —— 关 autograd 上下文
  - `@abc.abstractmethod` —— 抽象方法，子类必须实现

### A.5 引用计数 + 循环 GC
- Python 主力是 ref count，**对环不工作**
- 有补充的 cyclic GC（mark-sweep）专门收环
- LMCache 不依赖自动 GC——**显式 `ref_count_up/down`**（同 C++ `shared_ptr`）
- 原因：GC 触发时机不可控、pinned memory 稀缺、跨线程意图要写出来

---

## B. 协程 vs 线程 vs syscall（最大的认知校准点）

### B.1 协程不是线程
- 一个 event loop = 一个 OS 线程
- 所有协程在这一个线程里挤
- `await` 时把状态存进协程对象（局部变量、PC），让出控制权给 event loop
- **不创建/挂起任何 OS 线程**

### B.2 GIL 释放 ≠ event loop 让出
- 这是**最关键的误解纠正**
- GIL 在 syscall 期间释放 —— 给**别的 OS 线程**抢
- event loop 让出 —— 协程显式 `await` 才发生，给**同 loop 的别的协程**
- 两件事完全独立

### B.3 为什么必须线程池
- 直接在 event loop 线程里调 `os.read(fd, n)`：OS 把线程挂起 → event loop 跟着挂 → 所有协程饿死
- GIL 是放了，但救不了同 loop 的其他协程（它们都在这个被挂起的线程上）
- 所以**磁盘 IO 必须丢线程池**——这就是 `disk_worker.submit_task` 存在的物理原因
- 网络 IO 可以用 selector + 非阻塞 socket，磁盘 IO 没有 POSIX 等价物（io_uring 是 Linux 新方案）

### B.4 协程唤醒机制
- **OS 事件多路复用**（selector / epoll / kqueue）—— socket/pipe/fd 类资源
- **显式 Future 完成** —— 另一协程或线程调 `future.set_result()`
- LMCache 走第 2 种：`asyncio.run_coroutine_threadsafe` 把任务塞进 event loop

---

## C. LMCache 架构层次

```
vLLM Scheduler / Worker
        ↓
LMCacheConnector（vLLM 侧适配层）
        ↓
LMCacheEngine（cache_engine.py） ← 对外动词：lookup / retrieve / store
        ↓
StorageManager（storage_manager.py） ← 多 tier 编排
        ↓
LocalCPU / LocalDisk / GDS / P2P / Nixl / Remote backends
        ↓
csrc/ (C++) + rust/raw_block/ (Rust) ← 真正字节搬运
        ↓
GPU HBM / DRAM / NVMe / 网络
```

**关键认知**：Python 这层只做编排和元数据，**不碰硬件**。

---

## D. `LocalDiskBackend` 设计要点

### D.1 元数据/数据分离
- 元数据：`self.dict: dict[CacheEngineKey, DiskCacheMetadata]`，全内存
- 数据：磁盘文件，每 key 一个文件
- 查存在性 O(1) 内存操作，**完全不碰盘**

### D.2 锁粒度（关键设计）
- `disk_lock` 只罩 `self.dict` 的 mutation
- **真正的 IO 永远在锁外**（read/write 不持 `disk_lock`）
- 临界区微秒级
- 读和写**对称**：两边都是"锁内查元数据/更新 dict，锁外做 IO"

### D.3 文件并发安全（怎么不需要文件锁）
- 每 key 一个文件 → 不同 key 写不同 inode，OS 天然安全
- 同 key 不会并发写：`put_tasks` 集合去重
- 同 key 不会 put + remove 冲突：未落盘前 key 不在 dict，淘汰策略选不到
- 同 key 不会 get + remove：pin 机制阻止淘汰
- 同 key 并发 get：POSIX 多 reader 安全

### D.4 双重生命周期管理
- **`MemoryObj.ref_count_up/down`**：保护 CPU 内存不被回收
- **`DiskCacheMetadata.pin/unpin`**：保护磁盘条目不被淘汰
- 两套独立、互不影响

### D.5 inflight 去重
- `LocalDiskWorker.put_tasks: List[CacheEngineKey]`
- `submit_put_task` 检查：已在飞 → 直接 return
- `insert_key` 检查：已在 dict → no-op
- 双层兜底

---

## E. 调度（LMCache 不是请求调度器）

### E.1 两层调度
- **请求调度**（哪个 request 进 batch、何时 prefill）= vLLM scheduler，**不在 LMCache 范围**
- **IO 任务调度** = `AsyncPQThreadPoolExecutor` 的优先级队列
  - `prefetch` 优先级 0（最高）—— 有人等
  - `delete` 优先级 1 —— 腾位置
  - `put` 优先级 2 —— 没人等
- 读路径优先于写路径，标准设计

### E.2 lookup → retrieve → store 三动词
- `lookup(tokens)` → **返回命中 prefix token 数**，只查元数据，便宜
- `retrieve(tokens)` → 把数据从 tier 搬到 vLLM GPU KV cache，贵
- `store(tokens, mask)` → 把新产生的 KV 写到 LMCache，异步
- lookup 和 retrieve 分开是 scheduler 硬需求：lookup 答"能拿多少"，scheduler 据此决定要不要拿

### E.3 store 的 mask
- mask = `[F × N_prefix, T × N_new]`
- `token_database.process_tokens` 跳过 False 段（420-421）
- 所以 **prefix 部分不会重新写** —— 即使上层传了也有 `insert_key` 去重兜底

---

## F. content-addressed cache 不变量

- `CacheEngineKey` 是 (token 序列 + 配置) 的 hash
- 同 token 序列 → 同 hash → 同 key
- 同 token 序列 + 同模型 → 同 KV（transformer forward 确定性）
- 所以 **key 是 value 的内容指纹**
- 推论：**永远不需要"更新一个 key"**，需要不同内容就是不同 key
- 同 git object、IPFS、Docker layer 一类设计

## G. prefix caching 命中模式

- **逐 token 字节级严格相同**才能命中
- 一个 token 不同 → 后面全 miss（key 是 prefix hash）
- 实际命中场景：
  - 共享 system prompt（千用户同 chatbot）
  - 多轮对话（前 N-1 轮固定）
  - RAG 固定 doc + 用户 query
  - few-shot prompt 固定 examples + 变化 query
  - code completion 同文件
- 命中率两极：聊天/RAG 60-95%；开放 query 接近 0%
- LMCache **不解决语义相似**（"明天天气" vs "明天的天气"命中率 0）

---

## H. 工业模式（面试时容易讲的"工程素养"点）

| 模式 | LMCache 位置 | 同款 |
|------|-------------|------|
| 事件总线 | `kv_events`（store 521-548） | Kafka producer |
| 批量消息 | `BatchedMessageSender`（178-187） | Nagle、DB WAL、Kafka |
| 协程 + 优先级队列 + 线程池三层 | `AsyncPQThreadPoolExecutor` | OS scheduler、DB query planner |
| 完成回调 | `on_complete_callback`（311） | continuation passing style |
| 回调异常本地化 | 565-569 try/except 包回调 | 异步系统硬要求 |
| Pin 引用计数 | `pin`/`unpin` | Linux page pin、CUDA mem_lock |
| 多 tier 回填 | `SM.get` 命中后写回 LocalCPU（450-455） | CPU inclusive cache |

---

## I. 死锁实例（具体可讲的故事）

`local_disk_backend.py` 257-262 那段**被注释掉的代码**就是死锁现场：

```python
# NOTE: The following code will cause deadlock
# res = asyncio.run_coroutine_threadsafe(
#     self.disk_worker.submit_task("delete", os.remove, path),
#     self.loop,
# )
# res.result()
```

死锁拼图：
1. T1 持有 `disk_lock`（在 `submit_put_task` 的 evict 循环里）
2. T1 调 `res.result()` 同步等线程池跑 `os.remove`
3. 线程池 N 个 slot 全在跑 `async_save_bytes_to_disk`
4. 它们写完后要 `insert_key` → 需要 `disk_lock`
5. T1 拿着 `disk_lock` 等线程池
6. 线程池等 `disk_lock` 在 T1 手里
7. 死锁

**总结公式**：持有锁 + 同步等待最终需要这把锁的资源 = 死锁。

修复（264 行）：直接 `os.remove(path)` 同步调用，syscall 期间 GIL 释放但锁还在。

通用避免：
- 不要在锁里 await
- 不要在锁里同步等线程池
- 锁嵌套顺序一致

---

## J. ABI vs ABA（容易混的两个）

### ABI（Application Binary Interface）
- 二进制层接口约定：调用约定、struct 布局、name mangling、异常表
- API 兼容 ≠ ABI 兼容
- LMCache 里：pybind11 / C 扩展依赖 CPython C API ABI；CUDA 版本（cu121 vs cu129）也是 ABI 话题

### ABA 问题
- 无锁数据结构的陷阱
- T1 读到 A，CAS 准备改 A → C；T2 改 A→B→A；T1 CAS 成功，但中间状态变了
- 经典：lock-free stack pop 看到的 top 不再是同一个对象，use-after-free
- 解法：tagged pointer（版本号）、hazard pointer、epoch-based reclamation、RCU
- **LMCache 不踩这个**：有锁路线 + 微秒级临界区

口诀：**B**inary / **A→B→A**

---

## K. JD 对照（自查清单）

### LMCache 能覆盖
- ✅ 推理一体化分层存储
- ✅ vLLM 集成完整案例
- ✅ TTFT 优化（lookup 命中 → 跳 prefill）
- ✅ KVCache 优化（主营业务）
- ✅ O_DIRECT（LocalDiskBackend）
- ✅ GPUDirect（GDSBackend）
- ✅ RDMA（NIXL/Mooncake/Infinistore）
- ✅ io_uring（rust/raw_block/）
- ✅ uring_cmd（rust 测试有）

### LMCache 不覆盖
- ❌ 训练侧 checkpoint 存储
- ❌ 万卡分布式存储
- ❌ CXL
- ❌ "主导大型分布式系统"（LMCache 是 cache，不是 DFS）

---

## L. 我尚未读过、不确定的点

- `csrc/` 内字节搬运的具体实现
- `gpu_connector/` 怎么和 vLLM 的 KV cache 对接
- `transfer_channel/` 抽象的具体形态
- 9 种 RemoteConnector 的具体差异
- `cache_controller/` 跨实例协调协议
- `batched_get_blocking` 在 LocalDiskBackend 文件里没见到，可能基类默认
- vLLM 那侧 PagedAttention 怎么调 retrieve 把数据放进 GPU
