---
C++ 异常安全 与 数据安全
---

# C++ 异常安全 与 数据安全

::: tip 本项目契约
默认目标：**basic guarantee + RAII**。接口注释里显式声明保证级别。
strong guarantee 只在少数事务性接口上做；nothrow 只标在 dtor / move / swap 等真正不抛的地方。
:::

---

## 1. 三个保证级别

异常安全是单次函数调用范围内对"对象状态"的承诺。三档：

### 1.1 nothrow（no-throw guarantee）
- 永不抛。用 `noexcept` 标注，编译期可见。
- 典型：析构函数、`swap`、`move` 构造/赋值、简单 getter。

### 1.2 strong（commit-or-rollback）
- 要么成功，要么对象状态完全不变。
- 典型：`std::vector::push_back`（单元素）、事务接口。
- 实现代价：通常要"先做副本，成功后 swap"。

### 1.3 basic（no leak + invariants）
- 不漏资源、不变量成立、对象可继续使用。
- 状态**可能被部分修改**，数据可能丢一部分。
- **本项目默认。** 工业界绝大多数代码停在这一档。

---

## 2. RAII：异常安全的核心机制

不是 `try/catch`，是 RAII。

- 资源生命周期与对象生命周期绑定 —— 申请在构造、释放在析构。
- 栈展开时析构必然被调用，所以异常路径自动释放。
- 工具：`unique_ptr`、`shared_ptr`、`lock_guard`、`fstream`、自定义 RAII 包装。
- **为什么 RAII 让 basic guarantee 几乎免费**：不漏资源这一档，全靠 RAII 替你兜底。

---

## 3. 本项目契约

### 3.1 默认
- 公共接口默认提供 basic guarantee。
- 注释里用一行声明保证级别（例：`// strong guarantee; rolls back map on failure`）。

### 3.2 何时升级到 strong
- 对外契约严格的容器接口。
- 状态难以回滚的事务性操作。
- 已经"几乎是 strong" 的小代价场景（重排可抛操作即可达到）。

### 3.3 何时标 nothrow
- 析构函数（C++11 起隐式 noexcept，不要破坏）。
- move 构造 / move 赋值（让 STL 容器走优化路径，强相关）。
- `swap`。
- 性能关键路径上**确实**不抛的简单函数。

---

## 4. 抛出点速查

### 4.1 编译器/运行时保证不抛
- `operator delete`
- 析构函数（隐式 noexcept；显式标 `noexcept(false)` 几乎不该出现）
- trivial 类型的 swap / move
- C 风格 API（用 errno / 返回值报错）

### 4.2 一定可能抛
- `operator new`（默认形式）→ `std::bad_alloc`
- 增长性 STL 操作：`vector::push_back / reserve / resize`，`unordered_map::insert / operator[] / rehash`，`string::append`
- `at()` 越界 → `std::out_of_range`
- `dynamic_cast<T&>` 失败 → `std::bad_cast`（指针版返回 nullptr，不抛）
- `std::stoi / std::stod` 解析失败
- `std::regex`、`std::thread`、`std::async` 构造

### 4.3 取决于用户类型
- `std::hash<K>::operator()`（内置类型 noexcept）
- `K` 拷贝 / 移动 / 比较
- 比较器 / 谓词 / 回调

### 4.4 反直觉点
- `new T(args)` 中 `T` 构造抛了，**已分配的内存自动释放**（标准保证）。
- `new T[n]` 中第 k 个元素构造抛了，**前 k-1 个会被析构 + 内存释放**（标准保证）。
- `delete nullptr` 是 well-defined 的 no-op。

---

## 5. Linux 上的"真实抛点"

### 5.1 overcommit + demand paging
- `mmap` / `brk` 只更新 VMA，**不分配物理页**。
- 物理页等真正访问触发 page fault 时才分配。
- 默认 `vm.overcommit_memory = 0`，乐观策略 —— 进程申请远多于使用，内核押注。

### 5.2 物理内存耗尽的流程
- page fault handler 找不到物理页 → 触发 reclaim（drop page cache / swap / 压缩）。
- reclaim 失败 → **OOM killer**：按 `oom_score` 排序，对受害者发 `SIGKILL`。
- 进程立即终止，析构函数不跑。

### 5.3 为什么没有 C++ 异常
- 异常是用户态语言机制，unwind 表在用户态。
- SIGKILL 不可捕获、不可阻塞、不可忽略。
- 触发 page fault 的那条指令不会再返回用户态，没有"抛出"的机会。

### 5.4 `bad_alloc` 真会抛的少数场景
- `vm.overcommit_memory = 2`（strict accounting）
- 单次分配命中 RLIMIT_AS / RLIMIT_DATA / 虚拟地址上限
- 命中 cgroup `memory.max` 时**部分情况**（常见的还是 cgroup-OOM 直接 SIGKILL）
- 嵌入式无 swap、关 overcommit 的系统

> 结论：服务进程上认真处理 `bad_alloc` 性价比低 —— **真出问题大概率是 SIGKILL，根本到不了 catch**。

---

## 6. "不抛异常" ≠ "可靠"

异常安全只管单次函数调用范围内的对象状态，跟"程序在故障下不丢数据"是两个层面。

### 6.1 有信号但不是 C++ 异常
- SIGKILL（OOM、`kill -9`、cgroup 杀）
- SIGSEGV（空指针、UAF、栈溢出）
- SIGBUS（mmap 文件被 truncate、对齐错误）
- SIGABRT（assert 失败、glibc 检测到 double-free）

可以装 signal handler，但 handler 里只能调 async-signal-safe 函数（连 `malloc` 都不能），实际能做的就是写日志然后 abort。

### 6.2 完全没有信号的失败
- 数据竞争产生错误值（程序"正确地"算出错的结果）
- 没 ECC 时的内存位翻转
- `write()` 返回成功但没 fsync，掉电后数据没了
- 网络分区，对端"成功"但你没收到 ack

### 6.3 外部失败
- 内核 panic、hypervisor 崩、断电、机房失火、磁盘坏道

---

## 7. 数据安全靠分层撑起来

不是单一语言机制能搞定的，要分层：

| 目标 | 机制 |
|------|------|
| 持久性（committed 不丢） | WAL + `fsync` / `fdatasync`，journaling |
| 完整性（能发现损坏） | CRC32 / xxhash / Merkle tree |
| 可用性（节点死了仍服务） | 多副本，Raft / Paxos / 主从 |
| 可恢复 | snapshot、backup、PITR |
| 操作安全 | 幂等接口、退避重试、断路器 |
| 检测 | 监控、告警、chaos engineering、fault injection |
| 纵深防御 | 测试 + sanitizer + fuzz + code review |

**核心心智**：假定进程会在任何指令处死掉（crash-only design），靠重启 + 持久层 + 副本恢复状态。**不指望进程内做"优雅降级"**。

---

## 8. KV cache 的角度

KV cache 内容 = f(input tokens, 模型权重) —— **可再生**。所以：

- **不需要** WAL / fsync / 复制给缓存本身。
- **需要** 校验和：缓存损坏返回错的 KV → 模型生成错的 token → 静默错误，比 crash 糟糕得多。
- **需要** crash 重启策略：要么全清重建，要么严格验证持久索引才复用。

跟数据库不一样 —— 数据库不能说"算了从头来"，KV cache 可以。

---

## 9. noexcept 标注的判断

### 9.1 应该标
- 析构（默认就是，别破坏）
- move 构造 / move 赋值（影响 STL 容器的优化路径）
- `swap`
- 真正不抛的简单接口

### 9.2 别乱标
- 标了之后一旦抛，直接 `std::terminate`，没有 catch 机会。
- 标 noexcept 是契约，破坏代价比 strong 还高。
- 不确定时用条件 noexcept：`noexcept(noexcept(expr))`。

---

## 10. 实战 checklist：修改状态的函数

写一个会修改对象状态的非 const 成员函数时，按顺序检查：

1. **列出所有可能抛的操作**（参考第 4 节）。
2. **把可抛操作前置**，把"修改不变量"的操作后置。
3. 中间用 **RAII 包临时资源**（unique_ptr、lock_guard、scope_guard），即使抛了也自动释放。
4. 优先用 `std::move` 让赋值变 noexcept。
5. 必要时显式 try-rollback —— **代价高，谨慎选用**。
6. 在注释里声明保证级别。

---

## 11. 反模式

- `catch(...)` 吞异常不处理（"反正没人看 log"）。
- 析构函数抛。
- 构造函数中途抛却留下半成品（用 RAII 成员替代裸资源即可避免）。
- 持锁路径上 lock 不是 RAII 的，抛了死锁。
- 用异常做正常控制流（慢、难读、违背约定）。
- 标了 noexcept 又调可能抛的函数，靠"应该不会真抛吧"。

---

## 12. 参考

- Sutter, *Exceptional C++* / *More Exceptional C++*
- Stroustrup, *The C++ Programming Language* (异常章节)
- Google C++ Style Guide（反对异常的工程论点）
- Linux kernel docs: `Documentation/vm/overcommit-accounting.rst`
- cppreference: [Exception safety](https://en.cppreference.com/w/cpp/language/exceptions)

---

*本文档随项目推进迭代；遇到新场景就回来补案例。*
