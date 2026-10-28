简体中文 | English
[0m
[0m→ [0mSkill "code-review" 
我会只读检查，不改文件、不提交。先盘点仓库结构与文档入口，再按模块核对实现；只把能用具体行号支持的发现列为结论，不确定项标「未核实」。
[0m$ [0mgit status --short && git log -1 --oneline
030cca6 feat: parse native key value config files
[0m$ [0mrg --files -g '!*node_modules*' -g '!*.lock' | head -250
README.zh-CN.md
CMakeLists.txt
LICENSE
examples/echo.lua
examples/cross.lua
examples/http_probe.cpp
examples/http.lua
examples/mysql_probe.cpp
examples/crash_sample.cpp
examples/test_echo.lua
release-files.txt
todo.md
CMakePresets.json
docs/validation.md
THIRD_PARTY.md
package.json
README.md
pnpm-workspace.yaml
vcpkg-configuration.json
docs/public/logo.svg
docs/dependencies.md
docs/lua.md
docs/cross-server.md
docs/architecture-comparison.md
docs/http.md
docs/guide.md
docs/migration-status.md
docs/zh/validation.md
docs/zh/dependencies.md
docs/zh/lua.md
docs/zh/cross-server.md
docs/zh/architecture-comparison.md
docs/zh/http.md
docs/zh/guide.md
docs/zh/migration-status.md
docs/zh/architecture.md
docs/zh/index.md
docs/architecture.md
docs/index.md
pnpm-lock.yaml
vcpkg.json
src/cli_client.hpp
src/web.cpp
NOTICE
src/test_fixtures.hpp
src/scaffold.cpp
src/main.cpp
lua/mistlane/min_heap.lua
tools/client.py
tools/crash_symbols.py
tools/export.py
src/test_support.hpp
src/lua_tests.cpp
src/node.cpp
src/bot.cpp
src/selftest.cpp
src/diagnostics.cpp
src/lua_tests.hpp
src/cli_client.cpp
src/http_client.cpp
src/test_cases.cpp
src/test_databases.cpp
src/lua_service.cpp
src/test_resources.hpp.in
ports/LICENSE.txt
ports/drogon/0005-jsoncpp-target.patch
ports/drogon/0001-vcpkg.patch
ports/drogon/vcpkg.json
ports/drogon/portfile.cmake
ports/drogon/0004-drogon-ctl.patch
ports/drogon/usage
ports/drogon/0003-deps-redis.patch
include/mistlane/mysql_pool.hpp
ports/drogon/0002-drogon-config.patch
include/mistlane/lua_service.hpp
include/mistlane/transport.hpp
include/mistlane/token_bucket.hpp
include/mistlane/scaffold.hpp
include/mistlane/cli.hpp
include/mistlane/http_client.hpp
include/mistlane/cross.hpp
include/mistlane/node.hpp
include/mistlane/http_service.hpp
include/mistlane/diagnostics.hpp
tests/redis_integration.py
tests/crashpad_startup.py
tests/mysql_integration.py
tests/integration.py
tests/scaffold.py
ports/trantor/000-fix-deps.patch
ports/trantor/002-fix-client-hostname-validation.patch
ports/trantor/vcpkg.json
ports/trantor/portfile.cmake
ports/trantor/001-disable-werror.patch
[0m$ [0mfind . -maxdepth 3 -type f \( -iname 'readme*' -o -name 'AGENTS.md' -o -name 'CLAUDE.md' -o -name 'TODO*' -o -name 'package.json' -o -name 'pyproject.toml' -o -name 'Cargo.toml' -o -name 'go.mod' \) -print
./README.zh-CN.md
./package.json
./README.md
./todo.md
[0m→ [0mRead todo.md 
[0m→ [0mRead README.md 
[0m→ [0mRead README.zh-CN.md 
[0m$ [0mrg --files src include tests tools lua examples docs | sort
docs/architecture-comparison.md
docs/architecture.md
docs/cross-server.md
docs/dependencies.md
docs/guide.md
docs/http.md
docs/index.md
docs/lua.md
docs/migration-status.md
docs/public/logo.svg
docs/validation.md
docs/zh/architecture-comparison.md
docs/zh/architecture.md
docs/zh/cross-server.md
docs/zh/dependencies.md
docs/zh/guide.md
docs/zh/http.md
docs/zh/index.md
docs/zh/lua.md
docs/zh/migration-status.md
docs/zh/validation.md
examples/crash_sample.cpp
examples/cross.lua
examples/echo.lua
examples/http.lua
examples/http_probe.cpp
examples/mysql_probe.cpp
examples/test_echo.lua
include/mistlane/cli.hpp
include/mistlane/cross.hpp
include/mistlane/diagnostics.hpp
include/mistlane/http_client.hpp
include/mistlane/http_service.hpp
include/mistlane/lua_service.hpp
include/mistlane/mysql_pool.hpp
include/mistlane/node.hpp
include/mistlane/scaffold.hpp
include/mistlane/token_bucket.hpp
include/mistlane/transport.hpp
lua/mistlane/min_heap.lua
src/bot.cpp
src/cli_client.cpp
src/cli_client.hpp
src/diagnostics.cpp
src/http_client.cpp
src/lua_service.cpp
src/lua_tests.cpp
src/lua_tests.hpp
src/main.cpp
src/node.cpp
src/scaffold.cpp
src/selftest.cpp
src/test_cases.cpp
src/test_databases.cpp
src/test_fixtures.hpp
src/test_resources.hpp.in
src/test_support.hpp
src/web.cpp
tests/crashpad_startup.py
tests/integration.py
tests/mysql_integration.py
tests/redis_integration.py
tests/scaffold.py
tools/client.py
tools/crash_symbols.py
tools/export.py
[0m$ [0mcat package.json && cat CMakeLists.txt
{
  "name": "@cuihairu/mistlane-docs",
  "version": "0.1.0",
  "private": true,
  "license": "Apache-2.0",
  "engines": {
    "node": ">=22.12.0"
  },
  "scripts": {
    "docs:dev": "vitepress dev docs",
    "docs:build": "vitepress build docs",
    "docs:preview": "vitepress preview docs"
  },
  "devDependencies": {
    "mermaid": "12.2.0",
    "vitepress": "1.6.4"
  },
  "type": "module",
  "packageManager": "pnpm@10.33.0"
}
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.25)
project(mistlane VERSION 0.1.0 LANGUAGES CXX)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "This release supports Linux; other platforms are not yet validated")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)
find_package(Boost 1.92 CONFIG REQUIRED COMPONENTS asio mysql redis)
find_package(OpenSSL 3.6.5 REQUIRED)
find_package(Drogon 1.9.14 CONFIG REQUIRED)
find_package(CURL 8.23 CONFIG REQUIRED)
find_package(Lua 5.5.1 REQUIRED)
find_package(crashpad CONFIG REQUIRED)
find_package(prometheus-cpp CONFIG REQUIRED)
add_library(mistlane_core src/lua_service.cpp src/diagnostics.cpp)
target_include_directories(mistlane_core PUBLIC include)
target_include_directories(mistlane_core SYSTEM PUBLIC ${LUA_INCLUDE_DIR})
target_link_libraries(mistlane_core PUBLIC
  Boost::asio Boost::redis Threads::Threads OpenSSL::SSL OpenSSL::Crypto ${LUA_LIBRARIES})
target_link_libraries(mistlane_core PUBLIC crashpad::crashpad prometheus-cpp::core prometheus-cpp::pull)
target_compile_options(mistlane_core PRIVATE -Wall -Wextra -Wpedantic)
add_library(mistlane_net src/node.cpp src/web.cpp)
target_link_libraries(mistlane_net PUBLIC mistlane_core Drogon::Drogon)
target_compile_options(mistlane_net PRIVATE -Wall -Wextra -Wpedantic)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/lua/mistlane/min_heap.lua" MISTLANE_HEAP_SOURCE)
configure_file(src/test_resources.hpp.in generated/test_resources.hpp @ONLY)
add_executable(mistlane src/main.cpp src/scaffold.cpp src/cli_client.cpp src/bot.cpp
  src/selftest.cpp src/test_cases.cpp src/test_databases.cpp src/lua_tests.cpp)
target_include_directories(mistlane PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(mistlane PRIVATE mistlane_net mistlane_http Boost::mysql)
target_compile_options(mistlane PRIVATE -Wall -Wextra -Wpedantic)
add_library(mistlane::core ALIAS mistlane_core)
add_library(mistlane::net ALIAS mistlane_net)
add_library(mistlane_mysql INTERFACE)
target_include_directories(mistlane_mysql INTERFACE include)
target_link_libraries(mistlane_mysql INTERFACE Boost::mysql OpenSSL::SSL OpenSSL::Crypto)
add_library(mistlane::mysql ALIAS mistlane_mysql)
add_executable(mistlane-mysql-probe examples/mysql_probe.cpp)
target_link_libraries(mistlane-mysql-probe PRIVATE mistlane_mysql)
include(CTest)
if(BUILD_TESTING)
  foreach(test_case IN ITEMS core.contracts transport.tcp transport.websocket transport.http
      lua.lifecycle lua.http cross.admission probe.control scaffold.init config.file cli.bot)
    string(REPLACE "." "_" test_name "${test_case}")
    add_test(NAME mistlane_${test_name} COMMAND mistlane test --case "${test_case}")
    set_tests_properties(mistlane_${test_name} PROPERTIES TIMEOUT 60)
  endforeach()
  if(EXISTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/crashpad_handler")
    add_test(NAME mistlane_crashpad_startup COMMAND mistlane test --suite crashpad
      --crashpad-handler "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/crashpad_handler")
    set_tests_properties(mistlane_crashpad_startup PROPERTIES TIMEOUT 15)
  endif()
endif()

option(MISTLANE_TEST_LOCAL_DATABASE "Test against a disposable local MariaDB (requires installed tools)" OFF)
if(BUILD_TESTING AND MISTLANE_TEST_LOCAL_DATABASE)
  add_test(NAME mistlane_mysql_integration COMMAND mistlane test --suite mysql)
  set_tests_properties(mistlane_mysql_integration PROPERTIES TIMEOUT 60)
endif()

option(MISTLANE_TEST_LOCAL_REDIS "Test against a disposable local Redis server" OFF)
if(BUILD_TESTING AND MISTLANE_TEST_LOCAL_REDIS)
  add_test(NAME mistlane_redis_integration COMMAND mistlane test --suite redis)
  set_tests_properties(mistlane_redis_integration PROPERTIES TIMEOUT 60)
endif()

add_library(mistlane_http src/http_client.cpp)
target_include_directories(mistlane_http PUBLIC include)
target_link_libraries(mistlane_http PUBLIC CURL::libcurl)
add_library(mistlane::http ALIAS mistlane_http)
add_executable(mistlane-http-probe examples/http_probe.cpp)
target_link_libraries(mistlane-http-probe PRIVATE mistlane_http)
add_executable(mistlane-crash-sample examples/crash_sample.cpp)
target_link_libraries(mistlane-crash-sample PRIVATE crashpad::crashpad)
target_compile_options(mistlane-crash-sample PRIVATE -Wall -Wextra -Wpedantic)

include(GNUInstallDirs)
install(TARGETS mistlane mistlane-crash-sample RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# The Crashpad handler ships next to the server binary; both only need the
# system C/C++ runtime because vcpkg builds static dependencies.
if(EXISTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/crashpad_handler")
  install(PROGRAMS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/crashpad_handler"
    DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
仓库里的 `todo.md` 实际有大量已勾选项，且还留有未勾选项；这与消息中的“完成 0、未完成 0”不一致。我会把仓库文件当作审查对象，不把清单中的自述当作已独立验证的证据。接下来重点核对网络边界、配置/探针、Lua/Redis 与测试覆盖。
[0m→ [0mRead src/main.cpp 
[0m→ [0mRead src/node.cpp 
[0m→ [0mRead src/web.cpp 
[0m→ [0mRead include/mistlane/node.hpp 
[0m→ [0mRead include/mistlane/cli.hpp 
[0m→ [0mRead src/lua_service.cpp 
[0m→ [0mRead src/diagnostics.cpp 
[0m→ [0mRead include/mistlane/cross.hpp 
[0m→ [0mRead include/mistlane/lua_service.hpp 
[0m→ [0mRead include/mistlane/diagnostics.hpp 
[0m→ [0mRead include/mistlane/transport.hpp 
[0m→ [0mRead docs/cross-server.md 
[0m✱ [0mGrep "Probe|PROBE|readiness|cross-server|metrics|limits|IPv6|profile|profiler|capacity" [90min docs · 86 matches[0m
[0m→ [0mRead docs/http.md 
[0m→ [0mRead docs/validation.md 
[0m$ [0mfind . -maxdepth 2 -type d -name 'build' -o -name 'dist' -o -name 'node_modules'
./node_modules
./build
[0m→ [0mRead src/http_client.cpp 
[0m→ [0mRead include/mistlane/http_client.hpp 
[0m→ [0mRead src/bot.cpp 
[0m→ [0mRead include/mistlane/mysql_pool.hpp 
[0m→ [0mRead include/mistlane/token_bucket.hpp 
[0m$ [0mctest --test-dir build/debug --output-on-failure
Test project /home/cui/workspaces/mistlane/build/debug
      Start  1: mistlane_core_contracts
 1/14 Test  #1: mistlane_core_contracts ..........   Passed    0.13 sec
      Start  2: mistlane_transport_tcp
 2/14 Test  #2: mistlane_transport_tcp ...........   Passed    2.24 sec
      Start  3: mistlane_transport_websocket
 3/14 Test  #3: mistlane_transport_websocket .....   Passed    2.57 sec
      Start  4: mistlane_transport_http
 4/14 Test  #4: mistlane_transport_http ..........   Passed    0.33 sec
      Start  5: mistlane_lua_lifecycle
 5/14 Test  #5: mistlane_lua_lifecycle ...........   Passed    0.21 sec
      Start  6: mistlane_lua_http
 6/14 Test  #6: mistlane_lua_http ................   Passed    0.10 sec
      Start  7: mistlane_cross_admission
 7/14 Test  #7: mistlane_cross_admission .........   Passed    0.18 sec
      Start  8: mistlane_probe_control
 8/14 Test  #8: mistlane_probe_control ...........   Passed    3.07 sec
      Start  9: mistlane_scaffold_init
 9/14 Test  #9: mistlane_scaffold_init ...........   Passed    1.27 sec
      Start 10: mistlane_config_file
10/14 Test #10: mistlane_config_file .............   Passed    2.27 sec
      Start 11: mistlane_cli_bot
11/14 Test #11: mistlane_cli_bot .................   Passed    0.25 sec
      Start 12: mistlane_crashpad_startup
12/14 Test #12: mistlane_crashpad_startup ........   Passed    1.23 sec
      Start 13: mistlane_mysql_integration
13/14 Test #13: mistlane_mysql_integration .......***Failed    0.21 sec
FAIL mysql.pool: temporary MariaDB initialization failed
tests=1 passed=0 failed=1

      Start 14: mistlane_redis_integration
14/14 Test #14: mistlane_redis_integration .......   Passed    6.09 sec

93% tests passed, 1 tests failed out of 14

Total Test time (real) =  20.15 sec

The following tests FAILED:
	 13 - mistlane_mysql_integration (Failed)
Errors while running CTest
[0m→ [0mRead src/scaffold.cpp 
[0m→ [0mRead tests/mysql_integration.py 
[0m→ [0mRead docs/architecture.md 
[0m→ [0mRead docs/architecture-comparison.md 
[0m→ [0mRead docs/migration-status.md 
[0m→ [0mRead docs/guide.md 
[0m→ [0mRead docs/lua.md 
[0m→ [0mRead docs/dependencies.md 
[0m→ [0mRead tests/integration.py 
[0m→ [0mRead docs/zh/architecture-comparison.md [90m[offset=25, limit=12][0m
[0m✱ [0mGrep "probe.control|config.file|PROBE1|overload|ipv6|probe_port|mistlane_mysql_integration" [90min src · 48 matches[0m
[0m✱ [0mGrep "\[ \]" [90min todo.md · 8 matches[0m
[0m$ [0mgit status --short
(no output)
[0m✱ [0mGrep "native config format|core binary does not parse|sustained load generator|mistlane bot|user-service template|ready\(" [90min docs · 5 matches[0m
[0m$ [0m./build/debug/mistlane --role cross --listen 127.0.0.1:39099 --upstream '[::1]:39001:39002'
session admission limit 42
fatal: Invalid argument [system:22]
### 表一 缺陷

| 问题 | file:line | 为什么是问题 | 建议动作 |
|---|---|---|---|
| **高：探针异步写入引用已销毁的缓冲区** | `src/node.cpp:335-342`、`src/node.cpp:163-166` | 上游探针请求在 `async_write` 完成回调中没有保活；响应写入则直接把临时字符串 `text+'\n'` 交给异步操作。Asio buffer 不拥有数据，这两条路径都可能在写完前失效，导致探针响应损坏或未定义行为。 | 用共享所有权或成员缓冲区保活数据，直到异步写操作完成；增加重复探针与延迟写入测试。 |
| **高：探针连接对象随运行时间累积** | `src/node.cpp:184-187`、`src/node.cpp:192` | 每个连接都加入 `peers_`，完成或超时后只重置 `owner_`，没有从集合移除。cross 对每个新会话探测时，会不断留下 Peer 对象，形成持续内存增长。 | Peer 结束时从 `peers_` 移除自身；增加大量连接后的存活对象/内存回归测试。 |
| **中高：探针输入和并发连接没有上限** | `src/node.cpp:109-116`、`src/node.cpp:119-126`、`src/node.cpp:180-192` | `async_read_until` 对 LF 行没有大小限制，`peers_` 也没有并发连接上限。两秒超时不能阻止客户端在超时前持续发送数据或建立大量连接，可能耗尽内存和连接资源。 | 设置最大请求行长度、探针并发上限和超限处理；限制失败时不要影响主服务循环。 |
| **中：Prometheus ready 指标与实际准入状态脱节** | `src/node.cpp:543-548`、`src/node.cpp:557-561`、`src/diagnostics.cpp:27-28`、`src/diagnostics.cpp:86` | `admitting_` 因过载变为 false 时，探针会报 `notready`，但 `mistlane_ready` 只在启动和停止时更新，饱和期间仍可能为 1，监控与探针给出相反结论。 | 明确指标语义；若代表就绪状态，在迟滞翻转时同步更新；也可另设进程存活与准入状态指标。 |
| **中：准入拒绝指标漏计跨服拒绝** | `src/node.cpp:244-251`、`src/node.cpp:287-290`、`src/node.cpp:499-505`、`src/diagnostics.cpp:90` | `mistlane_admission_rejections_total` 的 `reject()` 只在本地连接数超限时调用；`ROOM_FULL`、`GROUP_FULL`、`SHARD_FULL`、`NOT_READY` 等跨服拒绝不会计入，过载监控会低报。 | 在统一拒绝路径按拒绝原因计数，并为跨服准入与探针拒绝增加指标测试。 |
| **中：带探针端口的方括号 IPv6 上游无法启动** | `src/main.cpp:35-46`、`src/node.cpp:437` | `upstream()` 接受 `[::1]:PORT:PROBE`，但把包含方括号的地址原样传给 `make_address`；本地实跑该形式以 `Invalid argument` 退出。 | 解析时去除地址两侧方括号，或统一规范化 Endpoint；补方括号 IPv6 上游测试。 |
| **低：本次可选 MariaDB 集成测试未通过** | `tests/mysql_integration.py:19-24` | 本次 `ctest` 为 13/14；`mistlane_mysql_integration` 报 `temporary MariaDB initialization failed`。失败原因**未核实**，不能据此认定是产品缺陷，但当前环境未能复现该验证通过状态。 | 保存 `mariadb-install-db` 的 stderr 和返回码，确认工具/环境原因后重跑；不要把本次结果记为通过。 |

### 表二 文档与实现不一致

| 文档说法 | 文档位置 | 代码实际 | 代码位置 |
|---|---|---|---|
| 核心二进制不解析原生配置格式。 | `docs/architecture-comparison.md:32`；中文对应 `docs/zh/architecture-comparison.md:32` | 二进制支持 `--config FILE`，读取带版本头的 `KEY=VALUE` 文件，并将字段展开为 CLI 参数。 | `src/main.cpp:56-73`、`src/main.cpp:154-165` |
| cross 压测可直接运行 `mistlane bot --url tcp://… --clients 64 --count 500`。 | `docs/cross-server.md:102-106`；中文对应 `docs/zh/cross-server.md:117-121` | cross 的首帧必须是 MLC1 握手；该命令没有 `--join`，bot 默认发送 `hello mistlane`，会被拒绝。另外默认内存准入容量为 42，64 个客户端超过上限。 | `src/bot.cpp:27-30`、`src/bot.cpp:93-97`；`src/node.cpp:417-420` |
| “sustained load generator is not yet integrated”。 | `docs/cross-server.md:66` | 已有 `mistlane bot` 并发请求生成器，也有可选 Prometheus 抓取端点；“生产级采集/持续压测尚未验证”可以成立，但“生成器未集成”过时。 | `src/bot.cpp:27-48`、`src/bot.cpp:92-124`；`src/diagnostics.cpp:23-63` |
| 生成的 systemd 文件是 user-service 模板。 | `src/scaffold.cpp:39`（生成 README 文案） | 实际 unit 指定系统用户/组并安装到 `multi-user.target`，是系统级服务模板。 | `src/scaffold.cpp:308-330` |

### 表三 值得补的能力

| 能力 | 为什么现在没有 | 价值高低 |
|---|---|---|
| 跨服连接身份认证及内部探针的 TLS/mTLS | 当前跨服握手只有路由字段；探针是普通 TCP，令牌鉴权可选。 | 高；若监听超出可信内网，伪造连接或探针请求会扩大风险。证据：`include/mistlane/cross.hpp:12-17`、`src/node.cpp:81-83`、`src/node.cpp:151-159`、`docs/cross-server.md:52` |
| 房间 drain、lease/fencing 与安全迁移 | 当前是静态 shard 列表和本地准入；变更上游顺序会重映射房间，没有在线迁移协议。 | 高；拓扑变更和故障恢复前置能力。证据：`docs/cross-server.md:56-60`、`docs/architecture-comparison.md:60-62` |
| Lua 内存配额及更强的 CPU 隔离 | Lua VM 没有内存预算；指令 hook 只拦截常见失控脚本，不构成完整隔离。 | 高；脚本可阻塞或增加进程内存压力。证据：`src/lua_service.cpp:136-140`、`docs/lua.md:118-120` |
| Crashpad 转储保留上限和自动清理 | 目前初始化本地 Crashpad 数据库并关闭上传，没有配额或保留清理逻辑。 | 中高；持续崩溃可能耗尽磁盘。证据：`src/diagnostics.cpp:66-81`、`docs/guide.md:240-244` |
| 游戏连接的服务端推送、广播与全双工队列 | 当前按请求—响应串行处理，没有 Lua 主动推送接口；Web 通道每次只允许一个待处理 payload。 | 高（实时玩法）/中（请求响应玩法）。证据：`docs/guide.md:125-129`、`src/web.cpp:39-45`、`src/node.cpp:234-244` |
| 目标环境下可重复的压力与 profiler 影响记录 | 现有验证明确没有目标环境的容量、采样开销和压力演练数据。 | 高；决定准入值、容量边界和生产部署前提。证据：`docs/cross-server.md:98-108`、`docs/validation.md:53`、`todo.md:193-195` |
| 版本、构建和节点身份指标 | 现有指标覆盖会话、流量、延迟和内存，但没有 build/version/node identity 指标。 | 中；多节点排障和版本分布检查更直接。证据：`src/diagnostics.cpp:23-60`、`todo.md:181` |
