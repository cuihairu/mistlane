# 验证记录

验证日期：2026-10-17；HTTP/HTTPS、Lua apiServer 与原生 gateway cross routing/crossServer 检查同 Redis 新增项于 2026-10-18 验证；多监听、WebSocket 生命周期、服务身份与共享连接多路复用检查于 2026-10-19 验证。环境为 Linux x64、GCC 15.2.0、CMake 4.2.3，固定 vcpkg baseline：a5f736224e85b32e25a8ffc52562333180396642。

| 项目 | 实际结果 |
|---|---|
| CMake 配置与完整 Debug 构建 | 通过，实际使用 vcpkg 的 Boost 1.92.0、Lua 5.5.1、OpenSSL 3.6.5#2 |
| 三段 TCP 链路 | gateway → forwarding gateway → gameServer 往返通过 |
| TCP 分帧 | 空、二进制、UTF-8、上限长度、拆包、合包与超大报文通过 |
| 并发与路由 | 并发请求及会话固定上游通过 |
| WS / WSS | Drogon 握手、掩码、分片、ping/pong、受信任证书 TLS echo 通过 |
| 会话推送与广播 | `transport.push` 启动带 TCP+WS 监听的 gameServer 并置于 gateway 之后，验证全双工会话：message context 中的会话身份、handler 返回 nil 后会话仍可用、`mistlane.session.send` 先于响应到达、广播覆盖全部帧式会话（直连 TCP、经 gateway、WS 链路）、约 32×64 KiB 推送后触发积压拒绝并以有界 `queued+refused` 报告而非 Lua 报错或断连、`unknown session id` 与 `payload exceeds 1 MiB` 原因、gateway 无请求转发主动推送、节点日志无 `callback failed` |
| 请求关联（MLC2） | `transport.correlate` 在同一节点组合上验证可选关联模式：仅首帧生效的握手（`MLC2` → `MLC2 OK`）、直连/gateway/WS 三链路响应逐字节回显 u32 BE 请求 ID、Lua 可见 `context.request`、推送与广播携带 ID 0 而同服务器未握手会话保持原帧格式、越界的关联推送返回 `payload exceeds 1 MiB` 而非 handler 报错、cross 路由以 `MLC1 ERR INVALID` 拒绝 `MLC2`、节点日志无 `callback failed` |
| 服务身份（MLS1） | `transport.identity` 启动一个要求令牌与一个普通 gameServer，配四种 gateway（令牌匹配、缺令牌、令牌错误、对端多余令牌）：匿名或格式错误的首帧得到 `MLS1 ERR TOKEN`/`MLS1 ERR INVALID` 并关闭且不进入脚本、`MLS1 gateway inst-9 s3cret` 通过后会话行为正常、`MLC2` 可在已接受握手的会话上叠加（直连与经 gateway 均验证）、握手对 gateway 客户端不可见、上游拒绝时关闭会话且不透传原因而缺令牌 gateway 会透传、普通服务端返回 `MLS1 ERR UNEXPECTED`、两令牌参数作用于 cross 路由时拒绝启动 |
| 共享连接多路复用（MLM1） | `transport.multiplex` 在 `--upstream-multiplex on` 的 gateway 后启动要求令牌的 gameServer 并验证：两个 gateway 会话共享一条 ESTABLISHED 上游套接字（`/proc/net/tcp`）、逐逻辑会话的 `MLC2` 握手走单 id 载荷且 id 精确回显、推送携带 id 0、服务端 `mistlane_active_sessions` 计入逻辑会话、直连 `MLS1` 链路与多路复用链路共存、双向 `CLOSE` 关闭后广播只达存活链路、handler 报错仅关闭自己的逻辑会话（日志留痕、链路与同伴存活）、客户端发来的 `MLM1` 直连/会话中/经 gateway 三种情形均被 `MLM1 ERR UNEXPECTED` 拒绝、缺 `--upstream-token` 的 gateway 带该开关拒绝启动 |
| HTTP / HTTPS | 两种传输下的 Drogon 请求、二进制正文，以及 curl 适配的证书 / 主机名校验、仅允许 HTTP/HTTPS 的协议限制和响应大小上限通过 |
| 多监听与 WebSocket 生命周期 | `transport.listeners` 启动混合 gameServer（TCP 主监听 + WS + WSS，WSS 用 `?cert=&key=` 单独覆盖证书）、apiServer HTTP 节点（`/`、`/ws` 升级进入 Lua `websocket_open/message/close` 生命周期，`mistlane.websocket.send` 覆盖成功、`unknown websocket id`、`payload exceeds 1 MiB`）、`websocket_message` 内 Lua 报错只关闭该会话且 HTTP 服务继续应答、SIGTERM 排空在线会话、慢消费者越过 2 MiB 输出高水位被强制关闭、HTTPS+WSS 单监听证书、gateway HTTP 监听承载游戏会话升级、配置文件 `LISTENERS=`，以及脚本角色 TCP 监听与重复端口的启动拒绝 |
| Lua apiServer | 请求元数据、before_request 短路、after_request 修改、on_error 兜底、响应校验与重载回退通过 |
| 原生 gateway cross routing/crossServer | MLC1 握手、group/room 固定到同一 crossServer、ROOM_FULL 最终准入、断连释放租约、EPOCH 不匹配与上游失败隔离通过 |
| Lua | 成功重载、语法 / 接口错误回退、回调错误、典型死循环中断、重复回调通过 |
| 提取的堆工具 | 在实际链接的 Lua 中验证模块隔离与有序弹出 |
| 错误和资源路径 | 上游连接失败、连接上限、消息限流、非法参数、SIGTERM 退出通过 |
| 数据库池 | 池上限为一，连续三次获取 / 查询 / 自动归还通过 |
| 参数与 TLS | 引号 / 反斜线参数绑定、CA 和主机名校验通过 |
| 数据库认证错误 | 在配置的获取期限内失败 |
| Redis Streams 与排行榜 | 本地 Redis 端到端 Lua 检查通过，覆盖消费组幂等创建、发布/读取、pending 认领与 ACK、增分/名次查询、榜单分页和删除 |
| 崩溃演练（minidump） | `mistlane-crash-sample` 经同一套本地管线执行受控空指针解引用：crashpad 数据库落出真实 minidump（大于 4 KiB 且以 `MDMP` 魔数开头）、样例进程因 `SIGSEGV` 退出、handler 无残留（`mistlane test --case crashpad.minidump` 连续四次通过） |
| dump 与二进制关联 | `tools/crash_symbols.py build-id` 返回样例的 `NT_GNU_BUILD_ID`（与 `readelf -n` 一致），`modules` 列出 dump 全部七个模块的 build id，`match` 将演练 dump 与确切样例二进制配对（exit 0）、拒绝另一构建（exit 1）；`minidump_stackwalk` 完整走栈步骤已记录但未本地执行，因 vcpkg crashpad port 只带 `crashpad_handler` |
| 流量与 tick 指标 | `transport.tcp` 抓取实时 `/metrics` 输出，断言收发消息计数非零、tx 字节数超过 1 MiB、请求耗时直方图有观测、事件循环 tick 延迟直方图与运行期最大值非零（等待首个调度 tick 后）、健康 gameServer 的上游错误/超时为零，以及指向死端口的 gateway 至少记录一次上游错误 |
| 内部探测（PROBE1） | `probe.control` 以裸 TCP 驱动协议：liveness/version/command/malformed/nonce 错误路径、带 `active`/`capacity`/`lag_ms` 的就绪应答、空闲连接截止、`--overload-ratio 0.5 --ready-hysteresis 2` 下饱和驱动的 `ready -> notready -> ready` 翻转并与 `mistlane_ready` 指标一致、`MISTLANE_PROBE_TOKEN` 下的 `err auth`，以及经 `IP:PORT:PROBE` 的 cross 门控（明确 notready 返回 `MLC1 ERR NOT_READY`、探测端口不可达返回 `MLC1 ERR UPSTREAM`、不带探测端口的上游不门控、已建立房间不受影响，被拒加入计入 `mistlane_admission_rejections_total`），另覆盖连续 50 次探测生命周期、300 字节超长请求行被无应答关闭、64 个挂起连接外的第 65 个被拒，以及 `[::1]` 方括号 IPv6 的监听/探测/上游地址（带门控与不门控） |
| 原生配置文件 | `config.file` 用 `# mistlane-config 1` 文件启动 gameServer（role、listen、相对文件解析的 script、transport、容量上限），拒绝缺失版本行、未知键与重复键并报「文件:行号」，`MISTLANE_MAX_CONNECTIONS` 覆盖文件值、显式参数再覆盖两者，`ROOT=` 支持项目根风格条目，文件中的 `PROBE_LISTEN`/`METRICS_LISTEN` 生效 |
| 文档站 | 使用 pnpm 10.33.0、VitePress 1.6.4 与修复版 Vite 6.4.4 构建两种语言；26 个导航页面（英文 13、中文 13）的语言、导航与仓库链接检查通过 |
| 文档依赖审计 | Vite 覆盖更新后，pnpm audit 返回零告警 |
| 源码包 | 白名单与 SHA256 检查通过，无原游戏目录或 Git 历史 |

决策记录（2026-10-18 巡检代决）：对 VitePress 构建中 Mermaid 懒加载产物触发的大 chunk 警告完成评估，维持单 chunk 懒加载布局、不做拆分；该警告不影响构建成败。仅当后续 bundle 审计显示实际加载代价时再复议。

数据库检查使用临时 **MariaDB 11.8.6**。这验证了适配器在 MariaDB 下的行为，不等于覆盖 Oracle MySQL 各版本。脚本自行创建临时数据目录、随机凭据、端口和证书，结束后清理。

执行命令：

```bash
cmake --preset linux-debug -DMISTLANE_TEST_LOCAL_DATABASE=ON -DMISTLANE_TEST_LOCAL_REDIS=ON
cmake --build --preset linux-debug
ctest --preset linux-debug
python3 tests/integration.py ./build/debug/mistlane .
python3 tests/mysql_integration.py ./build/debug/mistlane-mysql-probe
pnpm install --frozen-lockfile
pnpm docs:build
pnpm docs:check
python3 tools/export.py
```

启用后，数据库与 Redis 检查分别注册为 `mistlane_mysql_integration` 和 `mistlane_redis_integration`。Redis 集成测试会启动临时本地 Redis，覆盖消费组重复创建、服务端被杀后在同一端口重启后的断线重连、Lua 热重载期间仍挂起的 stream 回调（被替换 VM 持有的回调必须被丢弃），以及仅 TLS 的 Redis 服务端（CA 与名称匹配时接受；错误 CA 和缺少 IP 名称时拒绝）；完整 CTest 会执行所有已启用的检查。

依赖安装期间，当前环境的临时文件系统拒绝写入较大的 OpenSSL 归档；把 vcpkg 工作目录移到普通磁盘后完成构建。没有为此修改 port 源码或降低依赖版本。

安装规则已于 2026-10-18 冒烟验证：`cmake --install` 安装到临时前缀后 `bin/` 下同时得到 `mistlane` 与 `crashpad_handler`，且安装的 handler 配合刚构建的服务端通过 crashpad 启动套件；两个二进制只依赖系统 C/C++ 运行库。

未执行：原游戏集群启动和业务回归、生产容量压力、长期稳定性、其他平台、完整游戏迁移、Oracle MySQL 版本矩阵。GitHub 托管 CI 的当前状态见 [Actions](https://github.com/cuihairu/mistlane/actions)。当前结果服务于研究与学习，不构成生产就绪结论。
