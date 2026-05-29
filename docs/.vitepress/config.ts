import { defineConfig } from 'vitepress'

export default defineConfig({
  title: 'AI Infra Study',
  description: 'AI 推理与基础设施学习笔记',
  base: '/ai-infra-study/',
  lang: 'zh-CN',
  themeConfig: {
    nav: [
      { text: '首页', link: '/' },
      { text: '基础设施', link: '/infra/inference-fundamentals' },
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
})
