// SPDX-License-Identifier: Apache-2.0
#include "test_support.hpp"
#include "test_fixtures.hpp"
#include "test_resources.hpp"
#include "mistlane/cross.hpp"
#include "mistlane/http_client.hpp"
#include "mistlane/token_bucket.hpp"
#include <curl/curl.h>
#include <cstring>
#include <iomanip>
#include <future>
#include <sstream>
#include <netinet/in.h>

namespace mistlane::test {
namespace {
constexpr std::string_view echo = "return {message=function(p) return p end}";
class RawSocket {
public:
    explicit RawSocket(unsigned port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        require(fd_ >= 0, "test socket failed");
        timeval timeout{3, 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_port = htons(static_cast<std::uint16_t>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(fd_); throw std::runtime_error("raw TCP connect failed");
        }
    }
    ~RawSocket() { ::close(fd_); }
    void send(std::string_view value) {
        while (!value.empty()) {
            auto n = ::send(fd_, value.data(), value.size(), MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "raw TCP send failed");
            value.remove_prefix(static_cast<std::size_t>(n));
        }
    }
    std::string receive(std::size_t size) {
        std::string result(size, '\0');
        std::size_t offset = 0;
        while (offset < size) {
            auto n = ::recv(fd_, result.data()+offset, size-offset, 0);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "raw TCP closed or timed out");
            offset += static_cast<std::size_t>(n);
        }
        return result;
    }
    bool closed() {
        char ch;
        auto n = ::recv(fd_, &ch, 1, 0);
        return n == 0 || (n < 0 && errno == ECONNRESET);
    }
private:
    int fd_;
};
struct Http {
    long status = 0;
    std::string body, headers;
};
Http http(const std::string& url, const std::string& method = "GET",
          const std::string& payload = {}, const std::string& ca = {}, bool metadata = false) {
    static const int runtime = [] { require(curl_global_init(CURL_GLOBAL_DEFAULT) == 0, "curl initialization failed"); return 1; }();
    (void)runtime;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    require(static_cast<bool>(handle), "curl handle failed");
    Http result;
    curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    if (!ca.empty()) curl_easy_setopt(handle.get(), CURLOPT_CAINFO, ca.c_str());
    auto append = +[](char* bytes, std::size_t width, std::size_t count, void* context) -> std::size_t {
        auto& target = *static_cast<std::string*>(context);
        if (width && count > (1024*1024)/width) return 0;
        const auto n = width*count;
        if (target.size()+n > 1024*1024) return 0;
        target.append(bytes, n); return n;
    };
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, append);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, append);
    curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &result.headers);
    if (method == "POST") {
        curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, payload.data());
        curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.size()));
    }
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
        metadata ? curl_slist_append(nullptr, "x-test: yes") : nullptr, curl_slist_free_all);
    if (headers) curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
    const auto error = curl_easy_perform(handle.get());
    if (error != CURLE_OK) throw std::runtime_error(curl_easy_strerror(error));
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &result.status);
    return result;
}
std::vector<std::string> tls(const Context& c) {
    return {"--cert", c.path("cert.pem").string(), "--key", c.path("key.pem").string()};
}
}
void core_contracts() {
    TokenBucket bucket(2, 0.000001);
    require(bucket.consume() && bucket.consume() && !bucket.consume(), "token budget is not enforced");
    must_fail([] { TokenBucket invalid(0, 1); });
    auto context = cross_context("MLC1 1 realm group room");
    equal(context.room, "room");
    must_fail([] { (void)cross_context("MLC1 0 realm group room"); });
    must_fail([] { (void)cross_context("MLC1 1 realm group room extra"); });
    CrossAdmission admission(1, 1, 1, 1);
    int a = 0, b = 0;
    require(admission.acquire(&a, context, 0).empty(), "first admission failed");
    equal(admission.acquire(&b, context, 0), "ROOM_FULL");
    admission.release(&a);
    require(admission.acquire(&b, context, 0).empty(), "admission did not recover on release");
    Context c;
    for (auto args : std::vector<std::vector<std::string>>{
        {"--role", "invalid"}, {"--listen", "127.0.0.1:65536"}, {"--role", "worker"},
        {"--role", "gateway"}, {"--max-connections", "0"}, {"--role", "worker", "--transport", "wss"}}) {
        args.insert(args.begin(), self().string());
        require(c.command(args) != 0, "invalid CLI options accepted");
    }
}
void transport_tcp() {
    Context c;
    const auto script = c.path("echo.lua");
    write(script, echo);
    const auto worker=port(), router=port(), gateway=port(), metrics=port();
    c.node("worker", worker, {"--script", script.string(), "--metrics-listen", endpoint(metrics)});
    c.node("router", router, {"--upstream", endpoint(worker)});
    c.node("gateway", gateway, {"--upstream", endpoint(router)});
    auto client = tcp(gateway);
    for (const auto& payload : std::vector<std::string>{{}, "hello", std::string("\0\xff", 2), "中文", std::string(1024*1024, 'x')})
        equal(client->request(payload), payload);
    for (int i=0; i<100; ++i) equal(client->request("stack"), "stack");
    auto samples = http("http://"+endpoint(metrics)+"/metrics");
    require(samples.status == 200 && samples.body.find("mistlane_ready{role=\"worker\"} 1") != std::string::npos,
        "Prometheus readiness missing");
    require(samples.body.find("mistlane_lua_callback_seconds_count") != std::string::npos, "callback histogram missing");
    const std::string frame("\0\0\0\5abcde", 9);
    {
        RawSocket raw(gateway);
        for (char ch : frame) raw.send(std::string_view(&ch, 1));
        equal(raw.receive(frame.size()), frame);
        raw.send(frame+frame);
        equal(raw.receive(2*frame.size()), frame+frame);
        raw.send(std::string("\0\x10\0\1", 4));
        require(raw.closed(), "oversize frame was accepted");
    }
    std::vector<std::future<void>> tasks;
    for (int i=0; i<8; ++i) tasks.push_back(std::async(std::launch::async, [=] {
        auto peer = tcp(gateway); equal(peer->request("parallel"), "parallel");
    }));
    for (auto& task : tasks) task.get();
    const auto limited = port();
    c.node("worker", limited, {"--script", script.string(), "--max-connections", "1"});
    auto first = tcp(limited); equal(first->request("one"), "one");
    RawSocket extra(limited);
    require(extra.closed(), "connection admission was not enforced");
    equal(first->request("still-alive"), "still-alive");
    const auto dead = port(), ingress = port();
    c.node("gateway", ingress, {"--upstream", endpoint(dead)});
    must_fail([&] { (void)tcp(ingress)->request("unavailable"); });
}
void transport_websocket() {
    Context c; c.cert(); write(c.path("echo.lua"), echo);
    const auto worker = port(), router = port();
    c.node("worker", worker, {"--script", c.path("echo.lua").string()});
    c.node("router", router, {"--upstream", endpoint(worker)});
    for (const auto& transport : {"ws", "wss"}) {
        const bool secure = std::string_view(transport) == "wss";
        for (const auto& role : {"gateway", "worker"}) {
            const auto listen = port();
            std::vector<std::string> args{"--transport", transport};
            if (secure) { auto cert = tls(c); args.insert(args.end(), cert.begin(), cert.end()); }
            if (std::string_view(role) == "gateway") args.insert(args.end(), {"--upstream", endpoint(router)});
            else args.insert(args.end(), {"--script", c.path("echo.lua").string()});
            c.node(role, listen, args);
            for (const auto& path : {"/", "/ws"}) {
                cli::ClientOptions options{std::string(transport)+"://localhost:"+std::to_string(listen)+path,
                    secure ? c.path("cert.pem").string() : ""};
                auto client = cli::make_client(options);
                for (const auto& data : std::vector<std::string>{{}, "中文", std::string("\0\xff", 2), std::string(65536, 'w')})
                    equal(client->request(data), data);
            }
            if (secure) {
                // Certificate trust and hostname verification must stay enabled.
                must_fail([&] { auto peer = cli::make_client({"wss://localhost:"+std::to_string(listen)+"/ws", {}, 500}); });
                must_fail([&] { auto peer = cli::make_client({"wss://"+endpoint(listen)+"/ws", c.path("cert.pem").string(), 500}); });
            }
        }
    }
}
void transport_http() {
    Context c; c.cert(); write(c.path("echo.lua"), echo);
    const auto worker = port();
    c.node("worker", worker, {"--script", c.path("echo.lua").string()});
    for (const auto& transport : {"http", "https"}) {
        const bool secure = std::string_view(transport) == "https";
        const auto listen = port();
        std::vector<std::string> args{"--transport", transport, "--upstream", endpoint(worker)};
        if (secure) { auto cert = tls(c); args.insert(args.end(), cert.begin(), cert.end()); }
        c.node("gateway", listen, args);
        const auto origin = std::string(transport)+"://localhost:"+std::to_string(listen);
        HttpRequest request{origin+"/message", "POST", std::string("\0\xffhello", 7),
            secure ? c.path("cert.pem").string() : ""};
        const auto result = http_request(request);
        require(result.status == 200, "HTTP adapter status"); equal(result.body, request.body);
        require(http(origin+"/missing", "GET", {}, request.ca_file).status == 404, "unknown HTTP route");
        request.max_response_bytes = 2;
        must_fail([&] { (void)http_request(request); });
        if (secure) {
            request.max_response_bytes = 1024; request.ca_file.clear();
            must_fail([&] { (void)http_request(request); });
        }
    }
    must_fail([] { (void)http_request({"file:///etc/hosts"}); });
}
void lua_lifecycle() {
    Context c; const auto script = c.path("service.lua"); write(script, echo);
    const auto listen = port();
    auto& worker = c.node("worker", listen, {"--script", script.string()});
    auto client = tcp(listen); equal(client->request("v1"), "v1");
    write(script, "return {message=function(p) return 'v2:'..p end}");
    worker.signal(SIGHUP); worker.ready("script reloaded");
    equal(client->request("x"), "v2:x");
    write(script, "invalid lua !!!");
    worker.signal(SIGHUP); worker.ready("reload rejected");
    equal(client->request("x"), "v2:x");
    write(script, "return {}");
    worker.signal(SIGHUP); worker.ready("service.message is required");
    equal(client->request("x"), "v2:x");
    write(c.path("bad.lua"), "return {message=function(p) if p=='error' then error('bad') end "
        "if p=='loop' then while true do end end return p end}");
    const auto bad = port();
    c.node("worker", bad, {"--script", c.path("bad.lua").string()});
    for (const auto& payload : {"error", "loop"}) must_fail([&] { (void)tcp(bad)->request(payload); });
    equal(tcp(bad)->request("healthy"), "healthy");
    // The actual exported Lua source is embedded at configure time, so installed
    // binaries can run this regression without depending on a source checkout.
    write(c.path("heap.lua"), "local H=assert(load([==["+std::string(heap_lua)+"]==]))()\n"
        "assert(MinHeap==nil); local h=H:new(3,function(a,b) return a<b end)\n"
        "h:push(3);h:push(1);h:push(2);assert(h:pop()==1 and h:pop()==2 and h:pop()==3)\n"
        "return {message=function(p)return p end}");
    const auto heap = port(); c.node("worker", heap, {"--script", c.path("heap.lua").string()});
    equal(tcp(heap)->request("heap"), "heap");
    worker.signal(SIGTERM); require(worker.wait() == 0, "worker graceful shutdown failed");
}
void lua_http() {
    Context c; const auto script = c.path("http.lua"); write(script, http_lua);
    const auto listen = port();
    auto& server = c.node("worker", listen, {"--transport", "http", "--script", script.string()});
    const auto origin = "http://"+endpoint(listen);
    auto response = http(origin+"/echo?x=1", "POST", std::string("\0\xff", 2), {}, true);
    require(response.status == 201 && response.headers.find("x-hook: after") != std::string::npos, "HTTP metadata/after hook");
    equal(response.body, std::string("\0\xff", 2));
    for (const auto& [path, status, body] : std::vector<std::tuple<std::string, long, std::string>>{
        {"/private",401,"denied"}, {"/replace",202,"replaced"}, {"/error",502,"handled"},
        {"/badheader",502,"handled"}, {"/fatal",500,"Internal Server Error"}, {"/missing",404,"missing"}}) {
        response = http(origin+path); require(response.status == status, "HTTP hook status"); equal(response.body, body);
    }
    write(script, "return {http=function(r)return{body='bad'}end,start=function()error('start rejected')end}");
    server.signal(SIGHUP); server.ready("reload rejected"); equal(http(origin+"/missing").body, "missing");
    write(script, "return {http=function(r)return{body='v2'}end,"
        "init=function()print('HOOK init v2')end,start=function()print('HOOK start v2')end,"
        "stop=function()print('HOOK stop v2')end,shutdown=function()print('HOOK shutdown v2')end}");
    server.signal(SIGHUP); server.ready("script reloaded"); equal(http(origin+"/").body, "v2");
    server.signal(SIGTERM); require(server.wait() == 0, "HTTP shutdown failed");
    std::istringstream log(server.output()); std::string line; std::vector<std::string> hooks;
    while (std::getline(log, line)) if (line.starts_with("HOOK ")) hooks.push_back(line);
    require(hooks == std::vector<std::string>{"HOOK init v1","HOOK start v1","HOOK init v2","HOOK start v2",
        "HOOK stop v1","HOOK shutdown v1","HOOK stop v2","HOOK shutdown v2"}, "Lua lifecycle order changed");
}
void cross_admission() {
    Context c; write(c.path("cross.lua"), "return {message=function(p,c)return c.realm..':'..c.group..'/'..c.room..':'..p end}");
    const auto s0=port(), s1=port(), ingress=port(), ingress2=port();
    auto& shard = c.node("shard", s0, {"--script",c.path("cross.lua").string(),"--max-room-sessions","2"});
    c.node("shard", s1, {"--script",c.path("cross.lua").string(),"--max-room-sessions","2"});
    std::vector<std::string> args{"--upstream",endpoint(s0),"--upstream",endpoint(s1),"--max-room-sessions","2"};
    c.node("cross", ingress, args); c.node("cross", ingress2, args);
    std::array<std::string, 2> rooms;
    for (unsigned i=0; rooms[0].empty() || rooms[1].empty(); ++i) {
        CrossContext route{"a","g","r"+std::to_string(i),1};
        rooms[cross_shard(route,2)] = route.room;
    }
    auto join = [](cli::Client& peer, const std::string& realm, const std::string& room) {
        return peer.request("MLC1 1 "+realm+" g "+room);
    };
    {
        auto a=tcp(ingress), b=tcp(ingress);
        equal(join(*a,"a",rooms[0]),"MLC1 OK 1"); equal(join(*b,"b",rooms[0]),"MLC1 OK 1");
        equal(a->request("x"),"a:g/"+rooms[0]+":x"); equal(b->request("x"),"b:g/"+rooms[0]+":x");
        for (auto entry : {ingress,ingress2}) {
            auto third=tcp(entry); equal(join(*third,"c",rooms[0]),"MLC1 ERR ROOM_FULL");
        }
        auto other=tcp(ingress); equal(join(*other,"c",rooms[1]),"MLC1 OK 1");
    }
    eventually([&] { auto released=tcp(ingress); return join(*released,"d",rooms[0])=="MLC1 OK 1"; });
    equal(tcp(ingress)->request("MLC1 2 a g r"),"MLC1 ERR EPOCH");
    equal(tcp(ingress)->request("invalid"),"MLC1 ERR INVALID");
    shard.signal(SIGTERM); require(shard.wait()==0,"shard shutdown failed");
    must_fail([&] { auto failed=tcp(ingress); (void)join(*failed,"a",rooms[0]); });
    auto healthy=tcp(ingress); equal(join(*healthy,"a",rooms[1]),"MLC1 OK 1");
    equal(healthy->request("ok"),"a:g/"+rooms[1]+":ok");
}
void scaffold_init() {
    Context c; const auto root=c.path("sample service");
    require(c.command({self().string(),"init",root.string()})==0,"initializer failed");
    for (const auto& name : {"scripts/echo.lua","scripts/http.lua","scripts/cross.lua",
        "config/instances/gateway.env","config/instances/gateway-ws.env","config/instances/gateway-wss.env",
        "ops/systemd/mistlane@.service"})
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
    {
        ScopedEnv real_binary("MISTLANE_BIN",self().string());
        const auto listen=port();
        write(root/"config/instances/worker.env","ROLE=worker\nLISTEN="+endpoint(listen)+"\nSCRIPT=scripts/echo.lua\nTRANSPORT=tcp\nUPSTREAMS=\nROUTE_EPOCH=\n");
        auto& node=c.spawn({(root/"ops/run-instance.sh").string(),"worker"});
        node.ready(); equal(tcp(listen)->request("template-path-check"),"template-path-check");
        node.signal(SIGTERM); require(node.wait()==0,"generated worker shutdown failed");
    }
    write(root/"keep.txt","preserve");
    require(c.command({self().string(),"init",root.string()})!=0,"initializer overwrote nonempty project");
    equal(read(root/"keep.txt"),"preserve");
}
void cli_bot() {
    Context c; write(c.path("echo.lua"),echo);
    const auto listen=port(); c.node("worker",listen,{"--script",c.path("echo.lua").string()});
    auto& bot=c.spawn({self().string(),"bot","--url","tcp://"+endpoint(listen),
        "--clients","4","--count","3","--payload","native-bot","--expect-echo"});
    require(bot.wait()==0 && bot.output().find("completed=12")!=std::string::npos,"bot request accounting failed");
    require(c.command({self().string(),"bot","--url","tcp://"+endpoint(listen),"--expect","mismatch"})!=0,
        "bot ignored response mismatch");
    require(c.command({self().string(),"bot","--count","0"})!=0,"bot accepted invalid count");
    require(c.command({self().string(),"test","--case","does-not-exist"})!=0,"unknown test returned success");
    // A listening socket that never replies validates a bounded receive timeout.
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor stalled(io,{boost::asio::ip::address_v4::loopback(),0});
    const auto stalled_port=stalled.local_endpoint().port();
    auto& timeout=c.spawn({self().string(),"bot","--url","tcp://"+endpoint(stalled_port),"--timeout-ms","100"});
    require(timeout.wait(3s)!=0,"bot receive deadline was not enforced");
}
void crashpad_startup(const fs::path& handler) {
    require(fs::is_regular_file(handler),"provide --crashpad-handler FILE for the crashpad suite");
    Context c; write(c.path("echo.lua"),echo); const auto listen=port();
    auto& node=c.node("worker",listen,{"--script",c.path("echo.lua").string(),"--crashpad-handler",handler.string(),
        "--crashpad-database",c.path("reports").string()});
    require(fs::is_directory(c.path("reports")) && !fs::is_empty(c.path("reports")),"Crashpad database missing");
    equal(tcp(listen)->request("crashpad"),"crashpad");
    node.signal(SIGTERM); require(node.wait()==0,"Crashpad-enabled worker shutdown failed");
}
}
