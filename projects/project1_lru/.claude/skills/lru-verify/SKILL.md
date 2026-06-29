---
name: lru-verify
description: 当用户修改 include/lru_base.hpp 或 include/lru_mutex.hpp、新增 LRU 变体、或要确认 LRU 实现正确性时使用。运行单线程功能 + audit 不变量测试，以及 ThreadSanitizer 构建，确认改动既不破坏算法也不引入数据竞争。
metadata:
  type: skill
---

# lru-verify

改动 LRU 实现后，按本 skill 走一遍验证，再向用户报告结果。

## 何时触发

- 修改了 `include/lru_base.hpp` 或 `include/lru_mutex.hpp`
- 新增 LRU 变体（例如 lru_lfu、lru_arc）
- 用户明确要求「验证 LRU 正确性」「跑测试」「检查并发安全」

## 验证步骤

工作目录是项目根 `compare_project1_lru/`。两条都要跑，缺一不可：

1. **单线程功能 + 不变量**
   ```
   make test
   ```
   覆盖 `test/test.cpp`，包含 `audit()` 在每个操作后的不变量检查。

2. **多线程数据竞争**
   ```
   make test-tsan
   ```
   覆盖 `test/test_mutex.cpp` 的 `ConcurrentMix`，用 ThreadSanitizer 重新编译并运行。

两条全绿，才算通过。

## 坑点清单（Gotchas）

只列 Claude 读代码推断不出来的：

- **`audit()` 是条件编译的**：依赖 `-DLRU_TEST_HOOKS`，Makefile 默认带了。**不要**绕开 Makefile 用 `g++ test.cpp ...` 单独编译，否则 audit 调用会编译失败或者被静默跳过。
- **必须两条都跑**：`make test` 跑过 ≠ 线程安全；`make test-tsan` 跑过 ≠ 算法正确。改了 `lru_base.hpp` 的人最容易只跑前者就交差，结果 `lru_mutex` 的并发不变量被悄悄破坏。
- **`ConcurrentMix` 是随机的**：用 `std::mt19937(i)` 做种但跨线程交错不可控，**偶发失败要连跑 3 次**再下结论。判定规则：
  - TSan 报 race → 锁/共享状态有 bug
  - TSan 干净但断言失败 → 底层算法 bug（不是并发问题）
  - 偶现成功偶现失败且无 TSan 报告 → 大概率算法 bug 在某些交错下才暴露，不要当 flaky 忽略
- **新加 public 方法时**：必须同时在 `lru_mutex.hpp` 里加对应包装（`std::lock_guard` + 转发），否则并发用户绕过锁直接访问 `lru_base_`。`lru_mutex` 的契约是「所有出参都是 `shared_ptr<V>` 值拷贝」，绝不能漏出 `Node&` / 内部迭代器。
- **边界 case 必须跟着加测试**：容量为 1、erase 后立刻 push、push 同 key 覆盖、装满后再覆盖 —— 这四个在 `test.cpp` 里都有原型，新增方法漏掉任一个，free list 状态很容易错。
- **`make test-tsan` 产物在 `build/test_tsan_bin`，和 `build/test_bin` 分开**：不要复用同一个二进制，TSan 需要重新插桩编译。

## 报告格式

向用户汇报时，明确说清楚：
- 跑了哪两条命令
- 各自结果（通过 / 失败的具体 test name）
- 如果 ConcurrentMix 偶发失败，重复跑了几次、各次结果

不要只说「测试通过」就完事。
