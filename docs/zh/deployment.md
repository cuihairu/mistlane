# 部署工作流

Mistlane 以普通进程运行（`mistlane init` 生成的集群脚本与 systemd 模板见[使用指南](./guide.md)）。本页覆盖容器部署工作流：包装预编译二进制的运行时镜像、compose 示例、健康检查语义，以及验证这一切的钻取脚本。

## 交付物

- `deploy/Dockerfile` —— 运行时镜像。一个容器运行一个节点实例。
- `deploy/compose.yaml` —— 示例双服务集群（`gameServer` 加其前的会话路由 `gateway`），并挂载一份配置覆盖。
- `deploy/compose/instances/gateway.env` —— gateway 的容器覆盖（上游按服务名，替代脚手架的环回默认值）。
- `deploy/compose.stress.yaml` + `deploy/compose/instances/*stress*.env` —— 供压力钻取使用的跨服基准拓扑（两个 crossServer 与一个 cross 网关）。
- `deploy/healthcheck.sh` —— 镜像存活探针。
- `tools/container_smoke.sh` —— 部署验证钻取。
- `tools/container_stress.sh` —— 容器/裸机成对压力钻取。
- 主机名端点地址（下文）—— 容器面所需的唯一框架改动。

## 构建镜像

镜像包装**预编译二进制**；源码构建是 CI 的职责（`.github/workflows/ci.yml` 在 push 时构建并测试）。先把二进制放入构建上下文，再构建：

```sh
mkdir -p deploy/bin
cp build/debug/mistlane deploy/bin/mistlane     # 或发布产物
docker build -t mistlane:dev -f deploy/Dockerfile deploy/
```

基镜像的 glibc 必须不早于构建二进制所用的工具链——二进制链接其构建宿主的符号版本，更旧的基底会以 `version GLIBC_x.y not found` 中止。`BASE_IMAGE` 是构建参数，默认 `ubuntu:26.04`（项目构建工具链，glibc 2.43）；产物来自更旧 runner 时传 `--build-arg BASE_IMAGE=ubuntu:24.04`。

镜像在构建期运行 `mistlane init`，故 `ops/`、`config/instances/` 与 `scripts/` 随镜像携带，任一角色一条命令即可运行。仅环回的 `LISTEN` 默认值改写为 `0.0.0.0`，因为容器必须绑定 Docker 发布的接口，而非本地集群所用的环回。

## 单容器运行

```sh
docker run --rm -p 7102:7102 mistlane:dev gameServer
```

实例名即命令；入口为 `ops/run-instance.sh`，它以 `exec` 启动节点，故节点即 PID 1。`docker stop` 于是把 SIGTERM 直接送达节点，节点既有优雅停机路径以 exit 0 应答——与 `ops/cluster.sh` 和测试套件依赖的路径一致。

覆盖配置或 Lua 入口：绑定挂载覆盖镜像内路径：

```sh
docker run --rm -p 7102:7102 \
  -v /srv/mistlane/gameServer.env:/opt/mistlane/config/instances/gameServer.env:ro \
  -v /srv/mistlane/scripts:/opt/mistlane/scripts:ro \
  mistlane:dev gameServer
```

实例文件是项目所有者手写的受信 shell 赋值；机密仍应放在部署方管理的环境或 `LoadCredential=`（见[使用指南](./guide.md)），不要提交。

## compose 集群

```sh
docker compose -f deploy/compose.yaml up -d
```

示例先启动 `gameServer`，`gateway` 仅在健康检查变绿后启动（`depends_on: condition: service_healthy`）。gateway 在 compose 网络上**按服务名**到达 `gameServer`；其覆盖文件是唯一挂载，其余一切保持镜像默认。

## 端点地址接受主机名

与镜像同时落地的框架改动：服务端端点地址现在接受主机名，而非仅数字字面量。客户端早已解析主机名；`parse_address`（`src/node_internal.hpp`）现在在文本非地址字面量时回退 `getaddrinfo`，优先取首个 IPv4 答案、仅在无 v4 时回退 IPv6（显式带括号的 `[::1]` 字面量仍优先）。这使 `--upstream gameServer:7102`、`--listen localhost:7102` 与 FQDN 在监听、上游、coordinator、data、probe 与 dashboard 端点上同样可用。

## 健康检查语义

镜像健康检查应答的是**存活**而非就绪：它从 PID 1 的 cmdline 读节点自身的 `--listen` 并打开 TCP 连接。能接受连接意味着进程已起且已绑定。节点的就绪信号是其 `ready <role> <listen>` 标准输出行——与 `ops/cluster.sh` 等待的标记相同——内部探针监听（`PROBE1`）是编排器需要时的协议级应答，而非此健康检查。钻取同时断言健康检查与就绪行，故两者皆未过度宣称。

## 验证钻取

`tools/container_smoke.sh` 以本地二进制构建镜像，然后检查三条路径并全部清理：

1. 默认实例配置就绪，SIGTERM 后干净停止（exit 0）；
2. 绑定挂载的实例文件覆盖被遵循（节点在覆盖端口就绪，证明挂载与健康检查一致）；
3. compose 集群两服务均达就绪，gateway 上游经服务名 DNS，两者均干净停止。

`docker` 或 `docker compose` 不可用时输出 SKIP 并跳过，以便在无容器运行时的环境执行。2026-11-21 验证：三条路径全过，全量测试套件绿（81/81）。

## 压力钻取

`tools/container_stress.sh` 在目标容器环境里真实驱动文档化的跨服基准拓扑：leg A 先在默认每会话防洪水参数下探测（文档化基准负载会耗尽令牌桶，`mistlane_rate_limit_stops_total` 必须触发），再以调高的防洪水参数作为裸机参考腿跑同一负载；leg B 运行 `deploy/compose.stress.yaml`（两个 crossServer 加一个 cross 网关，上游按 compose 服务名，实例文件调高防洪水参数）。每条腿经发布的网关端口以文档化的 `--join "MLC1 1 realm g bench"` 握手驱动随仓 bot，覆盖三档负载（32 × 500 文档基准、128 × 781 近生成器上限、1 MiB `--expect-echo` 往返）。最后一条腿并发驱动两个不同房间名（`room-a` / `room-b`）的 bot 并比较各 shard 的 rx 增量：同一 group/room 始终固定在一个 crossServer，因此单房间负载让另一节点空闲，而两个不同房间名应分散到上游列表。随后从各节点发布的 metrics 端口抓取 `mistlane_request_seconds`、`mistlane_messages_total`、`mistlane_event_loop_lag_max_seconds`、`mistlane_active_sessions`、`mistlane_admission_rejections_total`、`mistlane_process_resident_memory_bytes` 与 `mistlane_rate_limit_stops_total`。两条腿都以 SIGTERM 收尾并断言 exit 0。可比性参数（脚本、epoch、生成器参数、拓扑）在钻取内固定；数字的解读见[跨服与过载保护](./cross-server.md)与[性能剖析](./profiling.md)。

**2026-11-27 验证**（主机 = Ubuntu 26.04.1 LTS，Docker 29.1.3，14 核，共租负载 5–8）：吞吐、分位数、滞后与 RSS 的实测见[验证记录](./validation.md)。这是部署环境钻取，不是版本发布，也不是编排决策。

## 尚未覆盖

- 镜像内不做源码构建：CI 产出二进制，镜像只打包。容器内源码构建可行，但非镜像交付内容。
- 无容器编排决策：lease/fencing、房间 drain/migration 与编排选择待真实压测后决定（见[迁移状态](./migration-status.md)）。
- Crashpad 采集未接入容器默认：脚手架实例文件尚无 crashpad 字段，容器化转储采集需先扩展脚手架。宿主节点上的 Crashpad 生成与保留已验证（[验证记录](./validation.md)）。
- 无版本发布：镜像本地构建并验证；发布版本化镜像是发布决策（见仓库根的 `RELEASE_NOTES.md`）。
