// SPDX-License-Identifier: Apache-2.0
#include "test_common.hpp"
namespace mistlane::test {
void scaffold_init() {
    Context c; const auto root=c.path("sample service");
    require(c.command({self().string(),"init",root.string()})==0,"initializer failed");
    for (const auto& name : {"scripts/echo.lua","scripts/http.lua","scripts/cross.lua","scripts/login.lua","scripts/record.lua",
        "config/instances/gateway.env","config/instances/gateway-ws.env","config/instances/gateway-wss.env",
        "config/instances/login.env","config/instances/data.env","config/instances/coordinator.env",
        "config/instances/gameServer-mixed.env","ops/systemd/mistlane@.service"})
        require(fs::is_regular_file(root/name),"missing template: "+std::string(name));
    for (const auto& name : {"ops/run-instance.sh","ops/cluster.sh","run-local.sh","run-cross-local.sh"}) {
        require(::access((root/name).c_str(),X_OK)==0,"launcher is not executable");
        require(c.command({"sh","-n",(root/name).string()})==0,"invalid shell syntax");
    }
    const auto capture=c.path("args"), recorder=c.path("recorder.sh");
    write(recorder,"#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$MISTLANE_CAPTURE\"\n");
    fs::permissions(recorder,fs::perms::owner_all);
    ScopedEnv bin("MISTLANE_BIN",recorder.string()), output("MISTLANE_CAPTURE",capture.string());
    require(c.command({(root/"ops/run-instance.sh").string(),"gateway-ws"})==0,"WS template launch failed");
    require(read(capture).find("--transport\nws\n")!=std::string::npos,"WS transport not forwarded");
    require(c.command({(root/"ops/run-instance.sh").string(),"gateway-wss"})!=0,"missing certificate accepted");
    const auto cert=root/"certs/server chain.pem", key=c.path("server key.pem");
    write(cert,"fixture"); write(key,"fixture");
    auto config=read(root/"config/instances/gateway-wss.env");
    config.replace(config.find("CERT=certs/fullchain.pem"),std::string("CERT=certs/fullchain.pem").size(),"CERT='certs/server chain.pem'");
    config.replace(config.find("KEY=certs/private-key.pem"),std::string("KEY=certs/private-key.pem").size(),"KEY='"+key.string()+"'");
    write(root/"config/instances/gateway-wss.env",config);
    require(c.command({(root/"ops/run-instance.sh").string(),"gateway-wss"})==0,"WSS template launch failed");
    require(read(capture).find("--cert\n"+cert.string())!=std::string::npos,"relative certificate path not resolved");
    require(read(capture).find("--key\n"+key.string())!=std::string::npos,"absolute key path not resolved");
    require(c.command({(root/"ops/run-instance.sh").string(),"gameServer-mixed"})==0,"mixed template launch failed");
    require(read(capture).find("--listener\nws://127.0.0.1:7082\n")!=std::string::npos,"LISTENERS not forwarded");
    require(c.command({(root/"ops/run-instance.sh").string(),"login"})==0,"login template launch failed");
    require(read(capture).find("--coordinator\n127.0.0.1:7500\n")!=std::string::npos,"COORDINATOR not forwarded");
    require(read(capture).find("--scope\nmain\n")!=std::string::npos,"SCOPE not forwarded");
    require(c.command({(root/"ops/run-instance.sh").string(),"coordinator"})==0,"coordinator template launch failed");
    require(read(capture).find("--script")==std::string::npos,"the coordinator launched a script");
    require(c.command({(root/"ops/run-instance.sh").string(),"data"})==0,"data template launch failed");
    require(read(capture).find("--coordinator")==std::string::npos,"data claimed a coordinator");
    {
        ScopedEnv real_binary("MISTLANE_BIN",self().string());
        const auto listen=port();
        write(root/"config/instances/gameServer.env","ROLE=gameServer\nLISTEN="+endpoint(listen)+"\nSCRIPT=scripts/echo.lua\nTRANSPORT=tcp\nUPSTREAMS=\nROUTE_EPOCH=\n");
        auto& node=c.spawn({(root/"ops/run-instance.sh").string(),"gameServer"});
        node.ready(); equal(tcp(listen)->request("template-path-check"),"template-path-check");
        node.signal(SIGTERM); require(node.wait()==0,"generated gameServer shutdown failed");
    }
    {
        // The registry trio boots from its own templates: the coordinator is
        // scriptless, login asks it through the sample script and answers the
        // client from the placement callback.
        ScopedEnv real_binary("MISTLANE_BIN",self().string());
        const auto registry=port(), login_listen=port();
        write(root/"config/instances/coordinator.env","ROLE=coordinator\nLISTEN="+endpoint(registry)+"\nSCRIPT=\nTRANSPORT=tcp\nUPSTREAMS=\nROUTE_EPOCH=\n");
        write(root/"config/instances/login.env","ROLE=login\nLISTEN="+endpoint(login_listen)+"\nSCRIPT=scripts/login.lua\nTRANSPORT=tcp\nUPSTREAMS=\nROUTE_EPOCH=\nCOORDINATOR="+endpoint(registry)+"\nSCOPE=main\n");
        auto& registry_node=c.spawn({(root/"ops/run-instance.sh").string(),"coordinator"});
        registry_node.ready();
        auto& login_node=c.spawn({(root/"ops/run-instance.sh").string(),"login"});
        login_node.ready();
        equal(tcp(login_listen)->request("place gameServer"),"ERR EMPTY");
        login_node.signal(SIGTERM); require(login_node.wait()==0,"generated login shutdown failed");
        registry_node.signal(SIGTERM); require(registry_node.wait()==0,"generated coordinator shutdown failed");
    }
    {
        // cluster.sh gates each start on the instance reporting readiness and
        // rolls back instances started by the same command on failure.
        ScopedEnv real_binary("MISTLANE_BIN",self().string());
        const auto cluster=(root/"ops/cluster.sh").string();
        const auto listen=port();
        write(root/"config/instances/gameServer.env","ROLE=gameServer\nLISTEN="+endpoint(listen)+"\nSCRIPT=scripts/echo.lua\nTRANSPORT=tcp\nUPSTREAMS=\nROUTE_EPOCH=\n");
        const auto forwarder=root/"config/instances/gateway.env";
        const auto original_gateway=read(forwarder);
        write(forwarder,"ROLE=gateway\nLISTEN="+endpoint(port())+"\nSCRIPT=scripts/missing.lua\nTRANSPORT=tcp\nUPSTREAMS="+endpoint(listen)+"\nROUTE_EPOCH=\n");
        auto& started=c.spawn({cluster,"start","gameServer"});
        started.reap_descendants(false);  // the daemon must outlive its launcher
        require(started.wait()==0,"cluster start failed");
        equal(tcp(listen)->request("ready-gate"),"ready-gate");
        auto& preserved=c.spawn({cluster,"start","gameServer","gateway"});
        require(preserved.wait()!=0,"broken forwarder instance was accepted");
        require(preserved.output().find("stopping instances started by this command")!=std::string::npos,
            "rollback banner missing");
        auto& preserved_status=c.spawn({cluster,"status","gameServer","gateway"});
        preserved_status.wait();
        require(preserved_status.output().find("gameServer running")!=std::string::npos,
            "pre-existing instance was rolled back");
        require(preserved_status.output().find("gateway stopped")!=std::string::npos,
            "broken instance reported running");
        auto& stopped=c.spawn({cluster,"stop","gameServer"});
        require(stopped.wait()==0,"cluster stop failed");
        auto& rolled_back=c.spawn({cluster,"start","gameServer","gateway"});
        require(rolled_back.wait()!=0,"broken sequence was accepted");
        auto& final_status=c.spawn({cluster,"status","gameServer","gateway"});
        final_status.wait();
        require(final_status.output().find("gameServer stopped")!=std::string::npos,
            "freshly started instance survived rollback");
        require(final_status.output().find("gateway stopped")!=std::string::npos,
            "forwarder reported running after failure");
        write(forwarder,original_gateway);
    }
    write(root/"keep.txt","preserve");
    require(c.command({self().string(),"init",root.string()})!=0,"initializer overwrote nonempty project");
    equal(read(root/"keep.txt"),"preserve");
}
}
