简体中文 | English
# 审核整改记录（回应 luna-review-2026-10-18）

处理原则：表一按风险修缺陷（先补测试）；表二以代码为准改文档，不为对齐文档改业务代码；表三只做评估与分期建议，不实现（需用户拍板）。每项先核对审核结论再动手。

## 表一 缺陷整改

| 缺陷 | 状态 | 证据 |
|---|---|---|
| 高：探针异步写缓冲生命周期 | 已修 | `src/node.cpp` `Peer::reply` 改用 `shared_ptr` 行缓冲保活至写入完成（reply 内 `asio::buffer(*line)`，line 由 handler 捕获）；`Session::probe` 写回调补 `request` 捕获，外层帧释放后缓冲仍有效。测试：`probe.control` 新增连续 50 次探测生命周期用例。 |
| 高：探针连接对象累积 | 已修 | `Peer::finish()` 统一收尾（finished_ 防重入），经 `ProbeServer::retire` 从 `peers_` 移除；读错误、应答完成、空闲超时三条路径都走 finish。测试：64 个挂起探测连接外的第 65 个被立即关闭，释放后探测恢复。 |
| 中高：探针输入与并发无上限 | 已修 | 请求行上限 256 字节（`asio::streambuf buffer_{probe_line_cap}`，超限 `async_read_until` 报错关闭）、并发上限 64（`max_peers`，超限 accept 即关）。测试：300 字节行被无应答关闭；第 65 连接被拒。文档：双语 cross-server「内部探测协议」补上限说明。 |
| 中：`mistlane_ready` 与准入状态脱节 | 已修 | `Node::tick` 在迟滞翻转后同步 `diagnostics_->set_ready(ready_ && admitting_)`。测试：饱和期间抓取 gauge=0、恢复后=1，与探测应答一致。 |
| 中：准入拒绝漏计跨服拒绝 | 已修 | `Session::reject` 内调用 `diagnostics_.reject(reason)`，`ROOM_FULL`/`NOT_READY`/`EPOCH`/`UPSTREAM` 等统一计数。测试：cross 门控拒绝后 `mistlane_admission_rejections_total{role="cross"}` ≥1。 |
| 中：方括号 IPv6 上游启动失败 | 已修 | 新增 `parse_address`（去除 `[` `]` 后交给 `make_address`），接入探针监听、上游会话、cross 加入、构造校验与监听绑定五处。测试：shard `--listen [::1]` + `--probe-listen [::1]`，cross 以 `[::1]:PORT:PROBE` 门控加入与 `[::1]:PORT` 不门控加入均 `MLC1 OK 1`。 |
| 低：MariaDB 集成测试未通过（审核环境） | 已复核 | 本环境存在 `mariadbd`/`mariadb-install-db`；`ctest -R mistlane_mysql_integration` 通过（6.3s）。审核环境失败未复现，按审核建议不将其记为产品缺陷，也不把审核轮结果记为通过。 |

回归证据：完整 ctest 14/14 通过（含扩展后的 `mistlane_probe_control`）。

## 表二 文档与实现不一致整改（以代码为准改文档）

| 文档说法 | 处理 | 证据 |
|---|---|---|
| 核心二进制不解析原生配置格式 | 文档已更新 | `docs/architecture-comparison.md` 与中文对应页「配置」行改为：启动器转 CLI 参数，运行时也可经 `--config` 解析原生 `KEY=VALUE` 文件（实现见 `src/main.cpp`，随 030cca6 交付）。 |
| cross 压测命令缺 `--join` 且 64 客户端超容量 | 文档已更新并实测 | 双语 cross-server 压测手册改为 `--join "MLC1 1 realm g bench" --clients 32 --count 500`，并说明 `--join` 语义与 bot 连接同样占用准入配额（默认封顶 42，启动日志打印）。实测：16000/16000 请求成功、0 失败、3370 req/s、p99 40ms、`/metrics` 计数对齐、零准入拒绝。 |
| “sustained load generator is not yet integrated” 过时 | 文档已更新 | 双语「容量计划」段改为：内置 `mistlane bot` 可驱动功能压测；持续生产级压测与 `/metrics` 之外的运营指标管线仍待补。 |
| 生成 systemd 文案称 user-service 模板 | 文案已更正 | `src/scaffold.cpp` 生成 README 改为 system-service 模板（与 unit 的 `User=`、`WantedBy=multi-user.target` 一致，安装目标 `/etc/systemd/system/`）；此为代码内嵌文档文本对齐实际行为，非业务逻辑改动。scaffold 测试通过。 |

## 表三 能力评估与分期建议（不实现，需用户拍板）

| 能力 | 评估 | 分期建议 | 现有承接 |
|---|---|---|---|
| 跨服连接认证与探针 TLS/mTLS | 价值高；当前前提是部署在可信内网，监听面扩大前必须补 | P2：随「多协议监听」的证书配置落地后单独立项，先定证书分发与轮换约定 | todo.md「多协议监听」（TLS 配置基础） |
| 房间 drain、lease/fencing 与安全迁移 | 价值高；是拓扑变更与故障恢复的前置协议 | P3：先出协议草案（租约授予/续期/ fencing 令牌、drain 完成判定），评审后再实现；依赖「按目标职责迁移运行角色」的 crossServer 设计 | todo.md 角色迁移子项（移交/结算/重试/离线/回服恢复） |
| Lua 内存配额与 CPU 隔离 | 价值高、成本中：Lua 5.5 可挂自定义分配器做步进配额，但 OOM 时序、回收边界与误伤需设计 | P2 单独立项；先在样例脚本上验证配额行为再接入节点 | 无（账本未记录，建议新增） |
| Crashpad 转储保留上限与自动清理 | 价值中高、成本低：目录配额 + 按数量/时间清理即可 | P1，可与下一批诊断工作同批 | 无（账本未记录，建议新增） |
| 服务端推送、广播与全双工队列 | 实时玩法必需；涉及发送队列上限、慢消费者背压、断连清理 | 已排期，按既有条目推进，不另立项 | todo.md「多协议监听」子项已覆盖 |
| 目标环境可重复压测与 profiler 影响记录 | 价值高；本沙箱无法采集（perf_event_paranoid=4 且无 CAP_PERFMON、ptrace 受限） | 受环境阻塞，需目标 Linux/容器环境 | todo.md 目标环境演练与压测条目（blocked） |
| 版本、构建与节点身份指标 | 价值中、成本低（gauges 带 build/version/node id，低基数） | P1，可与 Crashpad 保留策略同批 | todo.md metrics 条目已注明待补 |

## 测试与验证汇总

- `ctest` 14/14 通过（新增探针回归：生命周期、超长行、并发上限、指标一致性、IPv6）。
- `pnpm docs:build` 通过（文档站双语构建）。
- 压测手册命令实测通过：shard+cross 集群，32 客户端 × 500 请求，16000/16000 成功。
