// SPDX-License-Identifier: Apache-2.0
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
        { text: '进程职责与命名', link: '/zh/process-roles' },
        { text: '玩家、场景与跨服模型', link: '/zh/player-scene-model' },
        { text: 'gameServer 能力评估', link: '/zh/gameserver-capabilities' },
        { text: '场景生命周期', link: '/zh/scene-lifecycle' },
        { text: '大地图评估与需求', link: '/zh/big-map' },
        { text: 'Avatar 运行时契约', link: '/zh/avatar-runtime' },
        { text: 'Avatar 全生命周期', link: '/zh/avatar-lifecycle' },
        { text: 'Avatar 创建/回收契约', link: '/zh/avatar-spawn-destroy' },
        { text: 'Avatar 重连与日切', link: '/zh/avatar-reconnect-cutover' },
        { text: '实体契约与代码生成', link: '/zh/entity-contract' },
        { text: '老架构与现代架构对比', link: '/zh/architecture-comparison' },
        { text: '迁移状态', link: '/zh/migration-status' },
        { text: 'HTTP、WebSocket 与外部回调', link: '/zh/http' },
        ] }, { text: '运行与扩展', items: [
        { text: 'Lua 与生命周期', link: '/zh/lua' },
        { text: '跨服与过载保护', link: '/zh/cross-server' },
        { text: 'Profiler 工作流', link: '/zh/profiling' },
        { text: '跨服回传与唯一写入权协议', link: '/zh/cross-return-protocol' },
        { text: 'login、data 与 apiServer 契约', link: '/zh/service-contracts' },
        { text: '协议面分析', link: '/zh/protocol-analysis' },
        { text: '协议契约恢复', link: '/zh/protocol-contract-recovery' },
        { text: '协议清单', link: '/zh/protocol-inventory' },
        { text: '源码分层分析', link: '/zh/source-layer-analysis' },
        { text: '术语表', link: '/zh/glossary' },
        { text: '类 BigWorld 协议体系演化', link: '/zh/bigworld-like-protocol' },
        { text: '业务场景 bot 与内置仪表盘', link: '/zh/business-bot' },
        { text: '依赖管理', link: '/zh/dependencies' },
        { text: '验证记录', link: '/zh/validation' },
        { text: '部署工作流', link: '/zh/deployment' }
      ] }, { text: '分析', items: [
        { text: '实体库表映射与设计取向', link: '/zh/entity-db-mapping-and-philosophy' },
        { text: '实体同步与持久化', link: '/zh/entity-sync-persistence' },
        { text: 'MMO 引擎调研', link: '/zh/mmo-engine-survey' },
        { text: 'MMO 引擎设计要点', link: '/zh/mmo-engine-design-highlights' },
        { text: 'WebSocket bad weak_ptr 复盘', link: '/zh/websocket-bad-weak-ptr' }
      ] }],
      '/': [{ text: 'Mistlane', items: [
        { text: 'Introduction', link: '/' },
        { text: 'Getting started', link: '/guide' },
        { text: 'Architecture and migration', link: '/architecture' },
        { text: 'Process responsibilities', link: '/process-roles' },
        { text: 'Players, scenes and transfers', link: '/player-scene-model' },
        { text: 'gameServer capability assessment', link: '/gameserver-capabilities' },
        { text: 'Scene lifecycle', link: '/scene-lifecycle' },
        { text: 'Big maps: assessment and requirements', link: '/big-map' },
        { text: 'Avatar runtime contract', link: '/avatar-runtime' },
        { text: 'Avatar lifecycle', link: '/avatar-lifecycle' },
        { text: 'Avatar spawn/destroy contract', link: '/avatar-spawn-destroy' },
        { text: 'Avatar reconnect and daily cutover', link: '/avatar-reconnect-cutover' },
        { text: 'Entity contracts and generation', link: '/entity-contract' },
        { text: 'Legacy and modern comparison', link: '/architecture-comparison' },
        { text: 'Migration status', link: '/migration-status' },
        { text: 'HTTP, WebSocket and callbacks', link: '/http' },
        ] }, { text: 'Runtime and extensions', items: [
        { text: 'Lua lifecycle', link: '/lua' },
        { text: 'Cross-server and overload', link: '/cross-server' },
        { text: 'Profiler workflow', link: '/profiling' },
        { text: 'Cross return and single-writer protocol', link: '/cross-return-protocol' },
        { text: 'login, data and apiServer contracts', link: '/service-contracts' },
        { text: 'Protocol analysis', link: '/protocol-analysis' },
        { text: 'Protocol contract recovery', link: '/protocol-contract-recovery' },
        { text: 'Protocol inventory', link: '/protocol-inventory' },
        { text: 'Source layer analysis', link: '/source-layer-analysis' },
        { text: 'Glossary', link: '/glossary' },
        { text: 'Business scenario bot and dashboard', link: '/business-bot' },
        { text: 'BigWorld-like protocol evolution', link: '/bigworld-like-protocol' },
        { text: 'Dependencies', link: '/dependencies' },
        { text: 'Validation', link: '/validation' },
        { text: 'Deployment workflow', link: '/deployment' }
      ] }, { text: 'Analysis', items: [
        { text: 'Entity DB mapping and philosophy', link: '/entity-db-mapping-and-philosophy' },
        { text: 'Entity sync and persistence', link: '/entity-sync-persistence' },
        { text: 'MMO engine survey', link: '/mmo-engine-survey' },
        { text: 'MMO engine design highlights', link: '/mmo-engine-design-highlights' },
        { text: 'WebSocket bad weak_ptr postmortem', link: '/websocket-bad-weak-ptr' }
      ] }]
    }
  }
})
