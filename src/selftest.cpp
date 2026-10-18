// SPDX-License-Identifier: Apache-2.0
#include "mistlane/cli.hpp"
#include "test_support.hpp"
#include "lua_tests.hpp"
#include <charconv>
#include <algorithm>

namespace mistlane::test {
void core_contracts();
void transport_tcp();
void transport_websocket();
void transport_http();
void lua_lifecycle();
void lua_http();
void cross_admission();
void probe_control();
void scaffold_init();
void cli_bot();
void crashpad_startup(const fs::path&);
void crashpad_minidump(const fs::path&);
void redis_lua_api();
void redis_tls();
void mysql_pool();
}
namespace mistlane {
int run_tests(int argc, char** argv) {
    struct Case { const char* name; const char* suite; std::function<void()> run; bool external=false; };
    std::string suite="all", selected;
    std::vector<std::string> scripts;
    unsigned timeout_ms=5000;
    bool list=false;
    auto handler=test::self().parent_path()/"crashpad_handler";
    auto installed=test::self().parent_path()/"vcpkg_installed/x64-linux/tools/crashpad_handler";
    if (!test::fs::is_regular_file(handler) && test::fs::is_regular_file(installed)) handler=installed;
    for (int i=2;i<argc;++i) {
        const std::string key=argv[i];
        if (key=="--help") {
            std::cout << "mistlane test [--suite NAME | --case NAME] [--list] [--keep-temp]\n"
                "mistlane test --script FILE [--script FILE ...] [--case SUITE.CASE] [--timeout-ms N]\n"
                "Suites: core, transport, lua, cross, probe, scaffold, cli, crashpad, redis, mysql.\n"
                "Default 'all' runs dependency-free suites; Redis/MySQL/Crashpad are opt-in.\n"
                "  --crashpad-handler FILE  Handler used by the crashpad suite\n"
                "  --list                   List matching cases without running them\n"
                "  --keep-temp              Keep isolated logs/fixtures after the run\n"
                "Lua scripts return {name=..., setup=function(t)...end, teardown=function(t)...end, cases={...}}.\n"
                "Runs real child processes and returns nonzero if any selected case fails.\n";
            return 0;
        }
        if (key=="--list") {list=true;continue;}
        if (key=="--keep-temp") {test::keep_temp=true;continue;}
        if (++i>=argc) throw std::invalid_argument("missing value for "+key);
        if (key=="--suite") suite=argv[i];
        else if (key=="--case") selected=argv[i];
        else if (key=="--script") scripts.emplace_back(argv[i]);
        else if (key=="--timeout-ms") {
            const std::string_view value=argv[i];
            const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),timeout_ms);
            if (error!=std::errc{} || end!=value.data()+value.size() || !timeout_ms || timeout_ms>60000)
                throw std::invalid_argument("test timeout_ms must be between 1 and 60000");
        }
        else if (key=="--crashpad-handler") handler=argv[i];
        else throw std::invalid_argument("unknown test option: "+key);
    }
    if (!scripts.empty()) {
        if (suite!="all") throw std::invalid_argument("choose native --suite or Lua --script");
        return cli::run_lua_tests(scripts,selected,list,timeout_ms);
    }
    const std::vector<Case> cases{
        {"core.contracts","core",test::core_contracts},
        {"transport.tcp","transport",test::transport_tcp},
        {"transport.websocket","transport",test::transport_websocket},
        {"transport.http","transport",test::transport_http},
        {"lua.lifecycle","lua",test::lua_lifecycle},
        {"lua.http","lua",test::lua_http},
        {"cross.admission","cross",test::cross_admission},
        {"probe.control","probe",test::probe_control},
        {"scaffold.init","scaffold",test::scaffold_init},
        {"cli.bot","cli",test::cli_bot},
        {"crashpad.startup","crashpad",[&] {test::crashpad_startup(handler);},true},
        {"crashpad.minidump","crashpad",[&] {test::crashpad_minidump(handler);},true},
        {"redis.lua","redis",test::redis_lua_api,true},
        {"redis.tls","redis",test::redis_tls,true},
        {"mysql.pool","mysql",test::mysql_pool,true},
    };
    unsigned passed=0,failed=0,matched=0;
    for (const auto& item:cases) {
        if (!selected.empty() ? selected!=item.name : suite!="all" ? suite!=item.suite : item.external && !list) continue;
        ++matched;
        if (list) {std::cout<<item.name<<" ["<<item.suite<<"]"<<(item.external?" (opt-in external service/tool)":"")<<'\n';continue;}
        const auto begin=std::chrono::steady_clock::now();
        try {
            item.run();++passed;
            std::cout<<"PASS "<<item.name<<" "<<std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count()<<"s\n";
        } catch (const std::exception& error) {
            ++failed; std::cerr<<"FAIL "<<item.name<<": "<<error.what()<<'\n';
        }
    }
    if (!matched) throw std::invalid_argument("no matching test case or suite");
    if (!list) std::cout<<"tests="<<matched<<" passed="<<passed<<" failed="<<failed<<'\n';
    return failed?1:0;
}
}
