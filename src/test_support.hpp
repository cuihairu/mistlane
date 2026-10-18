// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "cli_client.hpp"
#include <boost/asio.hpp>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <set>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
extern char** environ;

namespace mistlane::test {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
inline bool keep_temp = false;
inline fs::path self() { return fs::canonical("/proc/self/exe"); }
inline void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
inline void equal(const std::string& actual, const std::string& expected) {
    require(actual == expected, "response mismatch: expected " + std::to_string(expected.size()) +
        " bytes, got " + std::to_string(actual.size()));
}
template<class Work> void must_fail(Work work) {
    bool failed = false;
    try { work(); } catch (const std::exception&) { failed = true; }
    require(failed, "operation unexpectedly succeeded");
}
inline std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
inline void write(const fs::path& path, std::string_view data) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
    require(stream.good(), "cannot write fixture");
}
struct Temp {
    fs::path path;
    Temp() {
        std::string pattern = (fs::temp_directory_path() / "mistlane-test-XXXXXX").string();
        auto* result = ::mkdtemp(pattern.data());
        if (!result) throw std::runtime_error("mkdtemp failed");
        path = result;
    }
    ~Temp() {
        if (keep_temp) std::cout << "  fixtures: " << path << '\n';
        else { std::error_code ignored; fs::remove_all(path, ignored); }
    }
};
class Child {
public:
    Child(std::vector<std::string> args, fs::path log, const fs::path& input = {}) : log_(std::move(log)) {
        fs::create_directories(log_.parent_path());
        posix_spawn_file_actions_t actions;
        posix_spawnattr_t attr;
        require(posix_spawn_file_actions_init(&actions) == 0, "spawn actions init failed");
        require(posix_spawnattr_init(&attr) == 0, "spawn attributes init failed");
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attr, 0);
        const auto stdin_path = input.empty() ? fs::path("/dev/null") : input;
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, stdin_path.c_str(), O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log_.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0600);
        posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        const auto error = posix_spawnp(&pid_, argv.front(), &actions, &attr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attr);
        if (error) throw std::runtime_error("cannot execute " + args.front() + ": " + std::strerror(error));
        group_ = pid_;
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child() {
        if (pid_ > 0) {
            ::kill(-group_, SIGTERM);
            for (int i=0; i<100 && !poll(); ++i) std::this_thread::sleep_for(20ms);
            if (pid_ > 0) {
                ::kill(-group_, SIGKILL);
                int status;
                while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
                pid_ = 0;
            }
        }
        // Reap/terminate utility descendants, including optional crash handlers.
        if (group_ > 0 && reap_) ::kill(-group_, SIGKILL);
    }
    std::optional<int> poll() {
        if (result_) return result_;
        int status = 0;
        const auto n = ::waitpid(pid_, &status, WNOHANG);
        if (n == 0 || (n < 0 && errno == EINTR)) return {};
        if (n < 0) throw std::runtime_error("waitpid failed");
        if (group_ > 0 && reap_) ::kill(-group_, SIGKILL);
        group_ = 0;
        pid_ = 0;
        result_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        return result_;
    }
    int wait(std::chrono::seconds timeout = 10s) {
        const auto end = std::chrono::steady_clock::now()+timeout;
        do {
            if (auto result = poll()) return *result;
            std::this_thread::sleep_for(20ms);
        } while (std::chrono::steady_clock::now() < end);
        throw std::runtime_error("child process timeout:\n" + output());
    }
    void signal(int value) { require(pid_ > 0 && ::kill(pid_, value) == 0, "child signal failed"); }
    void ready(const std::string& text = "ready ") {
        const auto end = std::chrono::steady_clock::now()+8s;
        do {
            if (output().find(text) != std::string::npos) return;
            if (poll()) throw std::runtime_error("child exited before " + text + ":\n" + output());
            std::this_thread::sleep_for(20ms);
        } while (std::chrono::steady_clock::now() < end);
        throw std::runtime_error("child readiness timeout:\n" + output());
    }
    std::string output() const { return read(log_); }
    // Launcher-style children deliberately leave descendants (daemons) behind;
    // opt out of the group kill for those.
    void reap_descendants(bool enabled) { reap_ = enabled; }
    // Process group of the spawned child, 0 once reaped; tests use it to clean
    // up descendants that deliberately outlive their leader.
    pid_t group() const { return group_; }
private:
    pid_t pid_ = 0, group_ = 0; bool reap_ = true;
    std::optional<int> result_;
    fs::path log_;
};
inline unsigned port() {
    static std::set<unsigned> used;
    for (;;) {
        boost::asio::io_context io;
        boost::asio::ip::tcp::acceptor socket(io, {boost::asio::ip::address_v4::loopback(), 0});
        const auto value = socket.local_endpoint().port();
        if (used.insert(value).second) return value;
    }
}
inline std::string endpoint(unsigned value) { return "127.0.0.1:" + std::to_string(value); }
inline std::unique_ptr<cli::Client> tcp(unsigned value) {
    return cli::make_client({"tcp://" + endpoint(value)});
}
struct Context {
    Temp temp;
    unsigned logs = 0;
    std::vector<std::unique_ptr<Child>> children;
    fs::path path(std::string_view name) const { return temp.path / name; }
    Child& spawn(std::vector<std::string> args, const fs::path& input = {}) {
        auto child = std::make_unique<Child>(std::move(args), path("process-"+std::to_string(++logs)+".log"), input);
        children.push_back(std::move(child));
        return *children.back();
    }
    Child& node(std::string role, unsigned listen, std::vector<std::string> options) {
        std::vector<std::string> args{self().string(), "--role", std::move(role), "--listen", endpoint(listen)};
        args.insert(args.end(), options.begin(), options.end());
        auto& child = spawn(std::move(args)); child.ready(); return child;
    }
    int command(std::vector<std::string> args, const fs::path& input = {}) {
        return spawn(std::move(args), input).wait();
    }
    void cert() {
        require(command({"openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout",
            path("key.pem").string(), "-out", path("cert.pem").string(), "-days", "1", "-subj",
            "/CN=localhost", "-addext", "subjectAltName=DNS:localhost"}) == 0, "test certificate generation failed");
    }
};
template<class Work> void eventually(Work work, std::chrono::seconds timeout = 5s,
    const std::string& what = "condition did not converge before deadline") {
    auto end = std::chrono::steady_clock::now()+timeout;
    do {
        if (work()) return;
        std::this_thread::sleep_for(30ms);
    } while (std::chrono::steady_clock::now() < end);
    throw std::runtime_error(what);
}
struct ScopedEnv {
    std::string name;
    std::optional<std::string> previous;
    ScopedEnv(std::string key, const std::string& value) : name(std::move(key)) {
        if (auto* old = std::getenv(name.c_str())) previous = old;
        require(::setenv(name.c_str(), value.c_str(), 1) == 0, "setenv failed");
    }
    ~ScopedEnv() {
        if (previous) ::setenv(name.c_str(), previous->c_str(), 1);
        else ::unsetenv(name.c_str());
    }
};
}
