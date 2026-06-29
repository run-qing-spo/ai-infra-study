# CLAUDE.md

## 1. Project Overview

这是一个**学习用**的 C++ LRU 缓存项目，对比单线程版与加锁版的实现差异。
- 技术栈：C++17 + GoogleTest + ThreadSanitizer
- 构建：Makefile（无 CMake，无包管理器）
- 角色：作者是在 study/practice 阶段，代码里大量中文笔记承载思考过程

## 2. Commands

工作目录就是项目根，所有命令在根目录执行：
- 跑功能 + 不变量测试：`make test`
- 跑数据竞争测试：`make test-tsan`
- 重建 compile_commands.json：`make compdb`
- 清理产物：`make clean`

## 3. Architecture

- `include/lru_base.hpp`：核心 LRU，侵入式双向链表 + free list + 哨兵节点
- `include/lru_mutex.hpp`：薄锁包装，所有公开方法 `std::lock_guard` 转发
- `include/lru_base_v2.hpp`：早期版本，仅做对比留存
- `test/test.cpp`：单线程功能 + audit 不变量
- `test/test_mutex.cpp`：并发 stress
- 详细设计思路见 `docs/lru_base_learning_notes.md`，**不要**把它的内容复制进本文件

## 4. Conventions

- 头文件全部 `.hpp` + `#pragma once`，模板实现写在头文件里
- 命名空间：`lru_base`、`lru_mutex`，新增变体起新命名空间，不要混
- 公开方法返回值是 `std::shared_ptr<V>` 的**值拷贝**，绝不返回 `Node&` 或内部迭代器
- 新增 public 方法时，`lru_mutex.hpp` 必须同步加锁转发

## 5. Hard Constraints

- **保留所有原有注释**，包括中文笔记、问号注释（如 `// 为什么不需要new？`）、TODO、看似冗余的解释。
  *Why*：这是学习项目，注释承载作者的思考过程和疑问，不是代码噪音。
  *边界*：只有当注释明显描述了已不存在的逻辑时才能删，且必须在回复里写明「删除了 X 注释，原因是 Y」。
- **不要绕开 Makefile** 用 `g++ ... test.cpp` 单独编译。`audit()` 依赖 `-DLRU_TEST_HOOKS`，绕开 Makefile 会编译失败或静默跳过不变量检查。
- **不要把 `lru_base_v2.hpp` 当当前实现改**，它只是历史对比版本。

## 6. Gotchas

- **改了 `lru_base.hpp` 或 `lru_mutex.hpp` 之后**：调用 `.claude/skills/lru-verify/`，跑 `make test` + `make test-tsan` 两条线再汇报，缺一不可。
- **`ConcurrentMix` 偶发失败**：随机交错不可控，连跑 3 次再下结论。判定：TSan 报 race = 锁 bug；TSan 干净但断言失败 = 算法 bug；不要当 flaky 忽略。
- **哨兵布局**：`used_head_sentinel_ = capacity`、`used_tail_sentinel_ = capacity+1`、free list 用 `-1` 当尾；`pool.reserve(capacity+2)` 多出来的两个就是干这个的。读代码看不出来这个约定。
- **free list 是单向的**：只走 `.next`，free 节点的 `.prev` 是垃圾值，**不要读**。
- **`push()` 三段判断顺序不能换**：先查 key2idx 命中（覆盖路径）→ 再查 free list 是否满（触发 evict）→ 最后插入。
- **`eraseByIdx` 顺序**：必须先 `key2idx.erase`，再改 pool 指针。反过来在某些 K 类型的 hash 上会出问题。
