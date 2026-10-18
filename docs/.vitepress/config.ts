import { defineConfig } from 'vitepress'

const base = process.env.DOCS_BASE || '/mistlane/'

export default defineConfig({
  title: 'Mistlane',
  description: 'A framework extracted from a legacy game backend, for research and learning.',
  base,
  head: [['link', { rel: 'icon', type: 'image/svg+xml', href: `${base}logo.svg` }]],
  markdown: {
    config(md) {
      const fence = md.renderer.rules.fence!
      md.renderer.rules.fence = (tokens, index, options, env, self) => {
        if (tokens[index].info.trim() === 'mermaid') {
          const source = md.utils.escapeHtml(JSON.stringify(tokens[index].content))
          return `<MermaidDiagram :source="${source}" />`
        }
        return fence(tokens, index, options, env, self)
      }
    }
  },
  cleanUrls: false,
  lastUpdated: true,
  locales: {
    root: {
      label: 'English', lang: 'en-US', link: '/',
      themeConfig: {
        nav: [
          { text: 'Guide', link: '/guide' },
          { text: 'Architecture', link: '/architecture' },
          { text: 'Legacy and modern comparison', link: '/architecture-comparison' },
          { text: 'Migration status', link: '/migration-status' },
          { text: 'GitHub', link: 'https://github.com/cuihairu/mistlane' }
        ],
        outline: { label: 'On this page' },
        docFooter: { prev: 'Previous page', next: 'Next page' },
        lastUpdated: { text: 'Last updated' },
        footer: { message: 'For research and learning. Extracted from a legacy game backend.' }
      }
    },
    zh: {
      label: '简体中文', lang: 'zh-CN', link: '/zh/',
      description: '从老游戏服务端抽取并整理的框架，供研究与学习使用。',
      themeConfig: {
        nav: [
          { text: '使用指南', link: '/zh/guide' },
          { text: '架构说明', link: '/zh/architecture' },
          { text: '老架构与现代架构对比', link: '/zh/architecture-comparison' },
          { text: '迁移状态', link: '/zh/migration-status' },
          { text: 'GitHub', link: 'https://github.com/cuihairu/mistlane' }
        ],
        outline: { label: '本页目录' },
        docFooter: { prev: '上一页', next: '下一页' },
        lastUpdated: { text: '最后更新' },
        returnToTopLabel: '返回顶部',
        sidebarMenuLabel: '菜单',
        darkModeSwitchLabel: '外观',
        langMenuLabel: '语言',
        footer: { message: '从老游戏服务端抽取并整理，供研究与学习使用。' }
      }
    }
  },
  themeConfig: {
    logo: { src: '/logo.svg', alt: 'Mistlane' },
    search: { provider: 'local' },
    socialLinks: [{ icon: 'github', link: 'https://github.com/cuihairu/mistlane' }],
    sidebar: {
      '/zh/': [{ text: 'Mistlane', items: [
        { text: '项目介绍', link: '/zh/' },
        { text: '使用指南', link: '/zh/guide' },
        { text: '架构与迁移', link: '/zh/architecture' },
        { text: '老架构与现代架构对比', link: '/zh/architecture-comparison' },
        { text: '迁移状态', link: '/zh/migration-status' },
        { text: 'HTTP 与客户端', link: '/zh/http' },
        { text: 'Lua 与生命周期', link: '/zh/lua' },
        { text: '跨服与过载保护', link: '/zh/cross-server' },
        { text: '依赖管理', link: '/zh/dependencies' },
        { text: '验证记录', link: '/zh/validation' }
      ] }],
      '/': [{ text: 'Mistlane', items: [
        { text: 'Introduction', link: '/' },
        { text: 'Getting started', link: '/guide' },
        { text: 'Architecture and migration', link: '/architecture' },
        { text: 'Legacy and modern comparison', link: '/architecture-comparison' },
        { text: 'Migration status', link: '/migration-status' },
        { text: 'HTTP and client', link: '/http' },
        { text: 'Lua lifecycle', link: '/lua' },
        { text: 'Cross-server and overload', link: '/cross-server' },
        { text: 'Dependencies', link: '/dependencies' },
        { text: 'Validation', link: '/validation' }
      ] }]
    }
  }
})
