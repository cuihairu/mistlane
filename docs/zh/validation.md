# 验证记录

验证日期：2026-10-17；HTTP/HTTPS、Lua HTTP worker 与原生 cross/shard 检查同 Redis 新增项于 2026-10-18 验证。环境为 Linux x64、GCC 15.2.0、CMake 4.2.3，固定 vcpkg baseline：a5f736224e85b32e25a8ffc52562333180396642。

| 项目 | 实际结果 |
|---|---|
| CMake 配置与完整 Debug 构建 | 通过，实际使用 vcpkg 的 Boost 1.92.0、Lua 5.5.1、OpenSSL 3.6.5#2 |
| 三段 TCP 链路 | gateway → router → worker 往返通过 |
| TCP 分帧 | 空、二进制、UTF-8、上限长度、拆包、合包与超大报文通过 |
| 并发与路由 | 并发请求及会话固定上游通过 |
| WS / WSS | Drogon 握手、掩码、分片、ping/pong、受信任证书 TLS echo 通过 |
| HTTP / HTTPS | 两种传输下的 Drogon 请求、二进制正文，以及 curl 适配的证书 / 主机名校验、仅允许 HTTP/HTTPS 的协议限制和响应大小上限通过 |
| Lua HTTP worker | 请求元数据、before_request 短路、after_request 修改、on_error 兜底、响应校验与重载回退通过 |
| 原生 cross/shard | MLC1 握手、group/room 固定到同一 shard、ROOM_FULL 最终准入、断连释放租约、EPOCH 不匹配与上游失败隔离通过 |
| Lua | 成功重载、语法 / 接口错误回退、回调错误、典型死循环中断、重复回调通过 |
| 提取的堆工具 | 在实际链接的 Lua 中验证模块隔离与有序弹出 |
| 错误和资源路径 | 上游连接失败、连接上限、消息限流、非法参数、SIGTERM 退出通过 |
| 数据库池 | 池上限为一，连续三次获取 / 查询 / 自动归还通过 |
| 参数与 TLS | 引号 / 反斜线参数绑定、CA 和主机名校验通过 |
| 数据库认证错误 | 在配置的获取期限内失败 |
| Redis Streams 与排行榜 | 本地 Redis 端到端 Lua 检查通过，覆盖消费组幂等创建、发布/读取、pending 认领与 ACK、增分/名次查询、榜单分页和删除 |
| 崩溃演练（minidump） | `mistlane-crash-sample` 经同一套本地管线执行受控空指针解引用：crashpad 数据库落出真实 minidump（大于 4 KiB 且以 `MDMP` 魔数开头）、样例进程因 `SIGSEGV` 退出、handler 无残留（`mistlane test --case crashpad.minidump` 连续四次通过） |
| dump 与二进制关联 | `tools/crash_symbols.py build-id` 返回样例的 `NT_GNU_BUILD_ID`（与 `readelf -n` 一致），`modules` 列出 dump 全部七个模块的 build id，`match` 将演练 dump 与确切样例二进制配对（exit 0）、拒绝另一构建（exit 1）；`minidump_stackwalk` 完整走栈步骤已记录但未本地执行，因 vcpkg crashpad port 只带 `crashpad_handler` |
| 流量与 tick 指标 | `transport.tcp` 抓取实时 `/metrics` 输出，断言收发消息计数非零、tx 字节数超过 1 MiB、请求耗时直方图有观测、事件循环 tick 延迟直方图与运行期最大值非零（等待首个调度 tick 后）、健康 worker 的上游错误/超时为零，以及指向死端口的 gateway 至少记录一次上游错误 |
| 内部探测（PROBE1） | `probe.control` 以裸 TCP 驱动协议：liveness/version/command/malformed/nonce 错误路径、带 `active`/`capacity`/`lag_ms` 的就绪应答、空闲连接截止、`--overload-ratio 0.5 --ready-hysteresis 2` 下饱和驱动的 `ready -> notready -> ready` 翻转、`MISTLANE_PROBE_TOKEN` 下的 `err auth`，以及经 `IP:PORT:PROBE` 的 cross 门控（明确 notready 返回 `MLC1 ERR NOT_READY`、探测端口不可达返回 `MLC1 ERR UPSTREAM`、不带探测端口的上游不门控、已建立房间不受影响） |
| 文档站 | 使用 pnpm 10.33.0、VitePress 1.6.4 与修复版 Vite 6.4.4 构建两种语言；10 个页面的语言、导航与仓库链接检查通过 |
| 文档依赖审计 | Vite 覆盖更新后，pnpm audit 返回零告警 |
| 源码包 | 白名单与 SHA256 检查通过，无原游戏目录或 Git 历史 |

决策记录（2026-10-18 巡检代决）：对 VitePress 构建中 Mermaid 懒加载产物触发的大 chunk 警告完成评估，维持单 chunk 懒加载布局、不做拆分；该警告不影响构建成败。仅当后续 bundle 审计显示实际加载代价时再复议。

数据库检查使用临时 **MariaDB 11.8.6**。这验证了适配器在 MariaDB 下的行为，不等于覆盖 Oracle MySQL 各版本。脚本自行创建临时数据目录、随机凭据、端口和证书，结束后清理。

执行命令：

```bash
cmake --preset linux-debug -DMISTLANE_TEST_LOCAL_DATABASE=ON -DMISTLANE_TEST_LOCAL_REDIS=ON
cmake --build --preset linux-debug
ctest --preset linux-debug -R '^mistlane_integration$'
python3 tests/mysql_integration.py ./build/debug/mistlane-mysql-probe
pnpm install --frozen-lockfile
pnpm docs:build
python3 tools/export.py
```

启用后，数据库与 Redis 检查分别注册为 `mistlane_mysql_integration` 和 `mistlane_redis_integration`。Redis 集成测试会启动临时本地 Redis，覆盖消费组重复创建、服务端被杀后在同一端口重启后的断线重连、Lua 热重载期间仍挂起的 stream 回调（被替换 VM 持有的回调必须被丢弃），以及仅 TLS 的 Redis 服务端（CA 与名称匹配时接受；错误 CA 和缺少 IP 名称时拒绝）；完整 CTest 会执行所有已启用的检查。

依赖安装期间，当前环境的临时文件系统拒绝写入较大的 OpenSSL 归档；把 vcpkg 工作目录移到普通磁盘后完成构建。没有为此修改 port 源码或降低依赖版本。

安装规则已于 2026-10-18 冒烟验证：`cmake --install` 安装到临时前缀后 `bin/` 下同时得到 `mistlane` 与 `crashpad_handler`，且安装的 handler 配合刚构建的服务端通过 crashpad 启动套件；两个二进制只依赖系统 C/C++ 运行库。

未执行：原游戏集群启动和业务回归、生产容量压力、长期稳定性、其他平台、完整游戏迁移、Oracle MySQL 版本矩阵。GitHub 托管 CI 的当前状态见 [Actions](https://github.com/cuihairu/mistlane/actions)。当前结果服务于研究与学习，不构成生产就绪结论。
