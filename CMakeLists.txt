# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.25)
project(mistlane VERSION 0.1.0 LANGUAGES C CXX)
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
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(crashpad CONFIG REQUIRED)
find_package(prometheus-cpp CONFIG REQUIRED)
add_library(mistlane_core src/lua_service.cpp src/diagnostics.cpp src/api.cpp src/api_store.cpp src/entity_lua.cpp src/journal.cpp)
target_include_directories(mistlane_core PUBLIC include)
target_include_directories(mistlane_core SYSTEM PUBLIC ${LUA_INCLUDE_DIR})
target_link_libraries(mistlane_core PUBLIC
  Boost::asio Boost::redis Threads::Threads OpenSSL::SSL OpenSSL::Crypto ${LUA_LIBRARIES}
  unofficial::sqlite3::sqlite3)
target_link_libraries(mistlane_core PUBLIC crashpad::crashpad prometheus-cpp::core prometheus-cpp::pull)
target_compile_options(mistlane_core PRIVATE -Wall -Wextra -Wpedantic)
add_library(mistlane_net src/node.cpp src/web.cpp)
target_link_libraries(mistlane_net PUBLIC mistlane_core Drogon::Drogon)
target_compile_options(mistlane_net PRIVATE -Wall -Wextra -Wpedantic)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/lua/mistlane/min_heap.lua" MISTLANE_HEAP_SOURCE)
configure_file(src/test_resources.hpp.in generated/test_resources.hpp @ONLY)
add_executable(mistlane src/main.cpp src/scaffold.cpp src/contract_tool.cpp src/cli_client.cpp src/bot.cpp
  src/selftest.cpp src/test_cases.cpp src/test_api.cpp src/test_databases.cpp src/lua_tests.cpp)
target_include_directories(mistlane PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(mistlane PRIVATE mistlane_net mistlane_http Boost::mysql)
target_compile_options(mistlane PRIVATE -Wall -Wextra -Wpedantic)
add_executable(mistlane-sproto-probe
  src/sproto_probe.cpp
  third_party/sproto/sproto.c third_party/sproto/lsproto.c
  third_party/lpeg/lpcap.c third_party/lpeg/lpcode.c third_party/lpeg/lpcset.c
  third_party/lpeg/lpprint.c third_party/lpeg/lptree.c third_party/lpeg/lpvm.c)
target_include_directories(mistlane-sproto-probe PRIVATE third_party/sproto third_party/lpeg ${LUA_INCLUDE_DIR})
target_include_directories(mistlane-sproto-probe PRIVATE include)
target_link_libraries(mistlane-sproto-probe PRIVATE ${LUA_LIBRARIES})
target_compile_options(mistlane-sproto-probe PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(mistlane-sproto-probe PRIVATE
  MISTLANE_SPROTO_LUA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/third_party/sproto"
  MISTLANE_LPEG_LUA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/third_party/lpeg")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sproto-fixture/avatar.sproto"
  COMMAND $<TARGET_FILE:mistlane> contract build --out "${CMAKE_CURRENT_BINARY_DIR}/sproto-fixture"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/contracts/avatar.def"
  DEPENDS mistlane "${CMAKE_CURRENT_SOURCE_DIR}/examples/contracts/avatar.def"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/contracts/types.xml"
  VERBATIM)
add_custom_target(mistlane_sproto_fixture DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/sproto-fixture/avatar.sproto")
add_dependencies(mistlane-sproto-probe mistlane_sproto_fixture)
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
  foreach(test_case IN ITEMS core.contracts contract.tool sproto.golden rpc.lifecycle core.roles transport.tcp transport.websocket transport.http
      transport.push transport.correlate transport.identity transport.multiplex coordinator.registry
      entity.envelope
      entity.properties
      entity.tree
      entity.sync
      entity.rpc
      entity.endpoint
      entity.transport
      entity.transport.gateway
      entity.codec
      entity.snapshot
      entity.gateway
      entity.persist
      entity.perf
      coordinator.placement coordinator.routes coordinator.migration.commit coordinator.migration.abort
      coordinator.migration.recover coordinator.migration.stale coordinator.migration.order
      coordinator.migration.retire coordinator.migration.wire coordinator.migration.journal coordinator.migration.journal.recovery cross.ticket lua.lifecycle lua.http
      coordinator.migration.authority
      api.signature api.tamper api.rotation api.replay api.acceptance api.acceptance.sqlite api.ownership api.wire api.http
      login.ticket login.ticket.wire
      cross.admission cross.record cross.record.journal cross.return.queue probe.control scaffold.init
      config.file cli.bot)
    string(REPLACE "." "_" test_name "${test_case}")
    add_test(NAME mistlane_${test_name} COMMAND mistlane test --case "${test_case}")
    set_tests_properties(mistlane_${test_name} PROPERTIES TIMEOUT 60)
  endforeach()
  add_test(NAME mistlane_sproto_upstream_parser COMMAND mistlane-sproto-probe parser
    "${CMAKE_CURRENT_BINARY_DIR}/sproto-fixture/avatar.sproto")
  add_test(NAME mistlane_sproto_wire_golden COMMAND mistlane-sproto-probe golden
    "${CMAKE_CURRENT_BINARY_DIR}/sproto-fixture/avatar.sproto")
  add_test(NAME mistlane_sproto_sdk_cpp_golden COMMAND mistlane-sproto-probe sdk-vector
    "${CMAKE_CURRENT_BINARY_DIR}/sproto-fixture/avatar.sproto")
  find_program(MISTLANE_NODE_EXECUTABLE node)
  if(MISTLANE_NODE_EXECUTABLE)
    add_test(NAME mistlane_sproto_sdk_js_golden COMMAND "${MISTLANE_NODE_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/examples/sproto_sdk_golden.mjs"
      "${CMAKE_CURRENT_BINARY_DIR}/sproto-fixture/avatar.bindings.js")
  endif()
  add_test(NAME mistlane_sproto_lua_roundtrip COMMAND mistlane-sproto-probe roundtrip
    "${CMAKE_CURRENT_BINARY_DIR}/sproto-fixture/avatar.sproto")
  add_test(NAME mistlane_sproto_dispatch COMMAND mistlane-sproto-probe dispatch
    "${CMAKE_CURRENT_BINARY_DIR}/sproto-fixture/avatar.sproto")
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
