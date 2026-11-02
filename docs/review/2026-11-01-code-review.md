# 代码深审报告（2026-11-01 执行）

范围：全仓源码 `src/`、`include/mistlane/`、`lua/`、`tools/`、`examples/`（排除 `build/`、`third_party/`、`docs/.vitepress/cache/`、测试夹具源文件）。两道独立扫描并行执行：

1. **安全静态扫描**（`src/ include/ lua/ ports/ tools/ scripts/ examples/`，94 个源文件约 25k 行）：硬编码密钥、SQL/shell/Lua 注入、eval、反序列化、路径穿越、网络字节作下标/长度、TLS/认证薄弱点。
2. **隐藏 bug 猎捕**（`src/*.cpp` + `include/mistlane/*.hpp` 实现代码，逐条读码验证）：错误处理、竞态、越界、资源泄漏、off-by-one、并发、生命周期。

## 发现统计

| 严重度 | 安全扫描 | bug 猎捕 | 去重后 |
|---|---|---|---|
| P0 | 0 | 0 | 0 |
| P1 | 0 | 1 | 1 |
| P2 | 0 | 4 | 4 |
| P3 | 3 | 3 | 5 |
| **合计** | **3** | **8** | **10** |

去重：`entry_ticket.hpp:180`（非恒定时间 HMAC 比较）在两道扫描均命中，计一条（按较高严重度 P2）。

修复：**P1 × 1 已当场修复**（含回归测试）。P2 × 4、P3 × 5 已登记 todo。

## P0 / P1 清单

无 P0。P1 一条，已修：

- **[P1·越界/数组] `include/mistlane/entity_property.hpp:287`** — `PropertyStore::descend()` 对中间路径段用未检查的 `std::vector::operator[]`（`[step.index]`），`array_at_value`（:283）对任何路径段均不检查；`set_path`（:136）只通过 `assign_child`（:137/:290）检查最后一段。陈旧容器代理（Lua 侧经 `entity_lua.cpp` 的 `index_array`/`newindex_proxy` 保存的 `ref.path`）在数组收缩（`:remove`）后写入越界读（UB）。
  **修复**：`descend` 改为与只读 `get_path`（:92-110）相同的边界检查，越界/类型不符返回 `nullptr`；`set_path`/`array_at_value`/`insert_at`/`remove_at` 判空后拒绝写入并设置 `error_`。`set_path` 中间段失败返回 `false`，`insert_at`/`remove_at` 判空 `array` 返回 `false`。
  **回归测试**：`src/test_cases.cpp` entity.tree bounds 段新增「收缩后陈旧索引写入被拒」用例（`remove_at` 后 `set_path`/`insert_at` 越界索引拒绝）。`mistlane_entity_codec`/`entity.tree` 全绿；全量 `ctest --preset linux-debug` 73/73 通过。

## P2 清单（已登记 todo，未修）

1. **[P2·生命周期] `src/node.cpp:461`** — `session_send` 把栈局部 `reason` 以引用 `[&reason, fail]` 捕获进 `Channel::write` 异步完成回调；`message_size`/`operation_aborted` 的写失败异步完成后向已销毁栈帧写入（use-after-scope）。注释声称"同步拒绝"不变式只对同步返回路径成立，异步失败路径不成立。
2. **[P2·逻辑/安全] `include/mistlane/entry_ticket.hpp:180`** — `TicketVerifier::verify` 用 `std::array::operator!=`（非常量时间）比较 HMAC，与 `src/api.cpp:95` 的 `CRYPTO_memcmp` 实践不一致，留下时序侧信道（网络可达的 MLT1 门票验证路径）。
3. **[P2·资源泄漏] `src/lua_service.cpp:807`** — `stream_publish` 在 `callback_ref` 之后才做字段校验，校验抛 C++ 异常时该 Lua registry 引用从未 `unref`，泄漏 registry 槽。
4. **[P2·资源泄漏] `include/mistlane/entity_runtime.hpp:48,58`** — `delivered_` 集合只 `emplace` 从不 `erase`（`destroy()` :51 只擦 `entities_`）；长驻 entity-hosting 节点每个已交付非 notify 调用永久留存去重条目，内存无上界增长。

## P3 清单（已登记 todo，未修）

1. **[P3·越界/逻辑] `include/mistlane/entity_envelope.hpp:123`** — `encode_entity_reply` 以 16 位宽度写 `reply.reason.size()`（`envelope_push_be` 宽 2），>64KB 的 reason 静默截断、回包损坏（低风险：reason 通常为短错误串）。
2. **[P3·逻辑/竞态] `src/web.cpp:57`** — `deliver` 置 `credit_=true` 后若 reader 重新 arm 期间又到一条消息，可能覆盖已暂存的 `incoming_`，单槽桥接下罕见消息丢失。
3. **[P3·逻辑/竞态] `include/mistlane/cross.hpp:92`** — `cross_shard` 做 `hash % count`（count=upstreams.size()），零 upstream 会除零；当前被 `main.cpp:334` 守卫（gateway 必须有 upstream），不可达，属防御缺失。
4. **[P3·TLS/认证] `src/node.cpp:1039`** — MLS1 服务令牌用 `std::string::operator!=`（非常量时间）比较，与 `api.cpp:95` 的 `CRYPTO_memcmp` 不一致。
5. **[P3·TLS/认证] `src/node.cpp:2096`** — registry 身份校验同样用非恒定时间令牌比较。

## 质量门

- **编译**：`cmake --build build/debug --target mistlane` 通过（仅 `-Wdangling-pointer` 测试夹具既有告警，非本次引入）。
- **测试**：全量 `ctest --preset linux-debug` **73/73 通过**（含 P1 修复后新增回归用例）。
- **文档**：`pnpm docs:build` 通过；`pnpm docs:check` 36 张 Mermaid 图渲染通过。
- **扫描基线对比**：与仓库既有审核记录（todo.md「审核发现」段、docs/review/luna-review-*.md）无冲突；本次发现的非恒定时间比较与既有 `CRYPTO_memcmp` 实践不一致属新面（此前审核未覆盖 MLS1/MLT1 比较路径）。

## 逐项确认非缺陷（重点复核）

- SQL 全参数化（`journal.cpp` `sqlite3_prepare_v2`+bind；MySQL `with_params`）；Lua 环境刻意排除 `debug/io/os/package` 且脚本仅从 `--script` 路径加载；TLS 全链路 `verify_peer`+hostname（mysql_pool/Redis/libcurl/Drogon，无 `SSL_VERIFY_NONE`）；所有网络长度读取有 `max_frame=1MiB`/标识符/边界校验；nonce 用 CSPRNG `RAND_bytes`。
- `entity_contract.hpp` 解析器全程 `at_<size()` 守卫；`entity_codec.hpp` sproto 长度检查均在读取前；`journal.cpp`/`api.cpp`/`api_store.cpp` 事务+常量时间比较；`entity_gateway.hpp`/`entity_runtime.hpp` RpcQueue 定容量；`migration.hpp` 守卫齐全；`bot.cpp` 多线程但锁内读写。
- 测试夹具 `"s3cret"` 令牌（test_cases 30+ 处、generate-config-gifs.py 8 处）与 `mysql_probe.cpp` 注入演示均为刻意为之，不计严重度。
