// SPDX-License-Identifier: Apache-2.0
#include "test_support.hpp"
#include "test_fixtures.hpp"
#include "mistlane/mysql_pool.hpp"
#include <boost/asio/cancel_after.hpp>
#include <openssl/rand.h>
#include <array>
#include <sstream>

namespace mistlane::test {
void redis_lua_api() {
    Context c; const auto redis_port=port(), worker=port();
    auto& redis=c.spawn({"redis-server","--bind","127.0.0.1","--port",std::to_string(redis_port),
        "--save","","--appendonly","no","--daemonize","no","--dir",c.temp.path.string()});
    eventually([&] {
        if (redis.poll()) throw std::runtime_error("temporary Redis exited:\n"+redis.output());
        try { auto connection=tcp(redis_port); return true; } catch (const std::exception&) { return false; }
    });
    write(c.path("redis.lua"),redis_lua);
    auto& node=c.node("gameServer",worker,{"--script",c.path("redis.lua").string(),"--redis",endpoint(redis_port)});
    auto client=tcp(worker);
    auto fields=[](const std::string& value) {
        std::vector<std::string> result; std::istringstream in(value); std::string item;
        while (std::getline(in,item,'|')) result.push_back(item);
        return result;
    };
    equal(client->request("setup"),"started");
    eventually([&] { auto value=fields(client->request("status")); return value.size()>2 && !value[2].empty(); });
    equal(client->request("read"),"reading");
    eventually([&] { return client->request("status").starts_with("ok|"); });
    equal(client->request("claim"),"claiming");
    eventually([&] { auto value=fields(client->request("status")); return value.size()>3 && value[0]=="ok" && value[3]=="1"; });
    equal(client->request("rank"),"ranking");
    eventually([&] { return client->request("status").find("7:1:7:1")!=std::string::npos; });
    equal(client->request("remove"),"removing");
    eventually([&] { return client->request("status").ends_with("|1"); });
    // Freeze Redis so a stream callback stays pending across a hot reload: the
    // callback owned by the replaced VM must be dropped, not run on closed state.
    redis.signal(SIGSTOP);
    equal(client->request("pend"),"pending");
    node.signal(SIGHUP);
    eventually([&] { return client->request("status")=="pending|pending|||"; });
    redis.signal(SIGCONT);
    eventually([&] { return client->request("status")=="pending|pending|||"; });
    // Kill Redis and restart it on the same port; the worker must reconnect and serve.
    redis.signal(SIGKILL); require(redis.wait()==137,"Redis did not exit on SIGKILL");
    auto& replacement=c.spawn({"redis-server","--bind","127.0.0.1","--port",std::to_string(redis_port),
        "--save","","--appendonly","no","--daemonize","no","--dir",c.temp.path.string()});
    eventually([&] {
        if (replacement.poll()) throw std::runtime_error("replacement Redis exited:\n"+replacement.output());
        try { auto connection=tcp(redis_port); return true; } catch (const std::exception&) { return false; }
    });
    equal(client->request("reconnect"),"probing");
    eventually([&] { auto probe=client->request("probe"); return !probe.empty() && probe.substr(0,6)!="error:"; });
    replacement.signal(SIGTERM); require(replacement.wait()==0,"replacement Redis shutdown failed");
}
void redis_tls() {
    Context c; const auto first=port(), second=port();
    auto certificate=[&](const char* name, const char* names) {
        require(c.command({"openssl","req","-x509","-newkey","rsa:2048","-nodes",
            "-keyout",c.path(std::string(name)+".key").string(),"-out",c.path(std::string(name)+".pem").string(),
            "-days","1","-subj","/CN=localhost","-addext",std::string("subjectAltName=")+names})==0,
            "test certificate generation failed");
    };
    // good: trust anchor with a DNS and an IP name; noip: valid chain, name only DNS;
    // other: unrelated trust anchor for a wrong-CA check.
    certificate("good","DNS:localhost,IP:127.0.0.1");
    certificate("noip","DNS:localhost");
    certificate("other","DNS:elsewhere.example");
    auto start_redis=[&](unsigned listen, const char* name) -> Child& {
        auto& server=c.spawn({"redis-server","--bind","127.0.0.1","--port","0",
            "--tls-port",std::to_string(listen),
            "--tls-cert-file",c.path(std::string(name)+".pem").string(),
            "--tls-key-file",c.path(std::string(name)+".key").string(),
            "--tls-ca-cert-file",c.path(std::string(name)+".pem").string(),
            "--tls-auth-clients","no",
            "--save","","--appendonly","no","--daemonize","no","--dir",c.temp.path.string()});
        eventually([&] {
            if (server.poll()) throw std::runtime_error(std::string(name)+" Redis exited:\n"+server.output());
            try { auto connection=tcp(listen); return true; } catch (const std::exception&) { return false; }
        });
        return server;
    };
    auto& primary=start_redis(first,"good");
    auto& nameless=start_redis(second,"noip");
    write(c.path("redis.lua"),redis_lua);
    auto worker=[&](unsigned redis_listen, const std::string& host) {
        const unsigned own=port();
        auto& child=c.node("gameServer",own,{"--script",c.path("redis.lua").string(),
            "--redis",host+":"+std::to_string(redis_listen)});
        return std::pair<Child&,unsigned>{child,own};
    };
    auto fields=[](const std::string& value) {
        std::vector<std::string> result; std::istringstream in(value); std::string item;
        while (std::getline(in,item,'|')) result.push_back(item);
        return result;
    };
    auto expect_ok=[&](std::pair<Child&,unsigned> launched) {
        auto client=tcp(launched.second);
        equal(client->request("setup"),"started");
        eventually([&] { auto value=fields(client->request("status")); return value.size()>2 && !value[2].empty(); });
        equal(client->request("read"),"reading");
        eventually([&] { return client->request("status").starts_with("ok|"); });
    };
    auto expect_rejected=[&](std::pair<Child&,unsigned> launched, const std::string& failure) {
        auto client=tcp(launched.second);
        equal(client->request("setup"),"started");
        std::this_thread::sleep_for(2s);
        auto value=fields(client->request("status"));
        require(value.size()<=2 || value[2].empty(),failure+" was bypassed");
        require(!launched.first.poll(),"worker exited while Redis TLS was being verified");
        require(launched.first.output().find("SSL")!=std::string::npos ||
            launched.first.output().find("certificate")!=std::string::npos ||
            launched.first.output().find("handshake")!=std::string::npos,
            "worker log lacks a TLS failure record:\n"+launched.first.output());
    };
    {
        ScopedEnv tls("MISTLANE_REDIS_TLS","on");
        ScopedEnv ca("MISTLANE_REDIS_CA",c.path("good.pem").string());
        expect_ok(worker(first,"localhost"));
        expect_ok(worker(first,"127.0.0.1"));
    }
    {
        ScopedEnv tls("MISTLANE_REDIS_TLS","on");
        ScopedEnv ca("MISTLANE_REDIS_CA",c.path("other.pem").string());
        expect_rejected(worker(first,"localhost"),"chain verification failure");
    }
    {
        ScopedEnv tls("MISTLANE_REDIS_TLS","on");
        ScopedEnv ca("MISTLANE_REDIS_CA",c.path("noip.pem").string());
        expect_rejected(worker(second,"127.0.0.1"),"hostname mismatch");
    }
    primary.signal(SIGTERM); require(primary.wait()==0,"primary Redis shutdown failed");
    nameless.signal(SIGTERM); require(nameless.wait()==0,"second Redis shutdown failed");
}
namespace {
std::string random_password() {
    std::array<unsigned char,24> bytes{};
    require(RAND_bytes(bytes.data(),static_cast<int>(bytes.size()))==1,"random password generation failed");
    static constexpr char hex[]="0123456789abcdef";
    std::string result;
    for (auto ch:bytes) { result.push_back(hex[ch>>4]);result.push_back(hex[ch&15]); }
    return result;
}
boost::asio::awaitable<void> query_pool(boost::mysql::connection_pool& pool) {
    for (int i=0;i<3;++i) {
        auto connection=co_await pool.async_get_connection(boost::asio::cancel_after(5s));
        boost::mysql::results result;
        const std::string parameter="quote' and backslash\\; --";
        co_await connection->async_execute(boost::mysql::with_params("SELECT {}, {}",42,parameter),
            result,boost::asio::cancel_after(5s));
        require(result.rows().at(0).at(0).as_int64()==42 && result.rows().at(0).at(1).as_string()==parameter,
            "parameterized SQL returned unexpected values");
    }
}
void check_pool(const MysqlConfig& config) {
    boost::asio::io_context io;
    auto pool=make_mysql_pool(io,config);
    pool->async_run(boost::asio::detached);
    std::exception_ptr failure;
    boost::asio::co_spawn(io,query_pool(*pool),[&](std::exception_ptr error) { failure=error;pool->cancel(); });
    io.run();
    if (failure) std::rethrow_exception(failure);
}
}
void mysql_pool() {
    Context c; c.cert(); const auto listen=port();
    require(c.command({"mariadb-install-db","--no-defaults","--datadir="+c.path("data").string(),
        "--auth-root-authentication-method=normal","--skip-test-db"})==0,"temporary MariaDB initialization failed");
    const auto socket=c.path("db.sock");
    auto& server=c.spawn({"mysqld","--no-defaults","--datadir="+c.path("data").string(),"--socket="+socket.string(),
        "--pid-file="+c.path("server.pid").string(),"--bind-address=127.0.0.1","--port="+std::to_string(listen),
        "--ssl-cert="+c.path("cert.pem").string(),"--ssl-key="+c.path("key.pem").string(),
        "--ssl-ca="+c.path("cert.pem").string(),"--skip-name-resolve"});
    const auto password=random_password();
    auto sql=[&](std::string_view statement) {
        write(c.path("query.sql"),statement);
        return c.command({"mysql","--no-defaults","--protocol=socket","--socket="+socket.string(),"-u","root"},c.path("query.sql"));
    };
    eventually([&] {
        if (server.poll()) throw std::runtime_error("temporary MariaDB exited:\n"+server.output());
        return sql("SELECT 1;")==0;
    },15s);
    require(sql("CREATE USER 'mistlane_test'@'127.0.0.1' IDENTIFIED BY '"+password+"';")==0,"test DB user creation failed");
    MysqlConfig config; config.max_size=1;config.initial_size=1;
    config.host="127.0.0.1";config.port=static_cast<unsigned short>(listen);
    config.username="mistlane_test";config.password=password;config.tls=false;
    check_pool(config);
    ScopedEnv trust("SSL_CERT_FILE",c.path("cert.pem").string());
    config.host="localhost";config.tls=true;check_pool(config);
    config.password=random_password();must_fail([&] {check_pool(config);});
    server.signal(SIGTERM); require(server.wait()==0,"MariaDB shutdown failed");
}
}
