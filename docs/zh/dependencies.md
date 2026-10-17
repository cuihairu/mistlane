# 依赖管理

本项目只通过 vcpkg manifest 与 CMake imported targets 使用第三方库，不带历史 3rd，不在 CMake 配置期间执行 make，也不自行下载 / 构建 OpenSSL。

固定 baseline：`a5f736224e85b32e25a8ffc52562333180396642`，核对日期 2026-10-17。

| 依赖 | baseline 版本 | 使用方式 |
|---|---|---|
| boost-asio | 1.92.0 | `Boost::asio`，协程、TCP、计时、信号 |
| boost-beast | 1.92.0 | `Boost::beast`，WS/WSS |
| boost-mysql | 1.92.0 | `Boost::mysql`，成熟异步连接池 |
| lua | 5.5.1 | CMake FindLua 提供 include 与 libraries |
| openssl | 3.6.5#2 | `OpenSSL::SSL` / `OpenSSL::Crypto` |
| VitePress | 1.6.4 | npm 固定直接版本与 package-lock.json |

Boost.Charconv 等间接依赖由 Boost.MySQL 的 port 和 CMake target 自动传递，避免手工列库遗漏符号。manifest 显式请求 Asio 的 ssl feature；其他必要 feature 的最终解析由 ports 决定。

这里“最新”指核对日期 vcpkg 固定 baseline 中提供的版本。Boost、Lua 同时与上游最新稳定版一致。OpenSSL 上游最新稳定版为 4.0.3，当前 vcpkg port 是 3.6.5#2；采用仓库已经集成的 port，未把上游 4.0.3 声称为已接入。本项目未集成 beta / alpha 或移动 latest 标签。

旧项目使用的 log4cxx、Poco、Pion、jemalloc、自定义 MySQL/Redis/Lua 扩展、LPeg、sproto 和渠道 SDK 不进入此构建。此次升级只覆盖候选框架；原游戏根目录仍使用其旧构建。

升级流程：

1. 在 vcpkg 中选择新的不可变 commit，更新 manifest 的 builtin-baseline。
2. 核对版本清单与变更说明；需要新的最低版本时更新 `version>=` 和 CMake find_package。
3. 用干净构建目录配置，检查实际解析结果与间接依赖。
4. 执行网络 / Lua 集成验证和可用时的临时数据库验证，再更新验证记录。

使用现有 vcpkg 安装时，建议建立 baseline 对应的独立 checkout，避免其当前版本数据库早于 baseline。不要只修改 builtin-baseline 而保留无法解析新版本的旧 ports 工作树。CI 建立固定 commit 的独立 vcpkg checkout。

依据：[vcpkg baseline 源码](https://github.com/microsoft/vcpkg/tree/a5f736224e85b32e25a8ffc52562333180396642)、[Boost 稳定版](https://www.boost.org/releases/latest/)、[Lua 下载](https://www.lua.org/ftp/)、[OpenSSL 下载](https://www.openssl-library.org/source/)、[Boost.MySQL 连接池教程](https://www.boost.org/latest/libs/mysql/doc/html/mysql/tutorial_connection_pool.html)。

文档使用 npm 稳定版 VitePress 1.6.4，未使用 2.x alpha。升级时应同步更新固定版本与 package-lock.json，并执行 npm ci 和文档构建。
