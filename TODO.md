# Mistlane 发布前 TODO

> 用途：把当前开源候选整理为可逐项验收的工作清单。每个复选框只表示一个可独立完成、可独立核验的动作；完成后在同一项末尾补充证据（命令输出、提交号、链接或文件路径）。发现新问题时新增单独复选框，不把多个动作塞进一个勾选项。
>
> 状态快照：本轮候选通过审核过的发布文件清单同步到独立 Mistlane 仓库；原游戏源码、配置与 Git 历史不在同步范围。后续发布继续使用 `tools/export.py` 校验导出范围。

## A. 项目许可证与来源说明

- [x] 将仓库根许可证替换为 Apache License 2.0 全文。证据：`LICENSE` 开头为 Apache License Version 2.0。
- [x] 将 vcpkg manifest 项目许可证字段设为 `Apache-2.0`。证据：`vcpkg.json`。
- [x] 将 pnpm package manifest 的项目许可证字段设为 `Apache-2.0`。证据：`package.json`。
- [x] 将英文 README 的许可证徽章和说明改为 Apache-2.0。证据：`README.md`。
- [x] 将中文 README 的许可证徽章和说明改为 Apache-2.0。证据：`README.zh-CN.md`。
- [x] 将英文文档首页许可证说明改为 Apache-2.0。证据：`docs/index.md`。
- [x] 将中文文档首页许可证说明改为 Apache-2.0。证据：`docs/zh/index.md`。
- [x] 更新 NOTICE，区分项目许可证、原游戏来源署名和改编的 vcpkg port 授权。证据：`NOTICE`。
- [x] 更新第三方清单，保留 Drogon overlay 源自 vcpkg 的 MIT 许可说明。证据：`THIRD_PARTY.md`、`ports/LICENSE.txt`。
- [x] 从发布文件清单移除独立重复的 Apache 许可证副本路径。证据：`release-files.txt`。
- [ ] 检查每个自有源文件的 SPDX 头部；为缺少头部的文件添加 `SPDX-License-Identifier: Apache-2.0`。
- [ ] 审核许可证变更的贡献者权利来源，确认仓库当前贡献内容均有权重新按 Apache-2.0 发布；记录审查结论。
- [ ] 用许可证扫描器检查源代码包的许可证识别结果，并保存报告。
- [ ] 检查生成的源代码包同时包含 `LICENSE`、`NOTICE`、`THIRD_PARTY.md` 和 `ports/LICENSE.txt`。
- [ ] 检查所有 README、文档页、About 文案和徽章没有把 Mistlane 项目本身描述成 MIT 或双许可证。

## B. README 与技术描述校正

- [ ] 对照 `src/web.cpp` 核验 README 对 HTTP/HTTPS/WS/WSS 使用 Drogon/Trantor 的描述。
- [ ] 对照 `src/node.cpp` 核验 README 对 TCP、gateway、router 和 worker 的描述。
- [ ] 对照 Lua 实现及 `examples/http.lua` 核验 README 中 HTTP handler 和钩子列表。
- [ ] 对照 `include/mistlane/mysql_pool.hpp` 核验 README 对 Boost.MySQL connection_pool 的描述。
- [ ] 对照 vcpkg overlay 实际版本核验 README 中 Drogon 和依赖版本信息。
- [ ] 对照 `CMakePresets.json` 核验 README 中平台、构建目录和命令。
- [ ] 检查中文 README 的所有相对链接都能解析到仓库内文件。
- [ ] 检查英文 README 的所有相对链接都能解析到仓库内文件。
- [ ] 检查 README 两种语言都将项目定位为从老游戏后端抽取、用于研究学习的框架。
- [ ] 检查 README 两种语言都明确说明移除了原游戏业务逻辑，不兼容原协议。

## C. 双语文档一致性

- [ ] 更新英文入门指南中的脚本接口，使其覆盖当前 Lua 消息和 HTTP worker 两种模式。
- [ ] 更新中文入门指南中的脚本接口，使其覆盖当前 Lua 消息和 HTTP worker 两种模式。
- [ ] 对照 `src/lua_service.cpp` 核验英文 Lua 文档中的 init/start/tick/stop/shutdown 顺序。
- [ ] 对照 `src/lua_service.cpp` 核验中文 Lua 文档中的 init/start/tick/stop/shutdown 顺序。
- [ ] 对照 HTTP worker 实现核验英文文档的 before_request/http/after_request/on_error 生命周期。
- [ ] 对照 HTTP worker 实现核验中文文档的 before_request/http/after_request/on_error 生命周期。
- [ ] 更新英文架构文档中任何过时的 Beast、Poco 或自研连接池表述。
- [ ] 更新中文架构文档中任何过时的 Beast、Poco 或自研连接池表述。
- [ ] 对照跨服代码核验英文跨服文档中的 group/room 路由、epoch 和准入失败码。
- [ ] 对照跨服代码核验中文跨服文档中的 group/room 路由、epoch 和准入失败码。
- [ ] 确认英文与中文依赖说明对 vcpkg baseline、Drogon overlay 和 Trantor 的说明一致。
- [ ] 确认英文与中文 HTTP 文档对 libcurl 的 TLS 校验、协议限制和响应大小限制说明一致。
- [ ] 更新英文验证记录，准确列出本轮 Drogon HTTP/HTTPS/WS/WSS、Lua HTTP 和跨服检查。
- [ ] 更新中文验证记录，准确列出本轮 Drogon HTTP/HTTPS/WS/WSS、Lua HTTP 和跨服检查。
- [ ] 检查 VitePress 每个英文页面都有对应中文页面，或在目录中注明仅有单语的原因。
- [ ] 检查所有 Mermaid 图在 README 和文档中的节点名称与当前进程/线程职责一致。
- [ ] 核验文档站点默认 base 和目标 GitHub Pages URL 一致。

## D. 架构与运行边界

- [ ] 记录 TCP 会话的长度头、空/二进制 payload 支持和最大消息字节数。
- [ ] 记录 WS/WSS 消息与内部 TCP 帧的差异及请求串行约束。
- [ ] 记录 Drogon/Trantor 线程与 Asio/Lua 线程之间的有界桥接规则。
- [ ] 记录 HTTP body、待处理请求数和连接数的各自限制及其实现位置。
- [ ] 将 Trantor 输出 backlog 阈值明确标为回调高水位，而非硬内存配额。
- [ ] 将跨服 room/group/shard 连接数配额明确标为本节点准入，不描述为全局一致性配额。
- [ ] 明确记录当前路由不提供服务发现、健康路由、自动重连或状态迁移。
- [ ] 明确记录连接配额统计连接数，不等同于已认证玩家数。
- [ ] 明确记录认证票据、mTLS、重复登录策略由应用集成，框架未实现。
- [ ] 明确记录 session 内存预算为估算值，不等同于 RSS 硬上限。
- [ ] 记录历史游戏“跨服人数过多崩溃”原因尚未由本仓库数据确认，不能声称已复现或根治。
- [ ] 记录生产容量需通过目标硬件上的负载测试测量，不以示例默认值承诺玩家容量。

## E. C++、依赖与构建复核

- [ ] 从干净构建目录执行 Linux Debug 配置并记录完整命令。
- [ ] 从干净构建目录编译 `mistlane` 及可选 probe targets 并记录结果。
- [ ] 检查最终主程序文件名为 `mistlane`。
- [ ] 使用项目 vcpkg baseline 和 overlay 执行依赖解析并记录 Drogon 实际版本。
- [ ] 检查 CMake 在 BUILD_TESTING=OFF 时不要求 Python 解释器。
- [ ] 检查 CMake 在 BUILD_TESTING=ON 时明确报告 Python 测试依赖。
- [ ] 检查默认 vcpkg triplet 及 OpenSSL/Curl TLS 配置与文档一致。
- [ ] 检查 curl 客户端拒绝非 HTTP/HTTPS 协议的实现路径。
- [ ] 检查 curl 客户端的 CA、主机名验证和响应体上限实现路径。
- [ ] 检查 Boost.MySQL 池使用成熟官方 connection_pool，而非项目自建池。
- [ ] 检查导出包没有包含构建目录、node_modules、凭据或本地证书。
- [ ] 检查 pnpm lockfile 与 package.json 的包管理器版本字段一致。
- [ ] 检查 Node.js 最低版本声明满足锁定的 Mermaid/VitePress 依赖要求。

## F. 自动化验证与文档站

- [ ] 在同步候选后运行 Debug CTest，并记录通过数量与日志位置。
- [ ] 确认集成测试覆盖 TCP gateway/router/worker 的基本往返。
- [ ] 确认集成测试覆盖 Drogon HTTP 与 HTTPS。
- [ ] 确认集成测试覆盖 Drogon WS 与 WSS。
- [ ] 确认集成测试覆盖 Lua HTTP handler 和请求生命周期钩子。
- [ ] 确认集成测试覆盖跨服握手、room 路由和准入拒绝。
- [ ] 确认集成测试覆盖 Lua 脚本重载失败时旧 VM 继续服务。
- [ ] 确认 MariaDB 集成测试使用临时数据目录、临时端口和清理逻辑。
- [ ] 使用 `pnpm install --frozen-lockfile` 检查文档依赖锁定状态。
- [ ] 使用 `pnpm docs:build` 构建英文与中文文档。
- [ ] 在浏览器打开英文架构图页面并确认 Mermaid SVG 成功渲染。
- [ ] 在浏览器打开中文跨服图页面并确认 Mermaid SVG 成功渲染。
- [ ] 在浏览器切换浅色/深色主题并确认图中文字对比度可读。
- [ ] 在移动视口检查主要架构图和生命周期图没有遮挡页面内容。
- [ ] 检查浏览器控制台没有 Mermaid 加载或渲染异常。
- [ ] 对文档依赖执行安全审计并保存零告警或已接受告警报告。
- [ ] 评估 VitePress 构建的 Mermaid 大 chunk 警告；若采取拆分，单独记录改动与构建结果。

## G. 导出包与独立仓库同步

- [x] 将本 TODO 加入导出 allowlist。证据：`release-files.txt`。
- [x] 执行 `python3 tools/export.py` 生成带 SHA256 清单的源码包。证据：`dist/mistlane-0.1.0.tar.gz`，68 个文件。
- [ ] 检查源码包文件列表严格符合 `release-files.txt`。
- [x] 检查源码包中的 `LICENSE` 是 Apache-2.0 全文。证据：从归档读取首行及版本行。
- [ ] 检查源码包中的 README、双语文档、logo、源码、示例和测试完整。
- [ ] 在全新临时目录解压源码包并确认其根目录可直接配置 CMake。
- [ ] 对独立仓库 `cuihairu/mistlane` 检查当前分支、工作区和远端状态。
- [ ] 将已审核的导出源码包同步到独立仓库工作树，不混入老游戏文件。
- [ ] 在独立仓库确认 Apache-2.0 是仓库识别到的唯一项目许可证。
- [ ] 在独立仓库检查源码变更差异并确认无意外删除或生成物。
- [ ] 提交独立仓库源码同步，并记录提交 SHA。
- [ ] 推送已授权的独立仓库分支，并记录远端提交 SHA。
- [ ] 检查 GitHub Actions 对推送提交的构建和文档任务结果。
- [ ] 检查 GitHub 仓库许可证页面识别为 Apache License 2.0。
- [ ] 检查 GitHub About、topics、主页链接仍与实际发布状态相符。

## H. 发布后整理

- [ ] 为首个公开源码候选确定版本号与发布说明。
- [ ] 发布说明写明研究学习用途、老游戏抽取背景和业务剥离范围。
- [ ] 发布说明列出当前不支持的生产能力和未验证容量边界。
- [ ] 为源码归档附上 SHA256 校验值。
- [ ] 检查 GitHub 发布页包含 Apache-2.0 许可证链接和 NOTICE。
- [ ] 在完成实际部署前，不将 GitHub About 首页设置为尚未发布的文档站地址。

## I. 老框架组件迁移追踪

- [x] 建立双语能力迁移状态清单，区分重写、替换、部分替换、未迁移和排除项。证据：`docs/migration-status.md`、`docs/zh/migration-status.md`。
- [x] 检查发布候选内没有以旧产品前缀命名的文件。证据：`rg --files mistlane` 前缀扫描无匹配。
- [ ] 从旧 C++ 生命周期实现中逐项核对 Node 启动、信号处理、退出顺序和异常路径，并更新状态清单。
- [ ] 从旧 Lua 回调实现中逐项核对 init/start/tick/stop/shutdown 映射，并更新状态清单。
- [ ] 追踪旧服务连接注册报文及其调用方，决定是否设计新的服务认证/注册接口。
- [ ] 追踪旧服务连接重连和发送队列的生产者、消费者与排队字节数，决定是否需要有界可靠投递模块。
- [ ] 对旧线程池的实际工作负载分类，逐项标记为 Asio I/O、Boost.MySQL 异步 I/O、阻塞任务或已废弃。
- [ ] 若存在阻塞任务需求，先评估 `boost::asio::thread_pool`；只为其补充任务准入上限、拒绝策略、取消和关闭适配，不另造线程调度器。
- [ ] 比较旧 Lua Timer/TimerQueue 的接口与候选 tick 钩子，确定是否需要 cancellable timeout/interval API。
- [ ] 若增加 Lua 定时器 API，为单次、周期、取消、回调异常和重载行为分别定义契约。
- [ ] 对旧 UDP API 的实际调用点做只读盘点；若仍需要，使用 Boost.Asio UDP socket 实现最薄的协议适配。
- [x] 对旧 Redis C/Lua API 的调用点做只读盘点，并基于 Boost.Redis 的 RESP3、断线重连和 Sentinel 能力确定集成边界。证据：Redis 适配集中在 Asio worker，Drogon Redis 未启用。
- [x] 检查当前 vcpkg baseline 是否提供所需 Boost.Redis port/feature，并记录解析出的精确版本。证据：`boost-redis` 1.92.0 已配置并由 vcpkg 成功安装。
- [x] 比较 Boost.Redis 与 Drogon Redis 在 worker 线程归属、连接生命周期和配置方式上的差异，选定一个默认实现，避免重复集成。决策：选 Boost.Redis，与现有 Asio/Lua 线程归属一致。
- [x] 为可选 Redis runtime 添加共享连接生命周期、异步回调失效保护、ACL 环境变量读取和 256 个未完成请求上限。证据：`src/lua_service.cpp`、`include/mistlane/lua_service.hpp`。
- [x] 为 Lua Redis Streams 添加 create_group、publish、read、ack、claim API，并对 stream 命名、消息字段、批次和待处理消息恢复行为写明契约。证据：`docs/lua.md`、`docs/zh/lua.md`。
- [x] 为 Lua 排行榜添加原子增分/名次、查询、分页榜单和删除 API，并定义一基名次、并列顺序、精度和分页上限。证据：`docs/lua.md`、`docs/zh/lua.md`。
- [x] 为 Streams 与排行榜 API 增加可重复执行的本地 Redis 集成验证：发布/读取字段、claim 接管 pending、ACK、排行榜增分/查询/分页/删除。证据：`tests/redis_integration.py`，通过 `MISTLANE_TEST_LOCAL_REDIS=ON` 启用。
- [ ] 扩展 Redis 集成验证，覆盖消费组重复创建、断线重连和热重载期间旧 Lua 回调失效。
- [ ] 增加 Redis TLS 与 CA/主机名校验配置；在实现前仅使用可信内网 Redis，并确保文档保持此限制说明。
- [ ] 评估 Stream 消息幂等键、保留/裁剪策略、失败重试与死信队列配置；确认默认值前不自动裁剪或 ACK。
- [ ] 为日志功能确定最小需求：结构化字段、级别、并发写入、轮转和采集边界；优先评估 Boost.Log 作为跨角色统一设施。
- [ ] 对旧监控/系统信息 API 的调用方做只读盘点，决定是否需要 Prometheus/OpenTelemetry 适配。
- [ ] 逐个筛查候选 Lua 通用工具的全局依赖、游戏术语、外部库和来源授权，再决定提取或排除。
- [ ] 将每个决定迁移的通用模块拆成单独实现任务，写明接口、许可证来源和独立验收条件。
- [ ] 保持游戏玩法、账号、支付、地图导航、线上配置和 SDK 不进入发布包。
- [ ] 保持公开文件名采用 Mistlane 命名，不使用旧产品前缀；同时保留准确的 NOTICE 和第三方来源说明。

## J. 崩溃报告、Prometheus、内部探测与性能剖析

- [x] 确认旧 Lua `C_monitorDeadLoop` 的职责与边界：超时后设置 Lua 指令 hook 尝试中断脚本；它不是 CPU/内存 profiler，也不是服务间健康探测。证据：旧实现 `d_common/common/src/DeadLoopMonitor.cpp` 与 `LuaContext.hpp`。
- [x] 明确旧框架与当前候选均没有 CPU 采样、内存分配或 Lua 函数级 profiler。证据：源码检索与迁移状态文档。
- [x] 将 Crashpad 和 `prometheus-cpp` pull 加入 vcpkg manifest。证据：`vcpkg.json`、Debug 构建和诊断启动测试。
- [x] 用固定 baseline 在 Linux Debug triplet 完成 Crashpad 与 prometheus-cpp 的 vcpkg 安装；handler 位于 triplet tools/crashpad_handler。
- [x] 在 CMake 中链接 `crashpad::crashpad` 与 Prometheus core/pull targets；当前均为核心依赖。
- [x] 为启动参数添加可选 Crashpad handler 路径和转储数据库目录；必须同时提供，上传默认关闭。
- [x] 在创建网络与 Lua 服务前初始化 Crashpad；annotation 仅含 role 和产品名。
- [x] Crashpad 不传入上传 URL，并显式关闭数据库上传设置；目录权限、磁盘配额、轮转/清理策略和运维提取方式仍待定义。
- [ ] 为 Linux Crashpad handler 增加部署复制/安装规则，并检查静态链接、运行库和容器 ptrace/seccomp 限制。
- [ ] 增加独立崩溃样例程序，在隔离临时目录触发可控 native crash，断言 Crashpad 生成可识别 minidump 后清理文件。
- [ ] 为 release 与调试构建归档匹配的未剥离符号/Build ID，记录用 minidump_stackwalk 符号化的复现步骤。
- [x] 实现 Prometheus Registry 与 role 低基数标签，不使用玩家、房间、会话 ID 作为 label。
- [x] 添加可配置 metrics 监听地址和端口，默认禁用；单独监听，不复用玩家协议端口。
- [x] 添加 `mistlane_ready`、活动会话 gauge、准入拒绝 counter，并以 role 作为标签；build/version/node identity 指标仍待补。
- [ ] 为 TCP/cross/shard 添加收发消息数/字节、请求耗时直方图、上游错误和超时计数，并限制直方图桶数与 label 集合。
- [x] 为 Lua message/http handler 记录耗时；tick 耗时和错误分类仍待补。
- [ ] 记录 Asio 100ms tick 的调度延迟直方图/最大值，让事件循环阻塞能被观测；测量本身不得阻塞 Lua loop。
- [x] 每个约 100ms tick 从 Linux procfs 采样 RSS；CPU 指标和采样语义文档仍待补。
- [x] 为 Prometheus endpoint 添加集成检查：路径、文本 exposition、ready 指标和活动连接数。
- [ ] 设计独立内部探测协议：独立 control listener、请求 nonce、超时、协议版本和最小身份校验；不与玩家会话共用连接或 Lua handler。
- [ ] 分开实现 liveness 与 readiness：liveness 判断进程/事件循环是否响应；readiness 判断能否接纳新会话，并返回 active/capacity、event-loop lag 等有界摘要。
- [ ] 为 shard 配置过载阈值与迟滞窗口；连续失败才降为 not-ready，连续恢复才重新 ready，避免探测抖动。
- [ ] 让 cross 在新房间握手时读取 shard readiness 并拒绝不可接纳节点；保留已有会话和房间 owner，禁止探测失败触发状态房间自动重映射。
- [ ] 增加探测功能测试：响应 nonce、错误版本、超时、事件循环阻塞、readiness 转换迟滞、关闭清理及多个 cross 观测同一 shard。
- [ ] 对 profiler 做 vcpkg 可用性与采样开销评估，选择可选外部采样器/工具集成路线；不在服务热路径自行实现采样器。
- [ ] 为 profiler 工作流提供可重复的跨服压测场景和符号化步骤，记录采样对吞吐、P99 和 RSS 的影响。
- [ ] 同步更新双语 README、依赖页、架构页、迁移状态、跨服排障页和验证记录，标清哪些能力已实现、哪些只是依赖声明。
- [ ] 在目标 Linux/容器环境执行崩溃演练、监控抓取、探针过载演练和压力剖析；未测试前不宣称已解决历史跨服卡死或容量问题。

## K. 项目初始化 CLI 与进程运维模板

- [x] 盘点旧部署目录、配置层级、工作目录假设、二进制/Lua/log/pid 路径和服务启动参数。证据：对照部署 shell、服务 main 参数及 per-service Lua 配置。
- [x] 记录旧项目按多个专用二进制、共享 Lua 根目录、共享/独立 Lua 配置、日志配置和实例 index 拆分的实际做法，并指出配置由部署脚本生成、运行依赖当前目录。
- [x] 比较持久世界 MMO 和 session/match 型 server 架构，记录旧协调热点、静态配置及当前 Mistlane 与现代能力的差距。证据：双语 `docs/architecture-comparison.md`。
- [x] 规定初始化模板按项目根目录解析 Lua/config/log/pid 位置，每个进程实例单独保存 role/listen/upstream/script/epoch 参数。
- [x] 增加 `mistlane init [DIRECTORY]`，在新目录生成 TCP 主集群、cross/two-shard 和 HTTP Lua 示例；拒绝覆盖非空目录。
- [x] 增加逐实例配置文件及通用前台实例启动脚本；可用 `MISTLANE_BIN` 指定二进制路径，Lua 入口相对项目根目录解析。
- [x] 增加本地 `start/stop/restart/status/logs` 运维脚本，PID 与日志写入模板项目 `var/`。
- [x] 增加 systemd 模板服务，支持按 instance name 读取对应配置；文档说明安装路径、服务账户与 unit 安装方式。
- [x] 为模板 CLI 添加集成检查：完整文件集、shell 语法、可执行位、相对脚本路径运行、TCP echo、启停/status 与不覆盖既有文件。证据：`tests/scaffold.py`。
- [x] 将新 CLI 源码、测试、双语架构对比页加入发布 allowlist 并更新文档导航。证据：`release-files.txt` 与 VitePress sidebar。
- [ ] 用 `systemd-analyze verify` 检查替换安装路径后的 unit 文件，并用临时系统用户做一次 systemd 启停演练。
- [ ] 为逐进程 shell 配置文档补充合法字段、逗号分隔 upstream、密钥应使用 systemd credentials/environment，而非提交到模板仓库。
- [ ] 在模板上增加进程间依赖/健康检查与启动失败回滚，避免用固定 sleep 代替 readiness；当前启动器只做短暂启动确认。
- [ ] 决定是否提供原生 Mistlane config-file schema；若实现，定义格式版本、字段校验、环境覆盖、相对路径规则和错误提示，再将 shell env 模板迁移到该 schema。
- [ ] 按架构比较文档继续补内部 liveness/readiness 探测、Crashpad 转储生成/保留与部署工作流；再依据真实压力测试决定动态 registry、lease/fencing、房间 drain/migration 或容器编排需求。
