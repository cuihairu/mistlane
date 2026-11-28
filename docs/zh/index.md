<script setup>
import { withBase } from 'vitepress'
</script>

<p align="center">
  <img :src="withBase('/logo.svg')" alt="Mistlane logo" width="64" height="64" style="margin-inline: auto;">
</p>

# Mistlane

Mistlane 是从**老游戏服务端抽取并整理**的 C++20 / Lua 框架，主要用于**研究与学习**：了解服务职责划分、异步网络、Lua 集成，以及历史代码的现代化改造。

当前版本是供学习参考的框架候选，不作为生产环境部署推荐。抽取范围排除了原游戏玩法、数值表、账号接入、支付、渠道 SDK、生产配置和原仓库历史。

当前实现已迁入协调面、角色（Avatar）运行时与场景生命周期、实体契约工具链；原游戏玩法、内容与空间 AOI/分块流式仍不在范围内。目标服务职责已经整理为 gateway、login、coordinator、gameServer、crossServer、data 和可选 apiServer，见[进程职责与命名](./process-roles.md)。

## 可以研究什么

| 领域 | 当前实现 |
|---|---|
| 异步 I/O | Boost.Asio 的 TCP、计时器、信号与协程 |
| WS 与 WSS | Drogon，配合 OpenSSL TLS |
| 数据库接入 | 直接使用 Boost.MySQL connection_pool 和参数绑定 |
| 应用脚本 | 单线程 Lua 模块生命周期和可回退的 VM 替换 |
| 依赖复现 | CMake Presets 和固定 baseline 的 vcpkg manifest |
| 实体契约 | `.def` 契约编译为确定性 sproto schema、C++/JS/C# 绑定与兼容性发布门 |
| Avatar 与场景 | 七态 Avatar 生命周期、宿主服务场景实例、跨宿主移交与结算 |

### 目标职责图

```mermaid
flowchart TB
  client["客户端"] <--> gate["gateway · 连接与消息转发"]
  gate <--> login["login · 认证与角色选择"]
  gate <--> game["gameServer · 本服角色与场景"]
  gate <--> cross["crossServer · 跨服角色与场景"]
  coord["coordinator · 分配、归属与切换协调"] -. "路由信息" .-> gate
  login -. "请求分配" .-> coord
  coord -. "承载分配" .-> game
  coord -. "承载分配" .-> cross
  game <--> data["data · 加载、缓存与持久化"]
  cross <--> data
```

数据库适配目前属于独立 C++ 接入，尚未成为 Lua 数据库桥接。默认按连接路由；gateway 可选以 `MLM1`（`--upstream-multiplex on`）让全部会话共享一条上游连接。coordinator 迁移事务与 `MLR1 MIG` 线协议已实现，crossServer 回传发送端（有界重试队列、home 最终序号权威）可在落库后回答 `ACK durable`。[Avatar 运行时](./avatar-runtime.md)已端到端接线：正常登录链（auth → 角色列表 → place/grant/ticket → 加入）、宿主服务场景实例之上的七态生命周期、跨宿主移交（冻结 → 围栏 → 提交 → 采纳）与结算（离开边最终账目等耐久 ACK 排空后才释放）。本版之外：发送方崩溃后的耐久重放、空间 AOI 与分块流式、以及原游戏玩法。

从[使用指南](./guide.md)开始，再阅读[架构与能力边界](./architecture.md)和[验证记录](./validation.md)。

## 来源与许可证

框架源于 FOGS，原项目名的具体含义尚无明确资料。Mistlane 使用 Apache License 2.0。源码来源和第三方依赖授权见 [LICENSE](https://github.com/cuihairu/mistlane/blob/main/LICENSE)、[NOTICE](https://github.com/cuihairu/mistlane/blob/main/NOTICE) 和[第三方说明](https://github.com/cuihairu/mistlane/blob/main/THIRD_PARTY.md)。
