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

目标目录非空时会拒绝覆盖。生成内容包括逐进程配置 `config/instances/`、Lua 入口 `scripts/`、按项目根目录解析路径的 `ops/run-instance.sh`、支持本地启停/重启/状态/日志的 `ops/cluster.sh`，以及 `ops/systemd/` 下的 unit 模板。配置文件由启动脚本读取；运行时也可解析原生 `--config FILE` 格式（见下文）。旧项目部署结构和新模板设计依据见[架构对比](./architecture-comparison.md)。

## 启动三个进程

在仓库根目录的三个终端分别运行：

```bash
./build/debug/mistlane --role gameServer --listen 127.0.0.1:7102 --script examples/echo.lua
./build/debug/mistlane --role gateway --listen 127.0.0.1:7100 --upstream 127.0.0.1:7102
```

发送请求：

```bash
python3 tools/client.py 'hello mistlane'
```

--upstream IP:PORT 可重复指定，以连接为单位轮询分配。同一会话固定到同一上游，尚不按账号或玩家 ID 路由。

## 游戏客户端使用 WS / WSS

游戏客户端可以直接连接 Mistlane 的 WebSocket 服务。WS/WSS 由 Drogon 监听，升级握手完成后传递持续连接上的游戏消息，Lua 入口为 `message(payload)`。HTTP API 使用 `http(request)`；`POST /message` 是另一种 HTTP 消息适配入口。选择 WS/WSS 时不调用 HTTP Lua handler。

| 用途 | 角色与参数 | 客户端地址 | Lua 入口 |
|---|---|---|---|
| 游戏长连接，经网关转发 | `gateway --transport ws/wss --upstream IP:PORT` | `ws://host:port/ws` 或 `wss://host:port/ws` | TCP gameServer 的 `message` |
| 单进程游戏消息服务 | `gameServer --transport ws/wss --script FILE` | 同上 | 当前 gameServer 的 `message` |
| HTTP API | `apiServer --transport http/https --script FILE` | `http://host:port/path` 或 HTTPS | `http` 及请求钩子 |

### WS 网关连接 TCP 游戏 gameServer

在两个终端分别运行；这里网关直接连接 gameServer：

```bash
./build/debug/mistlane --role gameServer --listen 127.0.0.1:7102 --script examples/echo.lua
./build/debug/mistlane --role gateway --listen 0.0.0.0:8080 \
  --transport ws --upstream 127.0.0.1:7102
```

游戏客户端连接 `ws://127.0.0.1:8080/ws`。远程客户端将 `127.0.0.1` 换成服务器域名或 IP；`0.0.0.0` 是服务端绑定地址，不是客户端连接地址。`/` 和 `/ws` 都支持 WebSocket 升级。

```mermaid
flowchart LR
  Client[游戏客户端] -->|WS / WSS| Gateway[Drogon 游戏网关]
  Gateway -->|内部 TCP| Worker[gameServer · Asio / Lua]
  Worker --> Message[message payload]
  Api[HTTP API 客户端] -->|HTTP / HTTPS| Http[独立 apiServer]
  Http --> Handler[http request]
```

一条完整 WS 消息对应一个 payload；客户端不加内部 TCP 的四字节长度头。网关负责转换为内部 TCP 帧。`--upstream` 填 `IP:PORT`，连接的是 TCP 服务，不填 WS URL。同一客户端连接固定到选定的上游。

### WSS 游戏网关

将游戏网关改为以下命令，内部 gameServer 仍使用上面的 TCP 配置：

```bash
./build/debug/mistlane --role gateway --listen 0.0.0.0:7443 \
  --transport wss --cert /path/to/fullchain.pem --key /path/to/private-key.pem \
  --upstream 127.0.0.1:7102
```

客户端连接 `wss://game.example.com:7443/ws`，域名必须匹配证书且证书链受客户端信任。证书和私钥保存在源码仓库之外。WSS 最低 TLS 1.2；HTTPS 网页中的游戏客户端应连接 WSS。

### 单进程直接处理游戏 WS / WSS

开发或小型消息服务可以让 gameServer 自己监听 WS，无须额外网关或上游：

```bash
./build/debug/mistlane --role gameServer --listen 0.0.0.0:8081 \
  --transport ws --script examples/echo.lua
```

连接 `ws://127.0.0.1:8081/ws`。启用 WSS 时改为 `--transport wss`，并添加 `--cert FILE --key FILE`；Lua 仍实现 `message(payload) -> string 或 nil`。

### 游戏客户端示例与当前限制

在浏览器开发者工具中发送一条二进制游戏消息：

```js
const socket = new WebSocket('ws://127.0.0.1:8080/ws')
socket.binaryType = 'arraybuffer'
socket.onmessage = ({ data }) => console.log(new TextDecoder().decode(data))
socket.onopen = () => socket.send(new TextEncoder().encode('hello mistlane'))
```

原生客户端同样使用标准 WS/WSS 库。服务端接受 text/binary 消息，响应统一为 binary，单消息最大 1 MiB。应用自己定义 payload 编码和消息 ID，当前示例只原样回显。

会话是全双工的：客户端帧不依赖响应到达，可以流水发送多条请求；handler 返回 nil 时该请求没有响应帧。响应与服务端推送共用同一条有界通道——每个会话最多积压约 2 MiB 输出，越界时 Lua 收到明确的拒绝而非 handler 报错。游戏会话的主动推送、广播与按会话发送通过 `mistlane.session.send` 与 `mistlane.session.broadcast` 提供；帧率令牌桶与输出上限共同约束吞吐。推送与响应交错导致归属不明时，可用可选关联模式：首帧 `MLC2` 将会话（直连或经 gateway，TCP 或 WS）切换为逐帧携带 4 字节请求 ID，响应回显、推送置 0——详见[架构说明](./architecture.md#执行与连接模型)。当前适合验证游戏连接、消息链路与推送路径；玩家鉴权、账号绑定与按玩家路由仍缺失。

一个进程也可以同时服务多种协议：`--transport` 仍为单一选择、作用于主监听；每个 `--listener PROTO://IP:PORT` 追加一个独立监听（明文与 TLS 可并存；`wss`/`https` 条目可追加 `?cert=FILE&key=FILE` 绑定自己的证书）。WS/WSS 监听照旧承载游戏会话，同一个 gameServer 可以在不同端口同时暴露原生客户端的帧式 TCP 和浏览器用的 WS/WSS。HTTP 侧见[HTTP 多监听与 WebSocket 生命周期](./http.md#多监听与-websocket-生命周期)。

### 内部服务身份（MLS1）

玩家连接保持匿名，但内部链路可以要求服务身份，让开放在可达网络上的 gameServer 端口不被无关方悄悄使用。给 gameServer 配置 `--service-token TOKEN`：此后每条连接的首帧必须为 `MLS1 <role> <instance> <token>`，收到 `MLS1 OK` 后会话恢复正常。gateway 一侧成对配置 `--upstream-token TOKEN`（同一秘钥）与 `--service-name NAME`（供对端日志识别的实例名，省略时用监听地址）——握手只发生在两个进程之间，客户端全程不可见。

失败是刻意且显式的：缺秘钥或秘钥错误回复 `MLS1 ERR TOKEN`，格式错误回复 `MLS1 ERR INVALID`，未配置令牌的普通服务端对携带令牌的 gateway 回复 `MLS1 ERR UNEXPECTED`。每次拒绝都关闭连接；自身握手被拒的 gateway 关闭客户端会话且不透传原因。两令牌参数作用于 cross 路由时直接拒绝启动（cross 以 `MLC1` 加入为身份）；已通过 `MLS1` 的会话仍可在下一帧切换 `MLC2` 关联模式。这是用于暴露配置错误的明文共享秘钥比对，不是 mTLS——传输层认证请使用 TLS 监听。

### 初始化模板中的配置

`mistlane init` 会生成可选的 `gateway-ws.env` 和 `gateway-wss.env`，默认集群仍启动 TCP 网关。WS 示例监听 `127.0.0.1:8080`；远程接入时修改对应文件中的 `LISTEN`。`gameServer-mixed.env` 演示同一个 gameServer 通过 `LISTENERS` 在 7102 提供帧式 TCP、在 7082 提供 WS 监听。

```bash
MISTLANE_BIN=/path/to/mistlane ./ops/cluster.sh start gameServer gateway-ws
```

WSS 在 `config/instances/gateway-wss.env` 中设置 `CERT` 和 `KEY`，可填绝对路径或相对项目根目录的路径，然后启动 `gateway-wss`。脚本会检查证书文件并传给 `--cert`/`--key`；不会生成或提交私钥。systemd 对应实例为 `mistlane@gateway-wss.service`。

### 实例配置字段

`config/instances/<name>.env` 由 `ops/run-instance.sh` 以 shell 赋值方式 source，属于项目所有者手写的受信部署配置，不是不受信输入。启动器只消费下列字段；文件中的其他变量一律忽略。

| 字段 | 必填 | 启动器行为 |
|---|---|---|
| `ROLE` | 必填 | 作为 `--role` 传入；`cluster.sh` 还用它匹配就绪行（`ready <ROLE> <LISTEN>`），匹配成功才报告实例已启动。 |
| `LISTEN` | 必填 | `IP:PORT` 作为 `--listen` 传入。 |
| `TRANSPORT` | 可选 | `tcp` 为默认值，不出现在命令行；`ws`、`wss`、`http`、`https` 原样传入。`wss`/`https` 在 `CERT` 和 `KEY` 不可读时拒绝启动。 |
| `SCRIPT` | 可选 | Lua 入口作为 `--script` 传入；相对路径按项目根目录解析。gateway 留空。 |
| `UPSTREAMS` | 可选 | 逗号分隔的 `IP:PORT`（或 `IP:PORT:PROBE`，使 cross 按对端内部就绪探测挡住新加入）列表；每个条目展开为一个 `--upstream` 参数。 |
| `ROUTE_EPOCH` | 可选 | gateway cross routing/crossServer 纪元作为 `--route-epoch` 传入；留空则省略该参数。 |
| `LISTENERS` | 可选 | 逗号分隔的 `PROTO://IP:PORT` 条目；每个条目展开为一个 `--listener` 参数，一个实例即可同时暴露 TCP、WS、WSS 与 HTTP。`wss`/`https` 条目可追加 `?cert=FILE&key=FILE`；query 路径原样传递，不按项目根目录解析。 |
| `CERT`、`KEY` | 仅 wss/https | 证书链与私钥，绝对路径或项目根目录相对路径；启动器先确认两者可读。 |

实例文件随模板仓库一起提交时不要放任何机密：初始化器不生成证书材料，`gateway-wss.env` 只带 `certs/` 占位路径。私钥等文件应放在仓库之外或由部署方管理的 `certs/` 目录；其他凭据通过安装后的 systemd unit（`Environment=`、`LoadCredential=`）或部署工具注入，而不是写进 `config/instances/` 提交。

### 原生配置文件

二进制也能不经 shell 启动器直接读原生配置格式：

```bash
cat > gameServer.native.env <<'EOF'
# mistlane-config 1
ROLE=gameServer
LISTEN=127.0.0.1:7102
SCRIPT=examples/echo.lua
EOF
./build/debug/mistlane --config gameServer.native.env --max-connections 512
```

- 首个非空行必须是版本标记 `# mistlane-config 1`；缺失即拒绝，避免把任意 shell env 文件（包括缺少该标记的 `config/instances/*.env` 模板）悄悄当作配置。
- 其余行为 `KEY=VALUE`。空行与 `#` 注释忽略，空值跳过，重复键报错，未知键以「文件:行号」拒绝。
- 键为 CLI 选项的大写形式（`ROLE`、`LISTEN`、`TRANSPORT`、`SCRIPT`、逗号分隔的 `UPSTREAMS`、`ROUTE_EPOCH`、`SERVICE_TOKEN`、`UPSTREAM_TOKEN`、`SERVICE_NAME`、逗号分隔的 `LISTENERS`、`CERT`、`KEY`、`REDIS`、`METRICS_LISTEN`、`PROBE_LISTEN`、`OVERLOAD_RATIO`、`READY_HYSTERESIS`、`MAX_CONNECTIONS`、`MEMORY_BUDGET_MIB`、`MAX_ROOM_SESSIONS`、`MAX_GROUP_SESSIONS`、`MAX_SHARD_SESSIONS`、`CRASHPAD_HANDLER`、`CRASHPAD_DATABASE`），另有 `ROOT`。
- 同一次运行的优先级：文件值 < 非空的 `MISTLANE_<KEY>` 环境变量 < `--config` 之后给出的显式参数。
- `SCRIPT`、`CERT`、`KEY`、`CRASHPAD_HANDLER`、`CRASHPAD_DATABASE` 的相对路径按配置文件所在目录解析；设置 `ROOT` 时改按 `ROOT`（其本身相对配置文件所在目录）。这与 shell 启动器按项目根目录解析不同。要让生成的 `config/instances/*.env` 模板直接走 `--config`，需先在文件首行补版本标记并加 `ROOT=../..`，其 `SCRIPT`/`CERT` 路径才会像启动器一样解析。
- 机密仍应放在部署方管理的环境（`MISTLANE_*` 或 systemd `LoadCredential=`），不提交到文件。


## apiServer

除了基于帧 TCP/WS 的 Lua `message` 模式，gameServer 还可以直接提供 HTTP 服务。用 `--transport http`（或 `https`）配合 HTTP 脚本（如 `examples/http.lua`）启动：

```bash
./build/debug/mistlane --role gameServer --listen 127.0.0.1:7500 \
  --transport http --script examples/http.lua
curl -s http://127.0.0.1:7500/healthz
```

HTTPS 需另加 `--cert` 和 `--key`。传给 Lua 的请求表包含 method、path、query、headers 和 body；响应表设置 status、headers 和 body。请求生命周期钩子为 `before_request`、`http`、`after_request`、`on_error`，以及通用的 `init`/`start`/`tick`/`stop`/`shutdown` 回调。apiServer 必须提供 `http`，可省略 `message`；帧 gameServer 必须提供 `message`。限制与钩子契约见 [HTTP 参考](./http.md) 和 [Lua 生命周期](./lua.md)。

## Lua 服务

脚本返回 table，必须提供 message(payload) -> string 或 nil，可选 init()、tick() 和 shutdown()。tick 间隔 100ms。所有回调在节点的单个事件循环线程上执行，应快速返回。

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
pnpm docs:check
pnpm docs:preview
```

`docs:check` 用无头浏览器渲染构建产物，断言每个 Mermaid 图都渲染成功、标签没有被裁切，并在桌面/移动宽度、亮色/暗色主题下检查两种语言。它需要一个 Playwright chromium 二进制（`npx playwright install chromium`，或设 `PLAYWRIGHT_CHROMIUM_EXECUTABLE=/path/to/chromium` 复用现有浏览器）；CI 在 `docs:build` 之后运行它。

文档提供中英文导航、本地搜索和成对页面。默认 base 为 /mistlane/；部署到域名根目录时可设置 DOCS_BASE=/。构建会检查内部链接并生成 docs/.vitepress/dist，以上命令不执行托管发布。

## 导出源码

```bash
python3 tools/export.py
```

归档仅包含 release-files.txt 显式清单中的文件和 SHA256 校验值，不包含父仓库历史、私有配置、原游戏玩法和预编译二进制。

## Crashpad handler 部署

`cmake --install <build-dir> --prefix PREFIX` 会把 `mistlane` 和 Crashpad 的 `crashpad_handler` 并排安装到 `PREFIX/bin`；handler 在 vcpkg 工具目录存在时一并复制。两个二进制只需要系统运行库（glibc、`libstdc++`、`libgcc_s`、`libm`），第三方依赖全部静态链接。

可选 dump 管线的部署约定：

- handler 与服务端放在同一文件系统并显式传入：`--crashpad-handler PREFIX/bin/crashpad_handler --crashpad-database /var/lib/mistlane/crashes`。数据库指向按保留策略定容的本地目录；上传保持关闭，数据不出机器。
- Linux 崩溃抓取依赖 `ptrace`（handler 需要检查崩溃进程）。容器运行时必须允许它：Docker 默认 seccomp profile 在受支持的内核上放行 `ptrace`，但加固、无特权或自定义 AppArmor/seccomp profile 可能拒绝。`ptrace` 被拒时节点照常启动与服务，只是不会写 minidump——在该环境依赖 dump 前先用一次受控崩溃验证整条链路。
- 仓库自带演练二进制：`mistlane-crash-sample`（与 `mistlane` 一起构建安装）用 `--crashpad-handler FILE --crashpad-database DIR` 接好同一套本地管线后主动解引用空指针。原生 `crashpad.minidump` 用例断言数据库落出带 `MDMP` 魔数的真实 minidump、样例进程因 `SIGSEGV` 退出。

### 符号归档与栈回溯

每个构建都内嵌 ELF `NT_GNU_BUILD_ID` note，minidump 的每个模块记录也带同一 ID，因此 dump 无需符号服务器即可指认自己的二进制。对你实际部署的那个二进制归档**未剥离**副本（`linux-debug` 与 `linux-release` 两个 preset 同样适用；剥离会删掉栈回溯所需的符号）：

```bash
id=$(python3 tools/crash_symbols.py build-id build/debug/mistlane)
cp build/debug/mistlane /var/lib/mistlane/symbols/mistlane-$id
# 之后对崩溃数据库下的任意 dump：
python3 tools/crash_symbols.py match /var/lib/mistlane/crashes/pending/xxx.dmp /var/lib/mistlane/symbols/mistlane-$id
python3 tools/crash_symbols.py modules /var/lib/mistlane/crashes/pending/xxx.dmp
```

`match` 在 dump 属于其他构建时返回非零——不要用错误的符号去走栈。完整栈回溯需从 Crashpad 上游获取 `minidump_stackwalk`（vcpkg crashpad port 只带 `crashpad_handler`），指向归档目录运行；栈帧按模块 build id 逐个解析。
