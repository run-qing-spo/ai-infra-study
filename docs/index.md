---
layout: home
hero:
  name: AI Infra Study
  text: AI 推理与基础设施学习笔记
  tagline: Transformer 推理原理 · 硬件架构 · IO 优化
  actions:
    - theme: brand
      text: 推理基础：原理与硬件
      link: /infra/inference-fundamentals
    - theme: alt
      text: 推理 IO 优化：技术全景
      link: /infra/inference-io-tech-complete
    - theme: alt
      text: GitHub
      link: https://github.com/run-qing-spo/ai-infra-study
features:
  - title: 推理原理
    details: 从 Tokenizer 到 Scheduling，理解框架为何调用 flash_attn、all_reduce、paged_attention 等接口。
  - title: 硬件架构
    details: GPU、HBM、NVLink、NIC、PCIe、NVMe、CXL 的内部结构与节点拓扑。
  - title: IO 优化
    details: 将计算步骤与物理数据路径对应，作为推理 IO 优化的起点。
---

## 目录

### 基础设施

- [推理基础：原理与硬件](/infra/inference-fundamentals) — Part 1 Transformer 推理原理 · Part 2 硬件组件内部结构
- [推理 IO 优化：完整技术全景](/infra/inference-io-tech-complete) — 芯片内到跨数据中心的 IO 技术栈
- [KV Cache C++ Backend 优化方向细则](/infra/kv-cache-cpp-backend-direction) — 推理推动力 → C++ backend 服务 → 技术实现 → 对位与切入点

### AI 存储

- [项目总览](/projects/overview) — 围绕 KV Cache 存储的 10 个渐进式 C++ 小项目（单层缓存 → 4 层完整路径）路线图
- [项目 1 · Thread-safe LRU Cache](/projects/project1_lru/README) — 16 线程并发 LRU，`-fsanitize=thread` 0 race
- 背景知识（服务 P1/P2）：
  - [多线程缓存设计综述](/algorithms/concurrent-cache) — Sharding、全局锁、不同算法的并发方案
  - [LRU/ARC/LFU 算法对比](/algorithms/lru-arc-lfu-comparison) — 原理、性能对比、适用场景

### C++

- [C++ 异常安全 与 数据安全](/cpp/exception-safety) — 三层异常保证、RAII 与持久化场景的边界
- [C++ 定义、声明与实现](/cpp/forward-declaration-vs-complete-definition) — 三层递进 "用到什么给到什么"，按需暴露减少头文件依赖与编译耦合
- [C++ 原子操作内存序](/cpp/atomic-memory-order) — relaxed / acquire / release / seq_cst 怎么挑，对照 project2 SPSC 实战

### AI

（待补充）
