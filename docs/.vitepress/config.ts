import { defineConfig } from 'vitepress'
// @ts-ignore - no bundled types
import taskLists from 'markdown-it-task-lists'
import { withMermaid } from 'vitepress-plugin-mermaid'


export default withMermaid(
  defineConfig({
  title: 'AI Infra Study',
  description: 'AI 推理与基础设施学习笔记',
  base: '/ai-infra-study/',
  lang: 'zh-CN',
  markdown: {
    config: (md) => {
      md.use(taskLists)
    },
  },
  themeConfig: {
    outline: 'deep',
    nav: [
      { text: '首页', link: '/' },
      { text: '基础设施', link: '/infra/inference-fundamentals' },
      { text: '硬件', link: '/hardware/ssd' },
      { text: 'AI 存储', link: '/prefix-cache/' },
      { text: 'C++', link: '/cpp/exception-safety' },
    ],
    sidebar: {
      '/infra/': [
        {
          text: '基础设施',
          items: [
            {
              text: '推理基础：原理与硬件',
              link: '/infra/inference-fundamentals',
            },
            {
              text: '推理 IO 优化：技术全景',
              link: '/infra/inference-io-tech-complete',
            },
            {
              text: 'KV Cache C++ Backend 优化方向细则',
              link: '/infra/kv-cache-cpp-backend-direction',
            },
            {
              text: 'SGLang 0.5.18 KV Cache 源码剖析',
              link: '/infra/sglang源码剖析',
            },
            {
              text: 'SGLang Prefix Cache 技术分析',
              link: '/infra/sglang-kv-cache-summary',
            },
            {
              text: 'SGLang Unified Radix Tree：match / split / insert',
              link: '/infra/unified-radix-tree-kv-cache',
            },
            {
              text: 'SGLang Unified Radix Tree：eviction 与 lock_ref',
              link: '/infra/unified-radix-evict',
            },
            {
              text: 'SGLang Unified Cache：请求级提交路径',
              link: '/infra/unified-cache-order',
            },
            {
              text: 'SGLang Unified Cache：异构 Component 状态语义',
              link: '/infra/unified-cache-components',
            },
          ],
        },
      ],
      '/hardware/': [
        {
          text: '硬件',
          items: [
            {
              text: 'SSD 的能力边界与压榨路径',
              link: '/hardware/ssd',
            },
            {
              text: '一次 AIO 请求的全链路',
              link: '/hardware/aio-path',
            },
            {
              text: 'io_uring 相比 AIO 改了什么',
              link: '/hardware/io_uring',
            },
          ],
        },
      ],
      '/prefix-cache/': [
        {
          text: 'SGLang Prefix Cache 转换审计',
          items: [
            { text: '专栏总览与进度', link: '/prefix-cache/' },
            {
              text: '轮次 01 · BasePrefixCache 接口契约',
              link: '/prefix-cache/round-01-contract',
            },
            {
              text: '轮次 02 · Scheduler 侧调用点',
              link: '/prefix-cache/round-02-scheduler',
            },
            {
              text: '轮次 03 · 建树与 match / split / insert',
              link: '/prefix-cache/round-03-tree-core',
            },
          ],
        },
      ],
      '/cpp/': [
        {
          text: 'C++',
          items: [
            {
              text: '异常安全 与 数据安全',
              link: '/cpp/exception-safety',
            },
            {
              text: '定义、声明与实现',
              link: '/cpp/forward-declaration-vs-complete-definition',
            },
            {
              text: '原子操作内存序',
              link: '/cpp/atomic-memory-order',
            },
          ],
        },
      ],
    },
    socialLinks: [
      {
        icon: 'github',
        link: 'https://github.com/run-qing-spo/ai-infra-study',
      },
    ],
  },
  }),
)
