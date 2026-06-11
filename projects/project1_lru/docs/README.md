# 设计 Tradeoff 文档

本目录记录 `project1_lru` 中关键的设计取舍，侧重**为什么这样选**而非**怎么实现**。每篇文档对应一个具体决策点，便于后续回顾和扩展。

| # | 文档 | 摘要 |
|---|------|------|
| 1 | [组合 vs 继承：LRUCache 与 LRUCacheBase](composition-vs-inheritance.md) | 线程安全包装层为何用成员组合而非 public 继承 |
| 2 | [特殊成员函数与错误处理策略](special-members-and-error-handling.md) | 拷贝删除 / 移动保留的理由；Debug assert / Release terminate |
| 3 | [ShardedLRUCache 容量契约与参数校验](sharded-capacity-and-validation.md) | 分片数边界、严格总容量上限与非法参数处理 |
