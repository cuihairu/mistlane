// SPDX-License-Identifier: Apache-2.0
#include "test_common.hpp"
namespace mistlane::test {
void crashpad_startup(const fs::path& handler) {
    require(fs::is_regular_file(handler),"provide --crashpad-handler FILE for the crashpad suite");
    Context c; write(c.path("echo.lua"),echo); const auto listen=port();
    auto& node=c.node("gameServer",listen,{"--script",c.path("echo.lua").string(),"--crashpad-handler",handler.string(),
        "--crashpad-database",c.path("reports").string()});
    require(fs::is_directory(c.path("reports")) && !fs::is_empty(c.path("reports")),"Crashpad database missing");
    equal(tcp(listen)->request("crashpad"),"crashpad");
    node.signal(SIGTERM); require(node.wait()==0,"Crashpad-enabled gameServer shutdown failed");
}
void crashpad_minidump(const fs::path& handler) {
    require(fs::is_regular_file(handler),"provide --crashpad-handler FILE for the crashpad suite");
    const auto sample=self().parent_path()/"mistlane-crash-sample";
    require(fs::is_regular_file(sample),"build mistlane-crash-sample for the crashpad suite");
    Context c; const auto database=c.path("reports");
    auto& victim=c.spawn({sample.string(),"--crashpad-handler",handler.string(),
        "--crashpad-database",database.string()});
    // The handler is a descendant that must stay alive long enough to finish
    // writing the dump, so opt out of the automatic group kill.
    victim.reap_descendants(false);
    const pid_t group=victim.group();
    bool found=false;
    eventually([&] {
        if (!fs::is_directory(database)) return false;
        for (auto entry : fs::recursive_directory_iterator(database)) {
            if (!entry.is_regular_file()) continue;
            std::ifstream stream(entry.path(),std::ios::binary); char magic[4]{};
            stream.read(magic,sizeof(magic));
            // Minidumps start with the MDMP magic and are far larger than a header.
            if (std::string(magic,magic+sizeof(magic))=="MDMP"
                && fs::file_size(entry.path())>4096) return found=true;
        }
        return false;
    },10s);
    // Give the handler a moment to finish the file before tearing it down.
    std::this_thread::sleep_for(100ms);
    require(victim.wait()==128+SIGSEGV,"crash sample did not die from SIGSEGV");
    require(found,"crashpad database never received a minidump");
    // The handler outlives the sample on purpose; do not leak it.
    if (group>0) ::kill(-group,SIGKILL);
}

}
