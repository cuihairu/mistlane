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
    auto& node=c.node("worker",worker,{"--script",c.path("redis.lua").string(),"--redis",endpoint(redis_port)});
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
