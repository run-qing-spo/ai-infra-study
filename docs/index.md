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

- [项目总览](/projects/overview) — KV Cache 分层卸载 + 异步 Checkpoint，两个动手项目的路线图与进度追踪

### 算法与数据结构

- [多线程缓存设计综述](/algorithms/concurrent-cache) — Sharding、全局锁、不同算法的并发方案
- [LRU/ARC/LFU 算法对比](/algorithms/lru-arc-lfu-comparison) — 原理、性能对比、适用场景

### AI

（待补充）
