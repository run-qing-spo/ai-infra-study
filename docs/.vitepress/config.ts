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
    nav: [
      { text: '首页', link: '/' },
      { text: '基础设施', link: '/infra/inference-fundamentals' },
      { text: 'AI 存储', link: '/projects/overview' },
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
          ],
        },
      ],
      '/projects/': [
        {
          text: 'AI 存储',
          items: [
            {
              text: '项目总览',
              link: '/projects/overview',
            },
            {
              text: '项目 1 · Thread-safe LRU Cache',
              link: '/projects/project1-lru',
            },
          ],
        },
      ],
      '/canvas/': [
        {
          text: 'Canvas',
          items: [
            { text: 'Basic', link: '/canvas/basic' },
            { text: 'Animation', link: '/canvas/animation' },
          ],
        },
      ],
      '/ai/': [
        {
          text: 'AI',
          items: [],
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
