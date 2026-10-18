# 使用指南

本指南用于运行从老游戏中抽取的框架，供研究与学习，不部署原游戏。

## 环境要求

Linux x64、C++20 编译器、CMake 3.25+、Ninja、Git、curl、zip/unzip、tar、pkg-config、Perl、Python 3.9+ 和 OpenSSL 命令行工具。文档另需 Node.js 22.12+ 和 package.json 固定版本的 pnpm；使用 Corepack shim 时请先启用。

## 准备 vcpkg

```bash
git clone https://github.com/microsoft/vcpkg.git "$HOME/vcpkg-mistlane"
git -C "$HOME/vcpkg-mistlane" checkout a5f736224e85b32e25a8ffc52562333180396642
"$HOME/vcpkg-mistlane/bootstrap-vcpkg.sh" -disableMetrics
export VCPKG_ROOT="$HOME/vcpkg-mistlane"
```

已有安装可复用，但需包含固定 baseline 和对应版本数据库。CMake 从 VCPKG_ROOT 获取工具链；个人路径可放入已忽略的 CMakeUserPresets.json。

## 构建与检查

在仓库根目录运行：

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

Release 使用 linux-release。首次配置由 vcpkg 安装依赖，之后复用二进制缓存。具体版本见[依赖管理](./dependencies.md)。

## 初始化应用模板

```bash
./build/debug/mistlane init ./my-service
```

目标目录非空时会拒绝覆盖。生成内容包括逐进程配置 `config/instances/`、Lua 入口 `scripts/`、按项目根目录解析路径的 `ops/run-instance.sh`、支持本地启停/重启/状态/日志的 `ops/cluster.sh`，以及 `ops/systemd/` 下的 unit 模板。配置文件是由启动脚本读取的可信 shell 赋值；Mistlane 本身仍接收 CLI 参数，不解析原生配置文件。旧项目部署结构和新模板设计依据见[架构对比](./architecture-comparison.md)。

## 启动三个进程

在仓库根目录的三个终端分别运行：

```bash
./build/debug/mistlane --role worker --listen 127.0.0.1:7102 --script examples/echo.lua
./build/debug/mistlane --role router --listen 127.0.0.1:7101 --upstream 127.0.0.1:7102
./build/debug/mistlane --role gateway --listen 127.0.0.1:7100 --upstream 127.0.0.1:7101
```

发送请求：

```bash
python3 tools/client.py 'hello mistlane'
```

--upstream IP:PORT 可重复指定，以连接为单位轮询分配。同一会话固定到同一上游，尚不按账号或玩家 ID 路由。

## WebSocket 与 TLS

网关增加 --transport ws 即可接收 WebSocket 消息。一条 WS 消息对应一个 payload，无须添加内部 TCP 长度头。

```bash
./build/debug/mistlane --role gateway --listen 127.0.0.1:7443 \
  --transport wss --cert /path/to/fullchain.pem --key /path/to/private-key.pem \
  --upstream 127.0.0.1:7101
```

使用自己的证书链和私钥，保存在源码仓库之外。协议细节由 Drogon 处理，WSS 最低 TLS 1.2。响应为二进制 WS 消息。内部 TCP 使用四字节网络序长度头加 payload，上限 1 MiB。

## HTTP worker

除了基于帧 TCP/WS 的 Lua `message` 模式，worker 还可以直接提供 HTTP 服务。用 `--transport http`（或 `https`）配合 HTTP 脚本（如 `examples/http.lua`）启动：

```bash
./build/debug/mistlane --role worker --listen 127.0.0.1:7500 \
  --transport http --script examples/http.lua
curl -s http://127.0.0.1:7500/healthz
```

HTTPS 需另加 `--cert` 和 `--key`。传给 Lua 的请求表包含 method、path、query、headers 和 body；响应表设置 status、headers 和 body。请求生命周期钩子为 `before_request`、`http`、`after_request`、`on_error`，以及通用的 `init`/`start`/`tick`/`stop`/`shutdown` 回调。HTTP worker 必须提供 `http`，可省略 `message`；帧 worker 必须提供 `message`。限制与钩子契约见 [HTTP 参考](./http.md) 和 [Lua 生命周期](./lua.md)。

## Lua 服务

脚本返回 table，必须提供 message(payload) -> string，可选 init()、tick() 和 shutdown()。tick 间隔 100ms。所有回调在节点的单个事件循环线程上执行，应快速返回。

SIGHUP 先构造并初始化新 VM，失败保留旧 VM，成功替换并重置模块状态。目前不进行状态迁移。SIGTERM/SIGINT 停止监听、取消会话并调用当前模块的 shutdown。

脚本属于可信应用扩展，不是执行不可信代码的安全沙箱。指令 hook 可以中断典型死循环，但不提供完整 CPU 或内存隔离。

## MySQL 连接池

include/mistlane/mysql_pool.hpp 只配置已有的 Boost.MySQL 池。异步获取连接后通过 pooled_connection 的生命周期自动归还；查询值交给 with_params 绑定。

```bash
export MISTLANE_MYSQL_HOST=localhost
export MISTLANE_MYSQL_PORT=3306
export MISTLANE_MYSQL_USER=your_user
# 从你的环境注入 MISTLANE_MYSQL_PASSWORD。
./build/debug/mistlane-mysql-probe
```

默认要求 TLS，验证 CA 和主机名；私有 CA 可通过 SSL_CERT_FILE 指定。MISTLANE_MYSQL_TLS=disable 仅作为明确的临时本地测试选项。示例池上限为一，连续执行三次参数化查询来检查获取与归还。

安装 MariaDB 工具后，可运行临时数据库检查：

```bash
python3 tests/mysql_integration.py ./build/debug/mistlane-mysql-probe
```

脚本创建临时数据目录、随机凭据、临时端口和证书，结束时停止并清理服务。配置 -DMISTLANE_TEST_LOCAL_DATABASE=ON 可纳入 CTest。

池适配当前尚未接入 Lua 消息回调，请勿在 Lua 事件循环中执行阻塞 SQL。

## 文档站

```bash
pnpm install --frozen-lockfile
pnpm docs:dev
pnpm docs:build
pnpm docs:preview
```

文档提供中英文导航、本地搜索和成对页面。默认 base 为 /mistlane/；部署到域名根目录时可设置 DOCS_BASE=/。构建会检查内部链接并生成 docs/.vitepress/dist，以上命令不执行托管发布。

## 导出源码

```bash
python3 tools/export.py
```

归档仅包含 release-files.txt 显式清单中的文件和 SHA256 校验值，不包含父仓库历史、私有配置、原游戏玩法和预编译二进制。
