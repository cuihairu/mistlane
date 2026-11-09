// SPDX-License-Identifier: Apache-2.0
// Shared test fixtures for the split protocol / entity / coordinator suites.
// These network helpers were extracted from the former single test_cases.cpp
// translation unit: several suites use them, so they live in one common header
// rather than a private anonymous namespace per file. Free functions are
// marked inline so the definitions stay valid across translation units.
#pragma once
#include "test_support.hpp"
#include "test_fixtures.hpp"
#include "test_resources.hpp"
#include "mistlane/api.hpp"
#include "mistlane/cross.hpp"
#include "mistlane/entity_contract.hpp"
#include "mistlane/entity_dispatch.hpp"
#include "mistlane/entity_envelope.hpp"
#include "mistlane/entity_endpoint.hpp"
#include "mistlane/entity_codec.hpp"
#include "mistlane/entity_gateway.hpp"
#include "mistlane/entity_runtime.hpp"
#include "mistlane/entity_property.hpp"
#include "mistlane/entity_lua.hpp"
#include "mistlane/entity_sync.hpp"
#include "mistlane/entity_persist.hpp"
#include "mistlane/journal.hpp"
#include "mistlane/record.hpp"
#include "mistlane/migration.hpp"
#include "mistlane/http_client.hpp"
#include "mistlane/token_bucket.hpp"
#include "mistlane/transport.hpp"
#include <curl/curl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <malloc.h>
#include <numeric>
#include <future>
#include <sstream>
#include <memory>
#include <string_view>
#include <cstdint>
#include <algorithm>
#include <map>
#include <array>
#include <utility>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <openssl/evp.h>
#include <lua.hpp>
#include <sqlite3.h>

namespace mistlane::test {
inline constexpr std::string_view echo = "return {message=function(p) return p end}";
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
    std::string receive_line() {
        std::string result;
        for (;;) {
            char ch;
            auto n = ::recv(fd_, &ch, 1, 0);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "raw TCP closed or timed out");
            if (ch == '\n') break;
            result.push_back(ch);
        }
        return result;
    }
    bool closed() {
        char ch;
        auto n = ::recv(fd_, &ch, 1, 0);
        return n == 0 || (n < 0 && errno == ECONNRESET);
    }
    bool closed_now() {
        char ch;
        auto n = ::recv(fd_, &ch, 1, MSG_DONTWAIT);
        return n == 0 || (n < 0 && errno == ECONNRESET);
    }
    // True when at least one byte has arrived without blocking.
    bool pending() {
        int bytes = 0;
        return ::ioctl(fd_, FIONREAD, &bytes) == 0 && bytes > 0;
    }
private:
    int fd_;
};
struct Http {
    long status = 0;
    std::string body, headers;
};
inline Http http(const std::string& url, const std::string& method = "GET",
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
inline std::vector<std::string> tls(const Context& c) {
    return {"--cert", c.path("cert.pem").string(), "--key", c.path("key.pem").string()};
}
// Socket buffers can absorb megabytes before the server-side send bound sees
// anything. Size the flood past tcp_wmem + tcp_rmem plus the 2 MiB bound.
inline std::string flood_pushes() {
    const auto max_field = [](const char* path) -> std::size_t {
        std::ifstream in(path);
        if (!in) return 6ull*1024ull*1024ull; // linux default-ish fallback
        std::size_t value = 0, item = 0;
        while (in >> item) value = std::max(value, item);
        return value;
    };
    const auto bytes = max_field("/proc/sys/net/ipv4/tcp_wmem")
        + max_field("/proc/sys/net/ipv4/tcp_rmem") + 3*max_frame;
    const auto pushes = std::min<std::size_t>(64, (bytes + max_frame - 1)/max_frame);
    return "flood " + std::to_string(std::max<std::size_t>(pushes, 8));
}
// Minimal websocket client over a raw socket: enough for lifecycle checks that
// the request/response cli client cannot do (registry state, unread floods).
class WsRaw {
public:
    explicit WsRaw(unsigned port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        require(fd_ >= 0, "websocket socket failed");
        timeval timeout{5, 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_port = htons(static_cast<std::uint16_t>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(fd_); throw std::runtime_error("websocket connect failed");
        }
        send_all("GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
        std::string head;
        char ch;
        while (!head.ends_with("\r\n\r\n")) {
            const auto n = ::recv(fd_, &ch, 1, 0);
            require(n == 1, "handshake incomplete");
            head.push_back(ch);
            require(head.size() < 16384, "handshake too large");
        }
        require(head.starts_with("HTTP/1.1 101"), "upgrade rejected: "+head.substr(0, head.find('\r')));
    }
    ~WsRaw() { ::close(fd_); }
    WsRaw(const WsRaw&) = delete;
    WsRaw& operator=(const WsRaw&) = delete;
    void text(std::string_view payload) {
        require(payload.size() <= 125, "test frame too large");
        const char mask[4] = {'\x11', '\x22', '\x33', '\x44'};
        std::string frame{'\x81', static_cast<char>(0x80 | payload.size()), mask[0], mask[1], mask[2], mask[3]};
        for (std::size_t i = 0; i < payload.size(); ++i) frame.push_back(payload[i] ^ mask[i % 4]);
        send_all(frame);
    }
    void bin(std::string_view payload) {
        require(payload.size() <= 125, "test frame too large");
        const char mask[4] = {'\x11', '\x22', '\x33', '\x44'};
        std::string frame{'\x82', static_cast<char>(0x80 | payload.size()), mask[0], mask[1], mask[2], mask[3]};
        for (std::size_t i = 0; i < payload.size(); ++i) frame.push_back(payload[i] ^ mask[i % 4]);
        send_all(frame);
    }
    std::string read_text() {
        for (;;) {
            unsigned char head[2];
            recv_all(reinterpret_cast<char*>(head), 2);
            std::size_t length = head[1] & 0x7f;
            if (length == 126) { unsigned char ext[2]; recv_all(reinterpret_cast<char*>(ext), 2); length = (ext[0]<<8)|ext[1]; }
            else if (length == 127) {
                unsigned char ext[8]; recv_all(reinterpret_cast<char*>(ext), 8);
                length = 0;
                for (unsigned char byte : ext) length = (length<<8)|byte;
            }
            char mask[4] = {};
            const bool masked = head[1] & 0x80;
            if (masked) recv_all(mask, 4);
            std::string payload(length, '\0');
            if (length) recv_all(payload.data(), length);
            if (masked) for (std::size_t i = 0; i < length; ++i) payload[i] ^= mask[i % 4];
            const auto opcode = head[0] & 0x0f;
            if (opcode == 8) throw std::runtime_error("websocket closed by server");
            if (opcode == 1 || opcode == 2) return payload; // ping/pong/continuation skipped
        }
    }
    // Reads and discards until the server closes the session; returns the bytes seen.
    std::size_t drain_until_closed(std::chrono::seconds budget) {
        const auto deadline = std::chrono::steady_clock::now()+budget;
        std::size_t drained = 0;
        char buffer[65536];
        for (;;) {
            const auto n = ::recv(fd_, buffer, sizeof buffer, 0);
            if (n == 0 || (n < 0 && errno == ECONNRESET)) return drained;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("flood session was not closed");
                continue;
            }
            require(n > 0, "drain failed");
            drained += static_cast<std::size_t>(n);
        }
    }
private:
    void send_all(std::string_view data) {
        while (!data.empty()) {
            const auto n = ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "websocket send failed");
            data.remove_prefix(static_cast<std::size_t>(n));
        }
    }
    void recv_all(char* target, std::size_t size) {
        std::size_t offset = 0;
        while (offset < size) {
            const auto n = ::recv(fd_, target+offset, size-offset, 0);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "websocket closed while reading a frame");
            offset += static_cast<std::size_t>(n);
        }
    }
    int fd_;
};
}
