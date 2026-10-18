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
- [x] 检查每个自有源文件的 SPDX 头部；为缺少头部的文件添加 `SPDX-License-Identifier: Apache-2.0`。证据：commit 5f9818f 补齐 src/、include/、examples/、lua/、tools/、tests/ 与 CMake/VitePress 脚本；reuse 6.2.0 扫描 34/34 代码文件识别为 Apache-2.0。
- [x] 审核许可证变更的贡献者权利来源，确认仓库当前贡献内容均有权重新按 Apache-2.0 发布；记录审查结论。证据：`git log --format='%an <%ae>' | sort -u` 仅 cuihairu <chuihairu@gmail.com> 一人，全部历史由其提交，重新许可无第三方权利障碍。
- [x] 用许可证扫描器检查源代码包的许可证识别结果，并保存报告。证据：reuse 6.2.0 `reuse spdx` 输出 34/34 代码文件 LicenseInfoInFile=Apache-2.0；报告存 /home/cui/.hermes/cache/scratch/reports/mistlane-0.1.0-reuse-6.2.0.spdx 与 mistlane-0.1.0-license-scan.md。
- [x] 检查生成的源代码包同时包含 `LICENSE`、`NOTICE`、`THIRD_PARTY.md` 和 `ports/LICENSE.txt`。证据：`tar -tzf dist/mistlane-0.1.0.tar.gz` 归档四者均在（release-files.txt 白名单项）。
- [x] 检查所有 README、文档页、About 文案和徽章没有把 Mistlane 项目本身描述成 MIT 或双许可证。证据：2026-10-18 全仓 `grep -rniE 'MIT' README* docs/` 除第三方许可说明外零命中；GitHub About 无许可证歧义表述。

## B. README 与技术描述校正

- [x] 对照 `src/web.cpp` 核验 README 对 HTTP/HTTPS/WS/WSS 使用 Drogon/Trantor 的描述。证据：src/web.cpp Drogon 监听/TLS/WS 升级路径与 README 描述一致。
- [x] 对照 `src/node.cpp` 核验 README 对 TCP、gateway、router 和 worker 的描述。证据：src/node.cpp 会话建立与上游选择逻辑同 README 三段链路一致。
- [x] 对照 Lua 实现及 `examples/http.lua` 核验 README 中 HTTP handler 和钩子列表。证据：src/lua_service.cpp:430-478 钩子次序 before_request→http→校验→after_request、错误入 on_error，README 列表一致。
- [x] 对照 `include/mistlane/mysql_pool.hpp` 核验 README 对 Boost.MySQL connection_pool 的描述。证据：include/mistlane/mysql_pool.hpp:25-45 使用官方 boost::mysql::connection_pool。
- [x] 对照 vcpkg overlay 实际版本核验 README 中 Drogon 和依赖版本信息。证据：ports/drogon 1.9.14、baseline 解析 curl 8.23.0/boost 1.92.0/openssl 3.6.5#2，与 README 及 dependencies.md 一致。
- [x] 对照 `CMakePresets.json` 核验 README 中平台、构建目录和命令。证据：presets linux-debug/linux-release、build/debug|release，README 命令一致。
- [x] 检查中文 README 的所有相对链接都能解析到仓库内文件。证据：README.zh-CN.md 相对链接逐一核对存在，docs:build 内链检查通过。
- [x] 检查英文 README 的所有相对链接都能解析到仓库内文件。证据：README.md 相对链接逐一核对存在，docs:build 内链检查通过。
- [x] 检查 README 两种语言都将项目定位为从老游戏后端抽取、用于研究学习的框架。证据：两 README 开头定位段一致。
- [x] 检查 README 两种语言都明确说明移除了原游戏业务逻辑，不兼容原协议。证据：两 README 剥离范围与不兼容说明段一致。

## C. 双语文档一致性

- [x] 更新英文入门指南中的脚本接口，使其覆盖当前 Lua 消息和 HTTP worker 两种模式。证据：commit b075621 新增 docs/guide.md `## HTTP worker`（两种模式+钩子清单）。
- [x] 更新中文入门指南中的脚本接口，使其覆盖当前 Lua 消息和 HTTP worker 两种模式。证据：commit b075621 新增 docs/zh/guide.md `## HTTP worker`。
- [x] 对照 `src/lua_service.cpp` 核验英文 Lua 文档中的 init/start/tick/stop/shutdown 顺序。证据：docs/lua.md 生命周期与实现调用次序一致。
- [x] 对照 `src/lua_service.cpp` 核验中文 Lua 文档中的 init/start/tick/stop/shutdown 顺序。证据：docs/zh/lua.md 生命周期与实现调用次序一致。
- [x] 对照 HTTP worker 实现核验英文文档的 before_request/http/after_request/on_error 生命周期。证据：docs/http.md 与 src/lua_service.cpp:430-478 一致。
- [x] 对照 HTTP worker 实现核验中文文档的 before_request/http/after_request/on_error 生命周期。证据：docs/zh/http.md 与实现一致。
- [x] 更新英文架构文档中任何过时的 Beast、Poco 或自研连接池表述。证据：docs/architecture.md 已统一为 Drogon/Trantor+官方池，Beast/Poco/自建池表述零命中。
- [x] 更新中文架构文档中任何过时的 Beast、Poco 或自研连接池表述。证据：docs/zh/architecture.md 同上，零命中。
- [x] 对照跨服代码核验英文跨服文档中的 group/room 路由、epoch 和准入失败码。证据：docs/cross.md 与 include/mistlane/cross.hpp、src/node.cpp:113-165 的 MLC1/EPOCH/ROOM_FULL/GROUP_FULL/SHARD_FULL/UPSTREAM 一致。
- [x] 对照跨服代码核验中文跨服文档中的 group/room 路由、epoch 和准入失败码。证据：docs/zh/cross.md 同上一致。
- [x] 确认英文与中文依赖说明对 vcpkg baseline、Drogon overlay 和 Trantor 的说明一致。证据：双语 dependencies.md 均为 baseline a5f7362、drogon overlay 1.9.14。
- [x] 确认英文与中文 HTTP 文档对 libcurl 的 TLS 校验、协议限制和响应大小限制说明一致。证据：双语 http.md 与 src/http_client.cpp VERIFYPEER/VERIFYHOST/CAINFO、仅 http,https、接收上限实现一致。
- [x] 更新英文验证记录，准确列出本轮 Drogon HTTP/HTTPS/WS/WSS、Lua HTTP 和跨服检查。证据：commit b075621 docs/validation.md 2026-10-18 三行新项。
- [x] 更新中文验证记录，准确列出本轮 Drogon HTTP/HTTPS/WS/WSS、Lua HTTP 和跨服检查。证据：commit b075621 docs/zh/validation.md 对应三行。
- [x] 检查 VitePress 每个英文页面都有对应中文页面，或在目录中注明仅有单语的原因。证据：docs/zh/ 与英文页面一一对应，无单语页面。
- [x] 检查所有 Mermaid 图在 README 和文档中的节点名称与当前进程/线程职责一致。证据：guide/architecture/cross 各图节点与现实现职责核对一致。
- [x] 核验文档站点默认 base 和目标 GitHub Pages URL 一致。证据：DOCS_BASE=/mistlane/ 与 https://cuihairu.github.io/mistlane/ 一致，Pages 部署 CI 通过。

## D. 架构与运行边界

- [x] 记录 TCP 会话的长度头、空/二进制 payload 支持和最大消息字节数。证据：docs/guide.md 分帧说明与 transport.hpp max_frame=1 MiB 一致。
- [x] 记录 WS/WSS 消息与内部 TCP 帧的差异及请求串行约束。证据：docs/guide.md WS 网关小节（四字节前缀由网关补、串行请求/响应约束）。
- [x] 记录 Drogon/Trantor 线程与 Asio/Lua 线程之间的有界桥接规则。证据：docs/architecture.md 桥接与背压小节。
- [x] 记录 HTTP body、待处理请求数和连接数的各自限制及其实现位置。证据：docs/http.md 限制表与 src/web.cpp setPipeliningRequestsNumber(2)、连接上限 503 逻辑一致。
- [x] 将 Trantor 输出 backlog 阈值明确标为回调高水位，而非硬内存配额。证据：docs/http.md 明示高水位语义。
- [x] 将跨服 room/group/shard 连接数配额明确标为本节点准入，不描述为全局一致性配额。证据：docs/cross.md 准入语义说明。
- [x] 明确记录当前路由不提供服务发现、健康路由、自动重连或状态迁移。证据：docs/guide.md 路由说明段与 docs/architecture.md 限制清单。
- [x] 明确记录连接配额统计连接数，不等同于已认证玩家数。证据：docs/architecture.md 限制清单明示。
- [x] 明确记录认证票据、mTLS、重复登录策略由应用集成，框架未实现。证据：docs/architecture.md 限制清单明示。
- [x] 明确记录 session 内存预算为估算值，不等同于 RSS 硬上限。证据：src/node.cpp session_envelope 注释与架构页说明。
- [x] 记录历史游戏“跨服人数过多崩溃”原因尚未由本仓库数据确认，不能声称已复现或根治。证据：docs/architecture-comparison.md 历史问题小节按此口径表述。
- [x] 记录生产容量需通过目标硬件上的负载测试测量，不以示例默认值承诺玩家容量。证据：docs/validation.md 未执行清单与容量口径说明。

## E. C++、依赖与构建复核

- [x] 从干净构建目录执行 Linux Debug 配置并记录完整命令。证据：2026-10-18 干净构建目录 `cmake --preset linux-debug` 成功（VCPKG_ROOT 指向 baseline a5f7362 检出）。
- [x] 从干净构建目录编译 `mistlane` 及可选 probe targets 并记录结果。证据：ninja 全量编译产出 mistlane、mistlane-http-probe、mistlane-mysql-probe。
- [x] 检查最终主程序文件名为 `mistlane`。证据：build/debug/mistlane 存在并完成三段链路回环。
- [x] 使用项目 vcpkg baseline 和 overlay 执行依赖解析并记录 Drogon 实际版本。证据：baseline 解析 drogon overlay 1.9.14、curl 8.23.0、boost 1.92.0、lua 5.5.1。
- [x] 检查 CMake 在 BUILD_TESTING=OFF 时不要求 Python 解释器。证据：CMakeLists.txt find_package(Python3) 位于 if(BUILD_TESTING) 内。
- [x] 检查 CMake 在 BUILD_TESTING=ON 时明确报告 Python 测试依赖。证据：find_package(Python3 REQUIRED COMPONENTS Interpreter) 缺失时配置失败并指明缺 Interpreter。
- [x] 检查默认 vcpkg triplet 及 OpenSSL/Curl TLS 配置与文档一致。证据：presets x64-linux；openssl 3.6.5#2、curl 8.23.0 与 dependencies.md 一致。
- [x] 检查 curl 客户端拒绝非 HTTP/HTTPS 协议的实现路径。证据：src/http_client.cpp CURLOPT_PROTOCOLS_STR="http,https"。
- [x] 检查 curl 客户端的 CA、主机名验证和响应体上限实现路径。证据：src/http_client.cpp VERIFYPEER=1/VERIFYHOST=2/CAINFO 与 receive() 字节上限。
- [x] 检查 Boost.MySQL 池使用成熟官方 connection_pool，而非项目自建池。证据：include/mistlane/mysql_pool.hpp:25-45。
- [x] 检查导出包没有包含构建目录、node_modules、凭据或本地证书。证据：allowlist 严格匹配校验，归档仅含白名单 79 文件+SHA256SUMS。
- [x] 检查 pnpm lockfile 与 package.json 的包管理器版本字段一致。证据：packageManager 字段与 pnpm-lock.yaml 一致，frozen 安装通过。
- [x] 检查 Node.js 最低版本声明满足锁定的 Mermaid/VitePress 依赖要求。证据：docs 指引 Node 22.12+，满足 VitePress 1.6.4/Vite 6.4.4 要求。

## F. 自动化验证与文档站

- [x] 在同步候选后运行 Debug CTest，并记录通过数量与日志位置。证据：2026-10-18 `ctest --preset linux-debug` 全部通过（启用本地库选项 5/5，默认 3/3），主树与隔离核对树均绿。
- [x] 确认集成测试覆盖 TCP gateway/router/worker 的基本往返。证据：tests/integration.py 三段链路用例。
- [x] 确认集成测试覆盖 Drogon HTTP 与 HTTPS。证据：tests/integration.py 两种传输用例（临时证书）。
- [x] 确认集成测试覆盖 Drogon WS 与 WSS。证据：tests/integration.py WS/WSS 用例。
- [x] 确认集成测试覆盖 Lua HTTP handler 和请求生命周期钩子。证据：tests/integration.py 钩子次序/短路/on_error 用例。
- [x] 确认集成测试覆盖跨服握手、room 路由和准入拒绝。证据：tests/integration.py MLC1/ROOM_FULL/租约释放用例。
- [x] 确认集成测试覆盖 Lua 脚本重载失败时旧 VM 继续服务。证据：tests/integration.py reload 回退用例。
- [x] 确认 MariaDB 集成测试使用临时数据目录、临时端口和清理逻辑。证据：tests/mysql_integration.py 临时目录/随机凭据/端口/证书并清理。
- [x] 使用 `pnpm install --frozen-lockfile` 检查文档依赖锁定状态。证据：frozen 安装通过无漂移。
- [x] 使用 `pnpm docs:build` 构建英文与中文文档。证据：VitePress 双语构建成功（含内链检查）。
- [x] 在浏览器打开英文架构图页面并确认 Mermaid SVG 成功渲染。证据：2026-10-18 预览站架构页 svg=1/canvas=1。
- [x] 在浏览器打开中文跨服图页面并确认 Mermaid SVG 成功渲染。证据：中文跨服页 svg=1、console alerts=[]。
- [x] 在浏览器切换浅色/深色主题并确认图中文字对比度可读。证据：VPSwitchAppearance 切换 dark 后图中文字清晰可读，截图留档 /home/cui/.tmp/shots/。
- [x] 在移动视口检查主要架构图和生命周期图没有遮挡页面内容。证据：390x844 视口 scrollW=375≤390 无横向溢出，图不遮挡正文。
- [x] 检查浏览器控制台没有 Mermaid 加载或渲染异常。证据：双语两页 console alerts=[] 零告警。
- [x] 对文档依赖执行安全审计并保存零告警或已接受告警报告。证据：`pnpm audit` 0 告警（docs/validation.md 记录）。
- [x] 评估 VitePress 构建的 Mermaid 大 chunk 警告；若采取拆分，单独记录改动与构建结果。证据：结论为保持懒加载单 chunk 不拆分，构建仅警告无错误（已代决，记录于 validation.md）。

## G. 导出包与独立仓库同步

- [x] 将本 TODO 加入导出 allowlist。证据：`release-files.txt`。
- [x] 执行 `python3 tools/export.py` 生成带 SHA256 清单的源码包。证据：`dist/mistlane-0.1.0.tar.gz`，68 个文件。
- [x] 检查源码包文件列表严格符合 `release-files.txt`。证据：export.py 逐文件比对通过，79 白名单文件无多余无缺失。
- [x] 检查源码包中的 `LICENSE` 是 Apache-2.0 全文。证据：从归档读取首行及版本行。
- [x] 检查源码包中的 README、双语文档、logo、源码、示例和测试完整。证据：归档清单覆盖以上全部类别。
- [x] 在全新临时目录解压源码包并确认其根目录可直接配置 CMake。证据：2026-10-18 全新目录解压后 `cmake --preset linux-debug`（VCPKG_ROOT 按 guide 设置）exit=0。
- [x] 对独立仓库 `cuihairu/mistlane` 检查当前分支、工作区和远端状态。证据：本地 main 与 origin/cuihairu/mistlane 同步推进，本轮按单 pull --rebase 后推送。
- [x] 将已审核的导出源码包同步到独立仓库工作树，不混入老游戏文件。证据：本轮提交内容均出自 allowlist 文件，旧产品前缀扫描零命中。
- [x] 在独立仓库确认 Apache-2.0 是仓库识别到的唯一项目许可证。证据：`gh api repos/cuihairu/mistlane --jq .license.spdx_id`=Apache-2.0（2026-10-18）。
- [x] 在独立仓库检查源码变更差异并确认无意外删除或生成物。证据：本轮提交 diff 仅新增 SPDX 头、文档段落与勾选记录，无删除、无构建产物。
- [x] 提交独立仓库源码同步，并记录提交 SHA。证据：5f9818f（SPDX）、b075621（HTTP worker 文档与验证记录）及本勾选提交。
- [x] 推送已授权的独立仓库分支，并记录远端提交 SHA。证据：git pull --rebase 后推送 0893275..c7dc632 至 origin/main（2026-10-18）。
- [x] 检查 GitHub Actions 对推送提交的构建和文档任务结果。证据：run 39098291345（c7dc632）success 6m4s；此前 run 39084967834/39084464941/39083908978 连续 success。
- [x] 检查 GitHub 仓库许可证页面识别为 Apache License 2.0。证据：GitHub API license.spdx_id=Apache-2.0，仓库页显示 LICENSE。
- [x] 检查 GitHub About、topics、主页链接仍与实际发布状态相符。证据：homepage=https://cuihairu.github.io/mistlane/（HTTP 200）、15 个 topics 与技术栈相符。

## H. 发布后整理

- [ ] 为首个公开源码候选确定版本号与发布说明。
- [ ] 发布说明写明研究学习用途、老游戏抽取背景和业务剥离范围。
- [ ] 发布说明列出当前不支持的生产能力和未验证容量边界。
- [ ] 为源码归档附上 SHA256 校验值。
- [ ] 检查 GitHub 发布页包含 Apache-2.0 许可证链接和 NOTICE。
- [x] 在完成实际部署前，不将 GitHub About 首页设置为尚未发布的文档站地址。证据：Pages 已部署（CI run 39084967834 成功）且 https://cuihairu.github.io/mistlane/ 返回 200，About 主页指向已上线站点。

## I. 老框架组件迁移追踪

- [x] 建立双语能力迁移状态清单，区分重写、替换、部分替换、未迁移和排除项。证据：`docs/migration-status.md`、`docs/zh/migration-status.md`。
- [x] 检查发布候选内没有以旧产品前缀命名的文件。证据：`rg --files mistlane` 前缀扫描无匹配。
- [x] 从旧 C++ 生命周期实现中逐项核对 Node 启动、信号处理、退出顺序和异常路径，并更新状态清单。证据：2026-10-18 对照 game-fogs `d_common/common/src/FogsProcess.cpp`、`FogsServer.hpp` 与 `d_gateway/main.cpp` 逐项核对，migration-status.md 双语新增"旧生命周期核对"表（启动入口、线程模型、信号集、热重载、退出入口、IO/线程收尾、异常路径七项），并更新首行状态指向该节；pnpm docs:build 通过。
- [x] 从旧 Lua 回调实现中逐项核对 init/start/tick/stop/shutdown 映射，并更新状态清单。证据：2026-10-18 对照 game-fogs `d_common/common/src/LuaCallHandler.cpp`（L_initServer/L_doTick/L_closeServer/L_ClientMsg 等 22 个全局回调）与 `LuaContext.hpp` 逐项映射到 LuaService 钩子，migration-status.md 双语新增"旧 Lua 回调核对"表（14 行：init/start/tick/stop/shutdown、消息类、会话事件、注册报文、HTTP、VM/错误模型、库集、热重载），首行状态指向该节；四个锚点在 docs:build 产物中验证存在。
- [x] 追踪旧服务连接注册报文及其调用方，决定是否设计新的服务认证/注册接口。证据：2026-10-18 追踪 `ServerConnBase::onConnSetup`→`L_FirstMsg`→`ServerService:handShakeToServer`（`sp.pack(Protocol.HandShake,{name,sidx,tarName})`，无认证）、被动方 `handleFirstPacket`→`L_Register`（LoginAP/WorldServer/PushServer/OmServer/HegelServer/DataServer 六处调用）、`L_Response` 后切帧与 2 秒重连；决策为不设计新注册/认证接口：静态 UPSTREAMS + MLC1 epoch 握手已覆盖当前形态，动态发现或不可信互联出现时再一并设计；双语 migration-status.md 行已更新，docs:build 通过。
- [x] 追踪旧服务连接重连和发送队列的生产者、消费者与排队字节数，决定是否需要有界可靠投递模块。证据：2026-10-18 核对 `TcpConn.cpp::asyncWrite`（侵入式无界发送链表、>64KB 拆分仅告警、无字节计数）、`ServerConnBase.cpp`（断线 `serverConn_.reset()` 销毁队列不重投、固定 2 秒重连定时器；`pushQueue` 握手前队列全树零调用方为死代码）、生产者 `netapi.hpp::net_asyncWrite` 与 `pushData`；决策为不引入有界可靠投递模块（旧实现无可移植投递保证），有具体应用需求时再定义并带显式字节/条数上限与溢出策略；双语 migration-status.md 行已更新，docs:build 通过。
- [x] 对旧线程池的实际工作负载分类，逐项标记为 Asio I/O、Boost.MySQL 异步 I/O、阻塞任务或已废弃。证据：2026-10-18 分类 `FogsProcess.cpp::start`（tg_ 线程组=Asio I/O）、`HttpRequestService.cpp::start`（固定 pthread 池阻塞 curl=阻塞任务）、`d_push/Task/TaskScheduler.cpp`（CPU 核数池阻塞推送=阻塞任务）、`d_world/HttpServer.hpp`（Poco 池=阻塞 I/O）、`DeadLoopMonitor.hpp`（看门狗线程=已废弃，指令 hook 取代）、旧 MySQL 为 libmysqlclient 同步调用（阻塞）；Mistlane 热路径全异步、curl 适配器仅 CLI，migration-status.md 双语行已更新，docs:build 通过。
- [x] 若存在阻塞任务需求，先评估 `boost::asio::thread_pool`；只为其补充任务准入上限、拒绝策略、取消和关闭适配，不另造线程调度器。证据：2026-10-18 评估结论为当前无阻塞任务需求（src 下无 asio::thread_pool 实例化；curl 适配器仅 cli_client 同步 CLI 调用并在契约中禁止入事件循环；MySQL/Redis 全异步），thread_pool 保持指定载体不落地，出现需求时按准入上限/拒绝策略/取消/关闭四项补充适配；migration-status.md 行已记录，docs:build 通过。
- [x] 比较旧 Lua Timer/TimerQueue 的接口与候选 tick 钩子，确定是否需要 cancellable timeout/interval API。证据：2026-10-18 核对 `game/Util/Timer.lua`（setTimeout/setInterval/remove/内建堆+近期数组、TIMER_LIMIT=100000、MAX_DELAY=5000、xpcall、`xx1` 哨兵自删）与 `TimerQueue.lua`（TimerHeap/TimerArr 双结构、墙钟驱动）；决策为不加 cancellable timeout/interval API：框架内部无消费者且旧需求属应用域，日后确需时按 steady_timer 薄适配+显式契约实现；双语 migration-status.md 行已更新，docs:build 通过。
- [x] 若增加 Lua 定时器 API，为单次、周期、取消、回调异常和重载行为分别定义契约。证据：2026-10-18 前置条件已由上一条决策否定（暂不增加 Lua 定时器 API），契约工作随之搁置；migration-status.md 定时器行已写明日后实现时的四项先行契约（句柄/取消/错误策略/重载销毁+有界），docs:build 通过。
- [x] 对旧 UDP API 的实际调用点做只读盘点；若仍需要，使用 Boost.Asio UDP socket 实现最薄的协议适配。证据：2026-10-18 盘点 `WorldServer.cpp:355`/`CAPI.cpp:147`（唯一服务端实例化 `C_openUdpServer`）、`game/Common/CService/Server/WorldService.lua:99-100`（唯一 Lua 调用点已注释）、`UdpClient`/`L_onUdpClientMsg`/`L_onUdpServerMsg` net 层外零调用方；结论为旧 UDP 属死代码、无需薄适配，保留"用例确认后再加公开 API"；双语 migration-status.md 行已更新，docs:build 通过。
- [x] 对旧 Redis C/Lua API 的调用点做只读盘点，并基于 Boost.Redis 的 RESP3、断线重连和 Sentinel 能力确定集成边界。证据：Redis 适配集中在 Asio worker，Drogon Redis 未启用。
- [x] 检查当前 vcpkg baseline 是否提供所需 Boost.Redis port/feature，并记录解析出的精确版本。证据：`boost-redis` 1.92.0 已配置并由 vcpkg 成功安装。
- [x] 比较 Boost.Redis 与 Drogon Redis 在 worker 线程归属、连接生命周期和配置方式上的差异，选定一个默认实现，避免重复集成。决策：选 Boost.Redis，与现有 Asio/Lua 线程归属一致。
- [x] 为可选 Redis runtime 添加共享连接生命周期、异步回调失效保护、ACL 环境变量读取和 256 个未完成请求上限。证据：`src/lua_service.cpp`、`include/mistlane/lua_service.hpp`。
- [x] 为 Lua Redis Streams 添加 create_group、publish、read、ack、claim API，并对 stream 命名、消息字段、批次和待处理消息恢复行为写明契约。证据：`docs/lua.md`、`docs/zh/lua.md`。
- [x] 为 Lua 排行榜添加原子增分/名次、查询、分页榜单和删除 API，并定义一基名次、并列顺序、精度和分页上限。证据：`docs/lua.md`、`docs/zh/lua.md`。
- [x] 为 Streams 与排行榜 API 增加可重复执行的本地 Redis 集成验证：发布/读取字段、claim 接管 pending、ACK、排行榜增分/查询/分页/删除。证据：`tests/redis_integration.py`，通过 `MISTLANE_TEST_LOCAL_REDIS=ON` 启用。
- [x] 扩展 Redis 集成验证，覆盖消费组重复创建、断线重连和热重载期间旧 Lua 回调失效。证据：2026-10-18 `src/test_databases.cpp`/`src/test_fixtures.hpp` 扩展 redis.lua 用例——SIGSTOP 冻结 Redis 后挂起 stream 回调、SIGHUP 换 VM、SIGCONT 后旧回调被丢弃且新 VM 状态保持纯净；SIGKILL 后同端口重启 Redis，worker 自动重连并可继续收发；`mistlane test --case redis.lua` 5/5 通过，完整 ctest 12/12 绿，双语 validation.md 更新，docs:build 通过。
- [x] 增加 Redis TLS 与 CA/主机名校验配置；在实现前仅使用可信内网 Redis，并确保文档保持此限制说明。证据：2026-10-18 `RedisRuntime` 构造自管 `asio::ssl::context`——`MISTLANE_REDIS_TLS=on` 启用 TLS，强制 `verify_peer|verify_fail_if_no_peer_cert`，按所拨名称做证书链+主机名/IP SAN 校验（无跳过校验选项；Boost.Redis 自身不做任何证书校验，故校验由适配层持有），`MISTLANE_REDIS_CA=FILE` 指定信任锚；新增 `redis.tls` 用例（同名+IP SAN 通过、错误 CA 拒绝、缺 IP SAN 的主机名不匹配拒绝），5/5 稳定，完整 ctest 12/12 绿；明文默认仍限可信内网，双语 lua.md/migration-status.md/validation.md 更新，docs:build 通过。
- [x] 评估 Stream 消息幂等键、保留/裁剪策略、失败重试与死信队列配置；确认默认值前不自动裁剪或 ACK。证据：2026-10-18 决策写入双语 docs/lua.md Streams 契约段——幂等由消费方以 `stream + entry.id` 在处理落库后、ack 前去重，框架不注入幂等键；保留/裁剪默认不自动 `XTRIM`/`MAXLEN`（裁到 pending 以下会困死消息，只在 ack 后由应用裁剪，未来 MAXLEN 选项须显式开启）；重试保持 claim 扫描（min_idle_ms+游标），无自动重试循环；无内置死信队列，应用重试耗尽后自发布到死信 stream 并在发布成功后才 ack。未改任何行为，docs:build 通过。
- [x] 为日志功能确定最小需求：结构化字段、级别、并发写入、轮转和采集边界；优先评估 Boost.Log 作为跨角色统一设施。证据：2026-10-18 决策写入双语 migration-status.md 日志行——行格式定为 UTC 时间戳+级别(error/warn/info，debug 显式开启)+角色+监听地址+消息；`ready <role> <addr>` 保持机器契约；并发写入须覆盖事件循环/web 线程/异步回调；轮转采集默认交进程监督方（journald），文件 sink 与远端聚合留部署层；Boost.Log 1.92 选定为确需第二 sink/级别过滤时的统一门面，现有约 24 处直接输出调用点继续有效，Drogon/Boost.Redis 保留自有日志器；出现具体脚本需求前不暴露 Lua logger API。未改代码，docs:build 通过。
- [x] 对旧监控/系统信息 API 的调用方做只读盘点，决定是否需要 Prometheus/OpenTelemetry 适配。证据：2026-10-18 盘点 game-fogs `d_common/sysinfo`——`initSysCounter()` 存活调用方为 d_gateway/d_login/d_push/d_pay/d_hegel/mergeServer 六个 main 的启动布尔，`gCpuNum` 唯一存活消费方是 d_push `TaskScheduler.cpp:82` 的 ThreadPool 定容；`getCpuUtil`/`gmGetSysInfo` 全部被注释、net 层外零调用方（CPU 利用率采样属死代码）；`L_StateMsg` 仅 StressTest 模拟器与 Cache 服务器的应用消息别名，`L_OmMsg` 为运营客户端消息通道，均非系统指标。结论：保留现有可选 `/metrics` Prometheus 面，CPU/内存交给节点级采集（cgroup/node_exporter），不引入 OpenTelemetry 适配或进程内 sysinfo API；双语 migration-status.md 已更新，docs:build 通过。
- [x] 逐个筛查候选 Lua 通用工具的全局依赖、游戏术语、外部库和来源授权，再决定提取或排除。证据：2026-10-18 逐个筛查 game-fogs `game/Util` 其余 23 个文件——`JSON.lua` 为 GPL 授权 JSON4Lua（硬性排除）；`Aes/Rsa/SnapShotDump/CacheCode` 依赖外部库（luacrypto/lua-openssl/snapshot/sproto Data）或源码内嵌密钥；`Logger/log/SystemUtil/Print/MessageUtil/StringUtil/TableUtil/Date` 依赖旧 `C_*` 全局且 `Print` 覆写 `print`；`DropTableUtil/ServerUtil/TimeExecute/Timer/TimerQueue` 属游戏领域/旧拓扑/已定案 timer；`Math` 需 `bit32`（沙箱不开放）；`VerificationUtil/TimeUtil` 属应用层。结论：全部排除不提取，`Base64/StringUtil/TableUtil/Sort` 出现具体消费方时再逐个带测试与来源重审；双语 migration-status.md 已更新，docs:build 通过。
- [x] 将每个决定迁移的通用模块拆成单独实现任务，写明接口、许可证来源和独立验收条件。证据：2026-10-18 复核全部已定案通用模块——MinHeap 已提取，timer/线程池/UDP/sysinfo/Lua 工具经 I144-I148、I159-I161 审查全部为「不迁移」或「出现具体需求再实现」，当前零个已批准待实现模块，故无可拆分任务；已把规则写入双语 migration-status.md「Extraction naming and provenance」段：今后任何批准须先立独立任务写明公开接口、许可证来源与独立验收条件再写代码。docs:build 通过。
- [x] 保持游戏玩法、账号、支付、地图导航、线上配置和 SDK 不进入发布包。证据：release-files.txt 白名单扫描无 gameplay/account/payment/billing/recharge/nav/worldmap/sdk/charge 路径；归档比对通过（2026-10-18 复核）。
- [x] 保持公开文件名采用 Mistlane 命名，不使用旧产品前缀；同时保留准确的 NOTICE 和第三方来源说明。证据：旧产品前缀扫描零命中；未跟踪本地目录（build/licenses/uploads）不入库；NOTICE 与 THIRD_PARTY.md 在归档内。

## J. 崩溃报告、Prometheus、内部探测与性能剖析

- [x] 确认旧 Lua `C_monitorDeadLoop` 的职责与边界：超时后设置 Lua 指令 hook 尝试中断脚本；它不是 CPU/内存 profiler，也不是服务间健康探测。证据：旧实现 `d_common/common/src/DeadLoopMonitor.cpp` 与 `LuaContext.hpp`。
- [x] 明确旧框架与当前候选均没有 CPU 采样、内存分配或 Lua 函数级 profiler。证据：源码检索与迁移状态文档。
- [x] 将 Crashpad 和 `prometheus-cpp` pull 加入 vcpkg manifest。证据：`vcpkg.json`、Debug 构建和诊断启动测试。
- [x] 用固定 baseline 在 Linux Debug triplet 完成 Crashpad 与 prometheus-cpp 的 vcpkg 安装；handler 位于 triplet tools/crashpad_handler。
- [x] 在 CMake 中链接 `crashpad::crashpad` 与 Prometheus core/pull targets；当前均为核心依赖。
- [x] 为启动参数添加可选 Crashpad handler 路径和转储数据库目录；必须同时提供，上传默认关闭。
- [x] 在创建网络与 Lua 服务前初始化 Crashpad；annotation 仅含 role 和产品名。
- [x] Crashpad 不传入上传 URL，并显式关闭数据库上传设置；目录权限、磁盘配额、轮转/清理策略和运维提取方式仍待定义。
- [x] 为 Linux Crashpad handler 增加部署复制/安装规则，并检查静态链接、运行库和容器 ptrace/seccomp 限制。证据：2026-10-18 CMakeLists 增加 GNUInstallDirs 安装规则——`cmake --install` 把 `mistlane` 与 vcpkg `crashpad_handler` 并排装进 `PREFIX/bin`；实测两者仅依赖 glibc/libstdc++/libgcc_s/libm（第三方全静态）；安装的 handler 配合服务端通过 `mistlane test --suite crashpad`；双语 guide 新增 Crashpad 部署约定（同目录+本地数据库+上传关闭+按保留策略定容），migration-status 崩溃报告行与双语 validation 记录冒烟结果；容器限制记录为 ptrace 必需（Docker 默认 profile 在受支持内核放行，加固/无特权/自定义 AppArmor|seccomp 可能拒绝——被拒时节点照常服务但不写 minidump，依赖 dump 前须先做受控崩溃演练）。
- [x] 增加独立崩溃样例程序，在隔离临时目录触发可控 native crash，断言 Crashpad 生成可识别 minidump 后清理文件。证据：2026-10-18 新增 `examples/crash_sample.cpp`（同款本地管线：本地数据库+上传关闭+StartHandler，随后解引用空指针）与 `mistlane-crash-sample` 目标（随 `mistlane` 一起安装到 `bin/`）；原生 `crashpad.minidump` 用例在隔离临时目录断言数据库递归出现大于 4 KiB 且以 `MDMP` 魔数开头的真实 minidump、样例进程以 128+SIGSEGV 退出、handler 组显式清理无残留（test_support Child 增加 `group()` 访问器并关组清理）；`mistlane test --suite crashpad` 2/2、单用例连续四次通过，完整 ctest 12/12 绿；双语 guide/validation/migration-status 更新，docs:build 通过。
- [x] 为 release 与调试构建归档匹配的未剥离符号/Build ID，记录用 minidump_stackwalk 符号化的复现步骤。证据：2026-10-18 新增 `tools/crash_symbols.py`（build-id 读 ELF `NT_GNU_BUILD_ID` note、modules 列 dump 模块表、match 按 dump 内 CodeViewRecordBuildID 与文件 build id 配对）；对演练产物实测——crash-sample build id `5232a2a0…` 与 readelf 一致、dump 七个模块 id 全列出、match 正例 exit 0 / 异构建 exit 1；两个 preset 归档步骤与"保留未剥离副本"写入双语 guide 符号归档小节（含 minidump_stackwalk 步骤；vcpkg port 仅带 crashpad_handler 故未本地走栈）；`tools/export.py` 白名单更新并跑通（81 文件），docs:build 通过。Release 树本轮未本地构建：正式发布物归用户发起，归档命令对两 preset 完全相同。
- [x] 实现 Prometheus Registry 与 role 低基数标签，不使用玩家、房间、会话 ID 作为 label。
- [x] 添加可配置 metrics 监听地址和端口，默认禁用；单独监听，不复用玩家协议端口。
- [x] 添加 `mistlane_ready`、活动会话 gauge、准入拒绝 counter，并以 role 作为标签；build/version/node identity 指标仍待补。
- [x] 为 TCP/cross/shard 添加收发消息数/字节、请求耗时直方图、上游错误和超时计数，并限制直方图桶数与 label 集合。证据：2026-10-18 `Diagnostics` 新增 `mistlane_messages_total`/`mistlane_payload_bytes_total`（direction∈{rx,tx}）、`mistlane_request_seconds`（11 桶，客户端收到→回给客户端的整链耗时）、`mistlane_upstream_errors_total`（连接/写/读/握手失败）、`mistlane_upstream_timeouts_total`（带上游会话的截止超时）；全部指标仅 role 标签+direction 闭集，Session 在 rx/tx/reply/forward/连接失败/截止路径埋点；`transport.tcp` 断言 worker 活跃值非零、死端口 gateway 错误≥1、健康节点错误/超时=0，用例 5/5 稳定，完整 ctest 12/12 绿；双语 cross-server/migration-status/validation 更新，docs:build 通过。
- [x] 为 Lua message/http handler 记录耗时；tick 耗时和错误分类仍待补。
- [x] 记录 Asio 100ms tick 的调度延迟直方图/最大值，让事件循环阻塞能被观测；测量本身不得阻塞 Lua loop。证据：2026-10-18 `Node::tick` 改用 `expires_at` 并在回调首行以纯时间戳计算 scheduled→fired 延迟（不触碰 Lua，不阻塞事件循环），`mistlane_event_loop_lag_seconds` 直方图（14 桶，0.5ms–10s）+ `mistlane_event_loop_lag_max_seconds` 运行期最大 gauge（原 gauge 从未接线，改为 max 语义）；`transport.tcp` 等待首个调度 tick 后断言 count/max 非零，5/5 稳定；双语文档同步。
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
- [x] 用 `systemd-analyze verify` 检查替换安装路径后的 unit 文件，并用临时系统用户做一次 systemd 启停演练。证据：2026-10-18 systemd-analyze verify 零告警；创建系统用户 mistlane、安装 /opt/mistlane 模板，mistlane@worker 于 7102 激活、journal 记录 ready、帧客户端回环 echo 成功、stop 干净退出；演练后清理 unit/二进制//opt/用户。
- [x] 为逐进程 shell 配置文档补充合法字段、逗号分隔 upstream、密钥应使用 systemd credentials/environment，而非提交到模板仓库。证据：2026-10-18 guide.md 与 zh/guide.md 新增"实例配置字段"表（ROLE/LISTEN/TRANSPORT/SCRIPT/UPSTREAMS/ROUTE_EPOCH/CERT/KEY，与 ops/run-instance.sh 逐字段核对），注明 UPSTREAMS 逗号分隔、wss/https 证书拒绝启动行为与机密经 systemd unit 注入；pnpm docs:build 双语构建与内链检查通过。
- [x] 在模板上增加进程间依赖/健康检查与启动失败回滚，避免用固定 sleep 代替 readiness；当前启动器只做短暂启动确认。证据：2026-10-18 ops/cluster.sh 每个 start 轮询 ready 行（MISTLANE_START_TIMEOUT 默认 5 秒），未就绪即 TERM 并移除 pidfile；start 失败只回滚本次命令启动的实例并保留已有实例，restart 失败反向停止已启动项；scaffold.init 覆盖"就绪门通过 + 坏实例回滚 + 已有实例保留 + 二次失败全停"四条路径。
- [ ] 决定是否提供原生 Mistlane config-file schema；若实现，定义格式版本、字段校验、环境覆盖、相对路径规则和错误提示，再将 shell env 模板迁移到该 schema。
- [ ] 按架构比较文档继续补内部 liveness/readiness 探测、Crashpad 转储生成/保留与部署工作流；再依据真实压力测试决定动态 registry、lease/fencing、房间 drain/migration 或容器编排需求。
