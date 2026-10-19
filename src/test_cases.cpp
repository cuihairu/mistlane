// SPDX-License-Identifier: Apache-2.0
#include "test_support.hpp"
#include "test_fixtures.hpp"
#include "test_resources.hpp"
#include "mistlane/cross.hpp"
#include "mistlane/entity_contract.hpp"
#include "mistlane/record.hpp"
#include "mistlane/http_client.hpp"
#include "mistlane/token_bucket.hpp"
#include "mistlane/transport.hpp" // max_frame bounds the flood sizing
#include <curl/curl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <future>
#include <sstream>
#include <netinet/in.h>
#include <sys/ioctl.h>

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
// Socket buffers can absorb megabytes before the server-side send bound sees
// anything. Size the flood past tcp_wmem + tcp_rmem plus the 2 MiB bound.
std::string flood_pushes() {
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
void core_contracts() {
    TokenBucket bucket(2, 0.000001);
    require(bucket.consume() && bucket.consume() && !bucket.consume(), "token budget is not enforced");
    must_fail([] { TokenBucket invalid(0, 1); });
    auto context = cross_context("MLC1 1 realm group room");
    equal(context.room, "room");
    require(context.character.empty() && !context.generation && context.return_room.empty(),
        "anonymous join gained participant fields");
    must_fail([] { (void)cross_context("MLC1 0 realm group room"); });
    must_fail([] { (void)cross_context("MLC1 1 realm group room extra"); });
    // A participant declares character, hosting generation and return route
    // together; every partial declaration is refused.
    auto participant = cross_context("MLC1 1 realm group room ch hero-7 gen 12 ret home arena r1");
    equal(participant.character, "hero-7");
    require(participant.generation == 12, "hosting generation lost");
    equal(participant.return_realm+"/"+participant.return_group+"/"+participant.return_room, "home/arena/r1");
    for (const auto& partial : std::vector<std::string>{
        "MLC1 1 realm group room ch hero",
        "MLC1 1 realm group room ch hero gen 12",
        "MLC1 1 realm group room ch hero gen 12 ret home arena",
        "MLC1 1 realm group room gen 12",
        "MLC1 1 realm group room ret home arena r1",
        "MLC1 1 realm group room ch hero gen 0 ret home arena r1",
        "MLC1 1 realm group room ch hero gen 12 ret home arena r1 extra",
        "MLC1 1 realm group room ch her!o gen 12 ret home arena r1"})
        must_fail([&] { (void)cross_context(partial); });
    // MLD1 record grammar: load and submit both parse, and every partial or
    // out-of-shape declaration is refused.
    auto load = parse_record("MLD1 LOAD home hero-7");
    equal(load.realm+"/"+load.character, "home/hero-7");
    require(load.type == RecordRequest::Type::Load, "record load mislabeled");
    auto submit = parse_record("MLD1 SUBMIT home hero-7 gen 12 seq 3 {\"hp\":5}");
    require(submit.type == RecordRequest::Type::Submit && submit.generation == 12
            && submit.sequence == 3, "record submit header lost");
    equal(submit.payload, "{\"hp\":5}");
    equal(parse_record("MLD1 SUBMIT home hero-7 gen 12 seq 3").payload, "");
    for (const auto& broken : std::vector<std::string>{
        "MLD1", "MLD1 LOAD home", "MLD1 LOAD home hero extra",
        "MLD1 SUBMIT home hero gen 12", "MLD1 SUBMIT home hero gen 12 seq",
        "MLD1 SUBMIT home hero gen 0 seq 1", "MLD1 SUBMIT home hero gen 12 seq 0",
        "MLD1 SUBMIT home hero gen x seq 1", "MLD1 SUBMIT home hero gen 12 seq 99999999999",
        "MLD1 SUBMIT ho/me hero gen 12 seq 1", "MLD1 SUBMIT home her!o gen 12 seq 1",
        std::string("MLD1 SUBMIT home hero gen 12 seq 1 ")+std::string(1024*1024+1, 'x')})
        must_fail([&] { (void)parse_record(broken); });
    // The home side owns the fence and the sequence discipline.
    RecordStore records;
    std::string_view result;
    equal(records.handle("MLD1 LOAD home hero", result), "MLD1 EMPTY");
    require(result == "empty", "record load mislabeled as " + std::string(result));
    equal(records.handle("MLD1 SUBMIT home hero gen 5 seq 1 hp=10", result), "MLD1 RECEIVED gen 5 seq 1");
    require(result == "accepted", "first submit was not accepted");
    equal(records.handle("MLD1 LOAD home hero", result), "MLD1 STATE gen 5 seq 1 hp=10");
    require(result == "loaded", "record load after submit mislabeled");
    // A resend dedups to a second receipt and never overwrites the record.
    equal(records.handle("MLD1 SUBMIT home hero gen 5 seq 1 hp=99", result), "MLD1 RECEIVED gen 5 seq 1");
    require(result == "duplicate", "resend was not deduplicated");
    equal(records.handle("MLD1 LOAD home hero", result), "MLD1 STATE gen 5 seq 1 hp=10");
    equal(records.handle("MLD1 SUBMIT home hero gen 5 seq 3 gap", result), "MLD1 ERR SEQ seq 2");
    require(result == "seq", "sequence gap mislabeled");
    equal(records.handle("MLD1 SUBMIT home hero gen 4 seq 2 late", result), "MLD1 ERR STALE gen 5");
    require(result == "stale", "generation fence mislabeled");
    equal(records.handle("MLD1 SUBMIT home hero gen 6 seq 2 taken", result), "MLD1 RECEIVED gen 6 seq 2");
    equal(records.handle("MLD1 SUBMIT home hero gen 7 seq 9 jump", result), "MLD1 ERR SEQ seq 3");
    equal(records.handle("MLD1 SUBMIT home hero gen 7 seq 3", result), "MLD1 RECEIVED gen 7 seq 3");
    equal(records.handle("MLD1 LOAD home hero", result), "MLD1 STATE gen 7 seq 3");
    // Records are independent per character, and a fresh record starts at one.
    equal(records.handle("MLD1 SUBMIT home other gen 1 seq 2", result), "MLD1 ERR SEQ seq 1");
    equal(records.handle("MLD1 NOPE", result), "MLD1 ERR INVALID");
    require(result == "invalid", "malformed frame mislabeled");
    // Entity contract prototype: the design page's example parses, resolves
    // and digests; every listed semantic rule refuses its violation.
    const auto document = R"(<Contracts>
  <Types>
    <Item><itemId id="0" type="UINT32"/><count id="1" type="UINT32" default="1"/></Item>
    <Inventory><capacity id="0" type="UINT16" default="20"/><items id="1" type="ARRAY" of="Item"/></Inventory>
  </Types>
  <Entity name="Avatar" id="1">
    <Properties>
      <level id="1" type="UINT16" default="1" flags="OWN_CLIENT" persistent="true"/>
      <hp id="2" type="UINT32" default="100" flags="ALL_CLIENTS"/>
      <gold id="3" type="INT64" default="-5" flags="OWN_CLIENT" persistent="true"/>
      <inventory id="4" type="Inventory" flags="OWN_CLIENT" persistent="true"/>
    </Properties>
    <ServerMethods>
      <Method name="attack" id="1" mode="notify" caller="owner">
        <Arg name="target" id="0" type="EntityRef"/>
      </Method>
      <Method name="getProfile" id="2" mode="request" caller="server">
        <Returns><Field name="level" id="0" type="UINT16"/></Returns>
      </Method>
    </ServerMethods>
    <ClientMethods>
      <Method name="onReward" id="1" mode="notify" audience="owner">
        <Arg name="itemId" id="0" type="UINT32"/><Arg name="count" id="1" type="UINT32"/>
      </Method>
    </ClientMethods>
  </Entity>
</Contracts>)";
    const auto document_with = [&](std::string_view from, std::string_view to) {
        auto text = std::string(document);
        const auto at = text.find(from);
        require(at != std::string::npos, std::string("test edit missed its anchor: ")+std::string(from));
        text.replace(at, from.size(), to);
        return text;
    };
    EntityContract contract(document);
    require(contract.structs().size() == 2 && contract.entities().size() == 1,
            "contract model lost a declaration");
    const auto& avatar = contract.entities().front();
    equal(avatar.properties[3].type, "Inventory");
    require(avatar.properties[3].persistent, "persistent attribute lost");
    require(avatar.properties[1].flags == "ALL_CLIENTS" && !avatar.properties[1].persistent,
            "explicit flags lost");
    require(avatar.properties[2].default_value == "-5", "negative INT64 default lost");
    require(avatar.server_methods[1].returns.size() == 1, "request returns lost");
    require(contract.structs().at("Item").front().id == 0, "struct field id lost");
    // The dump is the digest input and keeps declaration order and explicit ids.
    require(contract.describe().find(" prop {1 level UINT16 default 1} OWN_CLIENT persistent") != std::string::npos &&
            contract.describe().find(" method server 2 getProfile request server") != std::string::npos,
            "canonical dump lost the resolved model: "+contract.describe());
    require(contract.digest() != 0, "digest is computed");
    require(EntityContract(document).digest() == contract.digest(), "digest is not reproducible");
    for (const auto& broken : std::vector<std::string>{
        // ids: explicit, unique, never recomputed
        document_with("<hp id=\"2\" type=\"UINT32\" default=\"100\" flags=\"ALL_CLIENTS\"/>",
                      "<hp id=\"1\" type=\"UINT32\" default=\"100\" flags=\"ALL_CLIENTS\"/>"),
        // closed vocabularies
        document_with("flags=\"ALL_CLIENTS\"", "flags=\"EVERYONE\""),
        document_with("type=\"UINT32\" default=\"100\" flags", "type=\"UINT8\" default=\"100\" flags"),
        document_with("caller=\"server\"", "caller=\"anyone\""),
        document_with("mode=\"request\" caller=\"server\"",
                      "mode=\"fire-and-forget\" caller=\"server\""),
        // typed defaults
        document_with("default=\"100\" flags", "default=\"4294967296\" flags"),
        document_with("default=\"-5\"", "default=\"false\""),
        // arrays and struct references
        document_with("type=\"ARRAY\" of=\"Item\"", "type=\"ARRAY\""),
        document_with("type=\"ARRAY\" of=\"Item\"", "type=\"ARRAY\" of=\"Warehouse\""),
        // methods
        document_with("<Method name=\"getProfile\" id=\"2\" mode=\"request\" caller=\"server\">\n"
                      "        <Returns><Field name=\"level\" id=\"0\" type=\"UINT16\"/></Returns>\n"
                      "      </Method>",
                      "<Method name=\"getProfile\" id=\"2\" mode=\"request\" caller=\"server\"/>"),
        document_with("mode=\"notify\" caller=\"owner\">\n"
                      "        <Arg name=\"target\" id=\"0\" type=\"EntityRef\"/>\n"
                      "      </Method>",
                      "mode=\"notify\" caller=\"owner\">\n"
                      "        <Returns><Field name=\"x\" id=\"0\" type=\"UINT16\"/></Returns>\n"
                      "      </Method>"),
        document_with("audience=\"owner\"", "audience=\"server\""),
        "<Entity name=\"Avatar\" id=\"1\"><ServerMethods>"
        "<Method name=\"attack\" id=\"1\" mode=\"request\" caller=\"owner\"/></ServerMethods></Entity>",
        // xml subset: external entities and CDATA are refused outright
        std::string("<!DOCTYPE Contracts [<!ENTITY xxe \"evil\">]><Contracts></Contracts>"),
        std::string("<Contracts><Entity name=\"A\" id=\"1\"/><![CDATA[raw]]></Contracts>")})
        must_fail([&] { (void)EntityContract(broken); });
    // Duplicate member names across groups are refused, even across method
    // sides; the same method id on both sides is fine.
    EntityContract mirrored(std::string(
        "<Contracts><Entity name=\"A\" id=\"1\">"
        "<Properties><x id=\"1\" type=\"UINT32\"/></Properties>"
        "<ServerMethods><Method name=\"ping\" id=\"1\" mode=\"notify\" caller=\"server\"/></ServerMethods>"
        "<ClientMethods><Method name=\"pong\" id=\"1\" mode=\"notify\" audience=\"owner\"/></ClientMethods>"
        "</Entity></Contracts>"));
    require(mirrored.entities().front().client_methods.front().id == 1, "side-local method id lost");
    must_fail([&] { (void)EntityContract(std::string(
        "<Contracts><Entity name=\"A\" id=\"1\">"
        "<ServerMethods><Method name=\"ping\" id=\"1\" mode=\"notify\" caller=\"server\"/></ServerMethods>"
        "<ClientMethods><Method name=\"ping\" id=\"1\" mode=\"notify\" audience=\"owner\"/></ClientMethods>"
        "</Entity></Contracts>")); });
    must_fail([&] { (void)EntityContract(std::string(
        "<Contracts><Entity name=\"A\" id=\"1\">"
        "<Properties><x id=\"1\" type=\"UINT32\"/></Properties>"
        "<ServerMethods><Method name=\"x\" id=\"1\" mode=\"notify\" caller=\"server\"/></ServerMethods>"
        "</Entity></Contracts>")); });
    // Recursive struct chains are refused; acyclic chains resolve.
    must_fail([&] { (void)EntityContract(std::string(
        "<Contracts><Types>"
        "<A><next id=\"0\" type=\"ARRAY\" of=\"B\"/></A>"
        "<B><prev id=\"0\" type=\"A\"/></B>"
        "</Types></Contracts>")); });
    EntityContract chain(std::string(
        "<Contracts><Types>"
        "<A><b id=\"0\" type=\"B\"/></A>"
        "<B><leaf id=\"0\" type=\"UINT16\"/></B>"
        "</Types><Entity name=\"E\" id=\"1\">"
        "<Properties><root id=\"1\" type=\"A\"/></Properties>"
        "</Entity></Contracts>"));
    require(chain.digest() != 0, "digest is computed");
    require(chain.describe().find("struct A") != std::string::npos &&
            chain.describe().find(" prop {1 root A} SERVER_ONLY") != std::string::npos,
            "canonical dump lost the resolved model: "+chain.describe());
    // Reserved ids keep retired tags forever, and the schema/lock emit them.
    EntityContract locked(document_with("</Entity>", "<Reserved>"
            "<Property id=\"5\"/><ServerMethod id=\"9\"/><ClientMethod id=\"7\"/></Reserved></Entity>"));
    const auto& retained = locked.entities().front();
    require(retained.reserved_properties.size() == 1 && retained.reserved_properties[0] == 5 &&
            retained.reserved_server_methods.size() == 1 && retained.reserved_server_methods[0] == 9 &&
            retained.reserved_client_methods.size() == 1 && retained.reserved_client_methods[0] == 7,
            "reserved ids lost: "+locked.describe());
    require(locked.digest() != contract.digest(), "reserving an id does not change the model");
    for (const auto& broken : std::vector<std::string>{
        document_with("</Entity>", "<Reserved><Property id=\"1\"/></Reserved></Entity>"),
        document_with("</Entity>", "<Reserved><Property id=\"5\"/><Property id=\"5\"/></Reserved></Entity>"),
        document_with("</Entity>", "<Reserved><Property id=\"5\" note=\"retired\"/></Reserved></Entity>"),
        document_with("</Entity>", "<Reserved><Property/></Reserved></Entity>"),
        document_with("</Entity>", "<Reserved><Thing id=\"5\"/></Reserved></Entity>"),
        document_with("</Entity>", "<Reserved><Property id=\"7\"/></Reserved></Entity>"
        "<Reserved><Property id=\"5\"/></Reserved></Entity>")})
        must_fail([&] { (void)EntityContract(broken); });
    // The generated schema is well-formed XML and pins each member's id and type.
    const auto schema = locked.xsd();
    require(EntityContract::well_formed(schema), "generated schema is not well-formed: "+schema);
    require(EntityContract(document_with("</Entity>", "<Reserved><Property id=\"5\"/>"
            "<ServerMethod id=\"9\"/><ClientMethod id=\"7\"/></Reserved></Entity>")).xsd() == schema,
            "schema is not reproducible");
    const auto block = [&](const std::string& needle) {
        const auto at = schema.find(needle);
        require(at != std::string::npos, std::string("schema is missing ")+needle);
        const auto end = schema.find("</xs:element>", at);
        require(end != std::string::npos, "schema block is unterminated");
        return schema.substr(at, end-at);
    };
    const auto hp = block("<xs:element name=\"hp\" ");
    require(hp.find("<xs:enumeration xs:value=\"2\"/>") != std::string::npos &&
            hp.find("<xs:enumeration xs:value=\"UINT32\"/>") != std::string::npos &&
            hp.find("type=\"ReplicationFlags\"") != std::string::npos,
            "the schema does not pin a property's id, type and flags: "+hp);
    require(block("<xs:element name=\"Property\" ").find("<xs:enumeration xs:value=\"5\"/>") !=
            std::string::npos, "the schema does not pin a reserved property id");
    require(block("<xs:element name=\"ClientMethod\" ").find("<xs:enumeration xs:value=\"7\"/>") !=
            std::string::npos, "the schema does not pin a reserved client method id");
    require(block("<xs:element name=\"Field\" ").find("<xs:enumeration xs:value=\"UINT16\"/>") !=
            std::string::npos, "the schema lost the request's typed return");
    const auto attr = [&](const char* key, const std::string& value) {
        return std::string("<xs:attribute name=\"")+key+"\" use=\"required\"><xs:simpleType>"
            "<xs:restriction base=\"xs:string\"><xs:enumeration xs:value=\""+value+
            "\"/></xs:restriction></xs:simpleType></xs:attribute>";
    };
    require(schema.find(attr("mode", "request")) != std::string::npos &&
            schema.find(attr("mode", "notify")) != std::string::npos,
            "the schema does not pin the declared modes");
    require(schema.find(attr("caller", "owner")) != std::string::npos &&
            schema.find(attr("caller", "server")) != std::string::npos &&
            schema.find(attr("audience", "owner")) != std::string::npos,
            "the schema mixed up caller and audience");
    require(schema.find(attr("name", "attack")) != std::string::npos &&
            schema.find(attr("id", "1")) != std::string::npos,
            "the schema does not pin member names and ids");
    require(schema.find("<xs:enumeration xs:value=\"Inventory\"/>") != std::string::npos &&
            schema.find("<xs:element name=\"Inventory\"") != std::string::npos &&
            schema.find("name=\"capacity\"") != std::string::npos,
            "the schema lost the declared structs");
    require(contract.xsd().find("name=\"Reserved\"") == std::string::npos,
            "an empty Reserved group must not appear in the schema");
    require(!EntityContract::well_formed("<Contracts><A><B/></Contracts>"),
            "well-formedness accepted a mismatched close tag");
    require(!EntityContract::well_formed("<Contracts/></Contracts>"),
            "well-formedness accepted trailing content");
    // The release lock keeps every assignment a published version made.
    const auto lock_text = locked.lock();
    require(lock_text.find("# mistlane entity contract lock\n# digest ") != std::string::npos &&
            lock_text.find("struct Item") != std::string::npos &&
            lock_text.find("entity Avatar 1") != std::string::npos &&
            lock_text.find(" prop Avatar 3 gold INT64") != std::string::npos &&
            lock_text.find(" method Avatar server 2 getProfile") != std::string::npos &&
            lock_text.find(" method Avatar client 1 onReward") != std::string::npos &&
            lock_text.find(" reserved Avatar property 5") != std::string::npos &&
            lock_text.find(" reserved Avatar server 9") != std::string::npos &&
            lock_text.find(" reserved Avatar client 7") != std::string::npos,
            "release lock lost assignments: "+lock_text);
    require(EntityContract(document_with("</Entity>", "<Reserved><Property id=\"5\"/>"
            "<ServerMethod id=\"9\"/><ClientMethod id=\"7\"/></Reserved></Entity>")).lock() == lock_text,
            "release lock is not reproducible");
    CrossAdmission admission(1, 1, 1, 1);
    int a = 0, b = 0;
    require(admission.acquire(&a, context, 0).empty(), "first admission failed");
    equal(admission.acquire(&b, context, 0), "ROOM_FULL");
    admission.release(&a);
    require(admission.acquire(&b, context, 0).empty(), "admission did not recover on release");
    Context c;
    for (auto args : std::vector<std::vector<std::string>>{
        {"--role", "invalid"}, {"--listen", "127.0.0.1:65536"}, {"--role", "gameServer"},
        {"--role", "gateway"}, {"--max-connections", "0"}, {"--role", "gameServer", "--transport", "wss"}}) {
        args.insert(args.begin(), self().string());
        require(c.command(args) != 0, "invalid CLI options accepted");
    }
}
void role_names() {
    Context c;
    write(c.path("echo.lua"), echo);
    for (const auto& old : {"worker", "router", "cross", "shard"}) {
        auto& child = c.spawn({self().string(), "--role", old, "--script", c.path("echo.lua").string(),
                              "--upstream", endpoint(port())});
        require(child.wait() != 0, "removed role accepted");
        require(child.output().find("unknown role") != std::string::npos, "removed role was not rejected by name");
    }
    // login and data run as script services: a script-less launch is refused,
    // and with a script they serve framed sessions under their own role name.
    for (const auto& role : {"login", "data"}) {
        auto& child = c.spawn({self().string(), "--role", role});
        require(child.wait() != 0 && child.output().find("--script") != std::string::npos,
                std::string(role) + " started without a script");
        write(c.path(std::string(role)+".lua"), "return {message=function(p)return p end}");
        const auto listen = port(), metrics = port();
        c.node(role, listen, {"--script", c.path(std::string(role)+".lua").string(),
                              "--metrics-listen", endpoint(metrics)});
        equal(tcp(listen)->request("ok"), "ok");
        const auto samples = http("http://"+endpoint(metrics)+"/metrics");
        require(samples.status == 200 && samples.body.find(std::string("mistlane_ready{role=\"")+role+"\"} 1") != std::string::npos,
            std::string(role) + " readiness gauge missing");
    }
    write(c.path("api.lua"), http_lua);
    const auto api_port = port();
    auto& api = c.node("apiServer", api_port, {"--script", c.path("api.lua").string()});
    require(api.output().find("ready apiServer ") != std::string::npos, "apiServer role missing");
    // Default apiServer transport is HTTP. Missing HTTP callback has a controlled response.
    require(http("http://"+endpoint(api_port)+"/missing").status != 0, "apiServer HTTP default failed");
}
void transport_tcp() {
    Context c;
    const auto script = c.path("echo.lua");
    write(script, echo);
    const auto gameServer=port(), forwarder=port(), gateway=port(), metrics=port();
    c.node("gameServer", gameServer, {"--script", script.string(), "--metrics-listen", endpoint(metrics)});
    c.node("gateway", forwarder, {"--upstream", endpoint(gameServer)});
    c.node("gateway", gateway, {"--upstream", endpoint(forwarder)});
    auto client = tcp(gateway);
    for (const auto& payload : std::vector<std::string>{{}, "hello", std::string("\0\xff", 2), "中文", std::string(1024*1024, 'x')})
        equal(client->request(payload), payload);
    for (int i=0; i<100; ++i) equal(client->request("stack"), "stack");
    auto samples = http("http://"+endpoint(metrics)+"/metrics");
    require(samples.status == 200 && samples.body.find("mistlane_ready{role=\"gameServer\"} 1") != std::string::npos,
        "Prometheus readiness missing");
    require(samples.body.find("mistlane_lua_callback_seconds_count") != std::string::npos, "callback histogram missing");
    const auto metric_value = [](const std::string& body, const std::string& sample) {
        const auto at = body.find(sample);
        require(at != std::string::npos, "missing metric " + sample);
        return std::strtod(body.c_str() + at + sample.size(), nullptr);
    };
    require(metric_value(samples.body, "mistlane_messages_total{role=\"gameServer\",direction=\"rx\"}") > 0,
        "rx message counter missing");
    require(metric_value(samples.body, "mistlane_messages_total{role=\"gameServer\",direction=\"tx\"}") > 0,
        "tx message counter missing");
    require(metric_value(samples.body, "mistlane_payload_bytes_total{role=\"gameServer\",direction=\"tx\"}") > 1024*1024,
        "payload byte counter missing");
    require(metric_value(samples.body, "mistlane_request_seconds_count{role=\"gameServer\"}") > 0,
        "request latency histogram missing");
    require(metric_value(samples.body, "mistlane_request_seconds_sum{role=\"gameServer\"}") > 0,
        "request latency not observed");
    // The first scheduled tick fires at least 100ms after start; wait for it.
    Http ticks;
    eventually([&] {
        ticks = http("http://"+endpoint(metrics)+"/metrics");
        return metric_value(ticks.body, "mistlane_event_loop_lag_seconds_count{role=\"gameServer\"}") > 0;
    });
    require(metric_value(ticks.body, "mistlane_event_loop_lag_max_seconds{role=\"gameServer\"}") > 0,
        "tick lag maximum missing");
    require(metric_value(samples.body, "mistlane_upstream_errors_total{role=\"gameServer\"}") == 0,
        "unexpected upstream errors");
    require(metric_value(samples.body, "mistlane_upstream_timeouts_total{role=\"gameServer\"}") == 0,
        "unexpected upstream timeouts");
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
    c.node("gameServer", limited, {"--script", script.string(), "--max-connections", "1"});
    auto first = tcp(limited); equal(first->request("one"), "one");
    RawSocket extra(limited);
    require(extra.closed(), "connection admission was not enforced");
    equal(first->request("still-alive"), "still-alive");
    const auto dead = port(), ingress = port(), gateway_metrics = port();
    c.node("gateway", ingress, {"--upstream", endpoint(dead), "--metrics-listen", endpoint(gateway_metrics)});
    must_fail([&] { (void)tcp(ingress)->request("unavailable"); });
    auto failure = http("http://"+endpoint(gateway_metrics)+"/metrics");
    require(metric_value(failure.body, "mistlane_upstream_errors_total{role=\"gateway\"}") >= 1,
        "upstream connect failure not counted");
}
void transport_websocket() {
    Context c; c.cert(); write(c.path("echo.lua"), echo);
    const auto gameServer = port(), forwarder = port();
    c.node("gameServer", gameServer, {"--script", c.path("echo.lua").string()});
    c.node("gateway", forwarder, {"--upstream", endpoint(gameServer)});
    for (const auto& transport : {"ws", "wss"}) {
        const bool secure = std::string_view(transport) == "wss";
        for (const auto& role : {"gateway", "gameServer"}) {
            const auto listen = port();
            std::vector<std::string> args{"--transport", transport};
            if (secure) { auto cert = tls(c); args.insert(args.end(), cert.begin(), cert.end()); }
            if (std::string_view(role) == "gateway") args.insert(args.end(), {"--upstream", endpoint(forwarder)});
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
    const auto gameServer = port();
    c.node("gameServer", gameServer, {"--script", c.path("echo.lua").string()});
    for (const auto& transport : {"http", "https"}) {
        const bool secure = std::string_view(transport) == "https";
        const auto listen = port();
        std::vector<std::string> args{"--transport", transport, "--upstream", endpoint(gameServer)};
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
void transport_listeners() {
    Context c; c.cert();
    write(c.path("echo.lua"), echo);
    write(c.path("ws.lua"), websocket_lua);
    // One process, three listeners: framed tcp plus ws and wss with a
    // per-listener certificate override. The ws listeners stay framed game
    // sessions (Lua message), exactly like single-transport runs.
    const auto plain = port(), ws_port = port(), wss_port = port();
    auto& mixed = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(plain),
        "--script", c.path("echo.lua").string(),
        "--listener", "ws://127.0.0.1:"+std::to_string(ws_port),
        "--listener", "wss://127.0.0.1:"+std::to_string(wss_port)+"?cert="+c.path("cert.pem").string()+"&key="+c.path("key.pem").string()});
    mixed.ready("ready gameServer 127.0.0.1:"+std::to_string(wss_port));
    equal(tcp(plain)->request("plain"), "plain");
    equal(cli::make_client({"ws://localhost:"+std::to_string(ws_port)+"/ws"})->request("session"), "session");
    equal(cli::make_client({"wss://localhost:"+std::to_string(wss_port)+"/ws", c.path("cert.pem").string()})->request("secure"), "secure");

    // apiServer: one http listener serving the Lua HTTP service and websocket
    // lifecycle side by side. Plain requests stay with Lua; upgrades upgrade.
    const auto api = port();
    auto& apiServer = c.node("apiServer", api, {"--transport", "http", "--script", c.path("ws.lua").string()});
    const auto origin = "http://"+endpoint(api);
    require(http(origin+"/ws").status == 404, "plain HTTP on the ws path must reach the Lua service");
    require(http(origin+"/missing").status == 404, "Lua HTTP routing broken");
    std::string lifecycle_id;
    {
        WsRaw ws(api);
        ws.text("hello");
        equal(ws.read_text(), "echo:hello");
        ws.text("state");
        const auto state = ws.read_text();
        require(state.starts_with("open=1 closed=0 "), "websocket registry state wrong: "+state);
        lifecycle_id = state.substr(state.rfind('=')+1);
        // A lifecycle message that sends nothing must not wedge the bridge.
        ws.text("noreply");
        ws.text("state");
        require(ws.read_text().starts_with("open=1 closed=0 "), "reply-less message broke the bridge");
        ws.text("oversize");
        equal(ws.read_text(), "guarded");
        ws.text("unknown");
        equal(ws.read_text(), "guarded");
        const auto response = http(origin+"/echo", "POST", "via-lua");
        require(response.status == 201, "HTTP service must share the port with websocket upgrades");
        equal(response.body, "via-lua");
        equal(http(origin+"/state").body, "open=1 closed=0");
    }
    // Clean disconnect: close hook runs, registry bookkeeping updates, same id.
    eventually([&] { return apiServer.output().find("WSHOOK close "+lifecycle_id) != std::string::npos; },
        5s, "websocket close hook did not run");
    eventually([&] { return http(origin+"/state").body == "open=1 closed=1"; }, 5s, "websocket session leak");
    // A Lua failure inside the message callback closes that session only.
    {
        WsRaw ws(api);
        ws.text("boom");
        eventually([&] { return apiServer.output().find("websocket message failed") != std::string::npos; },
            5s, "lua failure in websocket message not logged");
    }
    require(http(origin+"/state").status == 200, "HTTP service must survive a failed websocket session");
    equal(cli::make_client({"ws://localhost:"+std::to_string(api)+"/ws"})->request("still"), "echo:still");
    // Exit strategy: SIGTERM with a live websocket session still exits cleanly.
    {
        WsRaw ws(api);
        ws.text("hello");
        equal(ws.read_text(), "echo:hello");
        apiServer.signal(SIGTERM);
        require(apiServer.wait() == 0, "shutdown with live websocket sessions failed");
    }
    // Slow consumer: pushes past the send bound must close the runaway session
    // while the same process keeps serving fresh ones.
    const auto slow = port();
    auto& lagging = c.node("apiServer", slow, {"--transport", "http", "--script", c.path("ws.lua").string()});
    {
        WsRaw ws(slow);
        ws.text(flood_pushes());
        std::this_thread::sleep_for(500ms); // pushes queue while nobody reads
        const auto drained = ws.drain_until_closed(20s);
        require(drained > 0, "server closed before any flood data reached the client");
        equal(cli::make_client({"ws://localhost:"+std::to_string(slow)+"/ws"})->request("alive"), "echo:alive");
    }
    // HTTPS and WSS on one process: the https primary uses the global cert,
    // the wss listener carries its own via the query override.
    const auto tls_port = port(), wss_extra = port();
    {
        auto cert = tls(c);
        std::vector<std::string> args{"--transport", "https", "--script", c.path("ws.lua").string(),
            "--listener", "wss://127.0.0.1:"+std::to_string(wss_extra)+"?cert="+c.path("cert.pem").string()+"&key="+c.path("key.pem").string()};
        args.insert(args.end(), cert.begin(), cert.end());
        c.node("apiServer", tls_port, args);
    }
    require(http("https://localhost:"+std::to_string(tls_port)+"/state", "GET", {}, c.path("cert.pem").string()).status == 200,
        "https listener not serving the Lua service");
    // The wss listener stays a framed game session: message echo, not lifecycle.
    equal(cli::make_client({"wss://localhost:"+std::to_string(wss_extra)+"/ws", c.path("cert.pem").string()})->request("tls"), "tls");
    // Gateway: framed tcp listener beside an http listener, both forwarding.
    const auto gate = port(), gate_http = port();
    c.node("gateway", gate, {"--upstream", endpoint(plain), "--listener", "http://127.0.0.1:"+std::to_string(gate_http)});
    equal(tcp(gate)->request("tcp-via-gateway"), "tcp-via-gateway");
    const auto forwarded = http("http://"+endpoint(gate_http)+"/message", "POST", std::string("\0\xff", 2));
    require(forwarded.status == 200, "gateway /message forwarding");
    equal(forwarded.body, std::string("\0\xff", 2));
    equal(cli::make_client({"ws://localhost:"+std::to_string(gate_http)+"/ws"})->request("ws-via-gateway"), "ws-via-gateway");
    // Config files carry LISTENERS like any other key.
    const auto cfg_plain = port(), cfg_ws = port();
    write(c.path("node.conf"), "# mistlane-config 1\nROLE=gameServer\nLISTEN=127.0.0.1:"+std::to_string(cfg_plain)+
        "\nSCRIPT="+c.path("echo.lua").string()+"\nLISTENERS=ws://127.0.0.1:"+std::to_string(cfg_ws)+"\n");
    auto& configured = c.spawn({self().string(), "--config", c.path("node.conf").string()});
    configured.ready("ready gameServer 127.0.0.1:"+std::to_string(cfg_ws));
    equal(tcp(cfg_plain)->request("config"), "config");
    equal(cli::make_client({"ws://localhost:"+std::to_string(cfg_ws)+"/ws"})->request("config-ws"), "config-ws");
    // Conflicting listener sets must fail at startup, never half-serve.
    must_fail([&] { c.node("apiServer", port(), {"--transport", "http", "--script", c.path("ws.lua").string(),
        "--listener", "tcp://127.0.0.1:"+std::to_string(port())}); });
    must_fail([&] { const auto reused = port();
        c.node("gameServer", port(), {"--script", c.path("echo.lua").string(),
            "--listener", "tcp://127.0.0.1:"+std::to_string(reused),
            "--listener", "ws://127.0.0.1:"+std::to_string(reused)}); });
}
// Framed game sessions carry a session identity and accept Lua pushes:
// mistlane.session.send/broadcast deliver frames without a matching request,
// a handler may answer nothing, and outbound pushes stay bounded per session.
// Optional correlation mode: the first frame "MLC2" switches the session to
// u32 BE request ids carried inside the frame payload; pushes carry id 0.
void transport_correlate() {
    Context c;
    write(c.path("correlate.lua"), correlate_lua);
    const auto gameServer = port(), gateway = port(), ws_port = port();
    auto& node = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(gameServer),
        "--script", c.path("correlate.lua").string(),
        "--listener", "ws://127.0.0.1:"+std::to_string(ws_port)});
    c.node("gateway", gateway, {"--upstream", endpoint(gameServer)});
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        const auto header = raw.receive(4);
        std::size_t length = 0;
        for (unsigned char ch : header) length = (length << 8) | ch;
        return raw.receive(length);
    };
    const auto tagged = [](std::uint32_t id, std::string_view payload) {
        std::string frame(4, '\0');
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(id & 255); id >>= 8; }
        return frame+std::string(payload);
    };
    std::uint32_t id = 0;
    std::string body;
    const auto split = [&id, &body](const std::string& frame) {
        require(frame.size() >= 4, "correlated frame too short");
        id = 0;
        for (int i = 0; i < 4; ++i) id = (id<<8) | static_cast<unsigned char>(frame[i]);
        body = frame.substr(4);
    };
    // Handshake, then replies echo the request id byte for byte.
    RawSocket first(gameServer);
    send_frame(first, "MLC2");
    equal(read_frame(first), "MLC2 OK");
    send_frame(first, tagged(0x01020304, "hello"));
    split(read_frame(first));
    require(id == 0x01020304 && body == "hello", "correlated echo lost id or payload");
    send_frame(first, tagged(9, "self"));
    split(read_frame(first));
    require(id == 9 && body.starts_with("session=") && body.ends_with(":req=9"),
        "lua request id missing: "+body);
    // A nil reply still means no frame; pushes carry id 0 and precede the reply.
    send_frame(first, tagged(5, "silent"));
    std::this_thread::sleep_for(300ms);
    require(!first.pending(), "nil reply produced a frame");
    send_frame(first, tagged(6, "push"));
    split(read_frame(first));
    require(id == 0 && body == "pushed", "push must stay uncorrelated");
    split(read_frame(first));
    require(id == 6 && body == "done", "reply after push lost its id");
    // An oversized correlated push reports instead of failing the handler.
    send_frame(first, tagged(7, "big"));
    split(read_frame(first));
    require(id == 7 && body == "big=false:payload exceeds 1 MiB", "oversize push not reported: "+body);
    // A plain client on the same server stays uncorrelated; broadcast shapes differ.
    RawSocket second(gameServer);
    send_frame(second, "hello");
    equal(read_frame(second), "hello");
    send_frame(second, "bcast");
    split(read_frame(first));
    require(id == 0 && body == "bc", "broadcast to a correlated session must carry id 0");
    equal(read_frame(second), "bc");
    equal(read_frame(second), "bcast=2");
    // The handshake is honored as the first frame only.
    send_frame(second, "MLC2");
    equal(read_frame(second), "MLC2");
    // The same contract through a gateway: ids survive the relay.
    RawSocket via(gateway);
    send_frame(via, "MLC2");
    equal(read_frame(via), "MLC2 OK");
    send_frame(via, tagged(0x0A0B0C0D, "ping"));
    split(read_frame(via));
    require(id == 0x0A0B0C0D && body == "ping", "gateway lost correlation");
    send_frame(via, tagged(3, "self"));
    split(read_frame(via));
    require(id == 3 && body.ends_with(":req=3"), "gateway lua request id missing: "+body);
    // The ws game-session leg speaks the same mode in binary frames.
    {
        WsRaw over(ws_port);
        over.text("MLC2");
        equal(over.read_text(), "MLC2 OK");
        over.bin(tagged(11, "w"));
        split(over.read_text());
        require(id == 11 && body == "w", "ws correlated echo failed");
    }
    // Cross routing keeps its own handshake and rejects correlation.
    const auto shard = port(), cross_gate = port();
    c.node("crossServer", shard, {"--script", c.path("correlate.lua").string()});
    c.cross_gateway(cross_gate, {"--upstream", endpoint(shard)});
    {
        RawSocket crossed(cross_gate);
        send_frame(crossed, "MLC2");
        equal(read_frame(crossed), "MLC1 ERR INVALID");
    }
    require(node.output().find("callback failed") == std::string::npos, "correlation path logged Lua failures");
}
// MLS1 identifies internal legs: a node with --service-token only serves
// peers that present "MLS1 <role> <instance> <token>", a gateway with
// --upstream-token presents itself before any business frame, and every
// refusal closes the connection with a reason instead of leaking frames into
// the script.
void transport_identity() {
    Context c;
    write(c.path("correlate.lua"), correlate_lua);
    const auto gameServer = port(), open_server = port(), gateway = port(),
              no_token = port(), wrong_token = port(), mismatched = port();
    auto& node = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(gameServer),
        "--script", c.path("correlate.lua").string(), "--service-token", "s3cret",
        "--service-name", "game-1"});
    auto& open_node = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(open_server),
        "--script", c.path("correlate.lua").string()});
    c.node("gateway", gateway, {"--upstream", endpoint(gameServer),
        "--upstream-token", "s3cret", "--service-name", "gwy-1"});
    c.node("gateway", no_token, {"--upstream", endpoint(gameServer)});
    c.node("gateway", wrong_token, {"--upstream", endpoint(gameServer), "--upstream-token", "nope"});
    c.node("gateway", mismatched, {"--upstream", endpoint(open_server), "--upstream-token", "s3cret"});
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        const auto header = raw.receive(4);
        std::size_t length = 0;
        for (unsigned char ch : header) length = (length << 8) | ch;
        return raw.receive(length);
    };
    const auto tagged = [](std::uint32_t id, std::string_view payload) {
        std::string frame(4, '\0');
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(id & 255); id >>= 8; }
        return frame+std::string(payload);
    };
    std::uint32_t id = 0;
    std::string body;
    const auto split = [&id, &body](const std::string& frame) {
        require(frame.size() >= 4, "correlated frame too short");
        id = 0;
        for (int i = 0; i < 4; ++i) id = (id<<8) | static_cast<unsigned char>(frame[i]);
        body = frame.substr(4);
    };
    const auto expect_close = [](RawSocket& raw) {
        for (int i = 0; i < 40 && !raw.closed(); ++i) std::this_thread::sleep_for(50ms);
        require(raw.closed(), "connection stayed open after a refusal");
    };
    // An unauthenticated first frame never reaches the script.
    RawSocket anonymous(gameServer);
    send_frame(anonymous, "self");
    equal(read_frame(anonymous), "MLS1 ERR TOKEN");
    expect_close(anonymous);
    // Malformed handshakes report the shape, not the secret.
    for (const auto& hello : {"MLS1 onlyrole", "MLS1 a b c d"}) {
        RawSocket malformed(gameServer);
        send_frame(malformed, hello);
        equal(read_frame(malformed), "MLS1 ERR INVALID");
        expect_close(malformed);
    }
    RawSocket refused(gameServer);
    send_frame(refused, "MLS1 gateway inst-9 nope");
    equal(read_frame(refused), "MLS1 ERR TOKEN");
    expect_close(refused);
    // Correct credentials admit the session; the handshake stays invisible after.
    RawSocket joined(gameServer);
    send_frame(joined, "MLS1 gateway inst-9 s3cret");
    equal(read_frame(joined), "MLS1 OK");
    send_frame(joined, "self");
    body = read_frame(joined);
    require(body.starts_with("session=") && body.ends_with(":req=nil"), "handshaken session broken: "+body);
    // MLC2 stacks on the accepted handshake: both close at the first payload.
    RawSocket correlated(gameServer);
    send_frame(correlated, "MLS1 gateway inst-9 s3cret");
    equal(read_frame(correlated), "MLS1 OK");
    send_frame(correlated, "MLC2");
    equal(read_frame(correlated), "MLC2 OK");
    send_frame(correlated, tagged(9, "self"));
    split(read_frame(correlated));
    require(id == 9 && body.ends_with(":req=9"), "correlation lost after the handshake: "+body);
    // A token-bearing gateway shows no handshake at either end.
    RawSocket via(gateway);
    send_frame(via, "MLC2");
    equal(read_frame(via), "MLC2 OK");
    send_frame(via, tagged(3, "ping"));
    split(read_frame(via));
    require(id == 3 && body == "ping", "gateway handshake leaked into the relay: "+body);
    RawSocket plain_via(gateway);
    send_frame(plain_via, "self");
    body = read_frame(plain_via);
    require(body.starts_with("session=") && body.ends_with(":req=nil"), "gateway session broke: "+body);
    // An upstream refusal ends the session without relaying the verdict.
    RawSocket silent(wrong_token);
    send_frame(silent, "ping");
    std::this_thread::sleep_for(300ms);
    require(!silent.pending(), "upstream verdict leaked to the client");
    expect_close(silent);
    // A gateway without a token cannot hide the mismatch, so it relays the reason.
    RawSocket relayed(no_token);
    send_frame(relayed, "ping");
    equal(read_frame(relayed), "MLS1 ERR TOKEN");
    expect_close(relayed);
    // A server that needs no handshake refuses the stray one.
    RawSocket to_plain(open_server);
    send_frame(to_plain, "MLS1 gateway inst-9 s3cret");
    equal(read_frame(to_plain), "MLS1 ERR UNEXPECTED");
    expect_close(to_plain);
    RawSocket via_open(mismatched);
    send_frame(via_open, "ping");
    std::this_thread::sleep_for(300ms);
    require(!via_open.pending(), "server refusal leaked to the client");
    expect_close(via_open);
    // ...and keeps serving anonymous clients.
    RawSocket open_client(open_server);
    send_frame(open_client, "hello");
    equal(read_frame(open_client), "hello");
    // Cross routing keeps its own identity, so the token flags refuse to start.
    const auto cross_arg = std::vector<std::string>{"--routing", "cross"};
    for (const auto& extra : {std::vector<std::string>{"--service-token", "s3cret"},
                              std::vector<std::string>{"--upstream-token", "s3cret"}}) {
        auto args = std::vector<std::string>{self().string(), "--role", "gateway",
            "--listen", endpoint(port()), "--upstream", endpoint(gameServer)};
        args.insert(args.end(), cross_arg.begin(), cross_arg.end());
        args.insert(args.end(), extra.begin(), extra.end());
        auto& child = c.spawn(args);
        require(child.wait() != 0 && child.output().find("does not apply to cross routing") != std::string::npos,
                "token flags accepted on cross routing");
    }
    for (const auto& node_output : {node.output(), open_node.output()})
        require(node_output.find("callback failed") == std::string::npos, "handshake path logged Lua failures");
}
// The gateway --upstream-multiplex mode (MLM1) shares one upstream connection
// across client sessions: logical sessions stay isolated, pushes and broadcast
// address a single session, a Lua error or disconnect closes only its own
// logical session, and plain clients are told off instead of upgrading.
void transport_multiplex() {
    Context c;
    write(c.path("correlate.lua"), correlate_lua);
    const auto gameServer = port(), metrics = port(), gateway = port();
    auto& node = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(gameServer),
        "--script", c.path("correlate.lua").string(), "--service-token", "s3cret",
        "--metrics-listen", endpoint(metrics)});
    c.node("gateway", gateway, {"--upstream", endpoint(gameServer),
        "--upstream-token", "s3cret", "--upstream-multiplex", "1"});
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        const auto header = raw.receive(4);
        std::size_t length = 0;
        for (unsigned char ch : header) length = (length << 8) | ch;
        return raw.receive(length);
    };
    const auto tagged = [](std::uint32_t id, std::string_view payload) {
        std::string frame(4, '\0');
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(id & 255); id >>= 8; }
        return frame+std::string(payload);
    };
    std::uint32_t id = 0;
    std::string body;
    const auto split = [&id, &body](const std::string& frame) {
        require(frame.size() >= 4, "correlated frame too short");
        id = 0;
        for (int i = 0; i < 4; ++i) id = (id<<8) | static_cast<unsigned char>(frame[i]);
        body = frame.substr(4);
    };
    const auto expect_close = [](RawSocket& raw) {
        for (int i = 0; i < 40 && !raw.closed(); ++i) std::this_thread::sleep_for(50ms);
        require(raw.closed(), "connection stayed open after a refusal");
    };
    const auto established = [](unsigned port) {
        char hex[8];
        std::snprintf(hex, sizeof hex, "%04X", port);
        std::ifstream net("/proc/net/tcp");
        std::string sl, local, remote, state, rest;
        int count = 0;
        while (net >> sl >> local >> remote >> state) {
            if (state == "01" && local.ends_with(":"+std::string(hex))) ++count;
            std::getline(net, rest);
        }
        return count;
    };
    // Two clients ride one upstream connection; the server still counts both.
    RawSocket a(gateway), b(gateway);
    send_frame(a, "hello"); equal(read_frame(a), "hello");
    send_frame(b, "world"); equal(read_frame(b), "world");
    std::this_thread::sleep_for(300ms);
    require(established(gameServer) == 1, "upstream connection not shared by logical sessions");
    const auto live = http("http://"+endpoint(metrics)+"/metrics").body;
    require(live.find("mistlane_active_sessions{role=\"gameServer\"} 2") != std::string::npos,
            "logical sessions missing from server admission");
    // Correlation is a first-frame handshake, so it lives on a third client and
    // must survive the envelope of a session it never shared.
    RawSocket corr(gateway);
    send_frame(corr, "MLC2"); equal(read_frame(corr), "MLC2 OK");
    send_frame(corr, tagged(5, "self"));
    split(read_frame(corr));
    require(id == 5 && body.ends_with(":req=5"), "correlation lost through the shared leg: "+body);
    send_frame(b, "self");
    body = read_frame(b);
    require(body.starts_with("session=") && body.ends_with(":req=nil"), "plain session broken: "+body);
    // A push and a broadcast stay inside their logical session.
    send_frame(corr, tagged(6, "push"));
    split(read_frame(corr)); require(id == 0 && body == "pushed", "push lost on the shared leg");
    split(read_frame(corr)); require(id == 6 && body == "done", "reply lost after a push");
    require(!b.pending(), "push leaked into another logical session");
    // A directly authenticated session shares the registry too, so the count
    // reaches four: a, b, corr and the direct peer.
    RawSocket direct(gameServer);
    send_frame(direct, "MLS1 gateway inst-9 s3cret");
    equal(read_frame(direct), "MLS1 OK");
    send_frame(direct, "hi"); equal(read_frame(direct), "hi");
    send_frame(b, "bcast");
    split(read_frame(corr)); require(id == 0 && body == "bc", "broadcast shape lost on the correlated session");
    equal(read_frame(a), "bc");
    equal(read_frame(b), "bc"); equal(read_frame(b), "bcast=4");
    equal(read_frame(direct), "bc");
    // A Lua error closes only that logical session.
    {
        RawSocket err(gateway);
        send_frame(err, "oops");
        expect_close(err);
    }
    require(node.output().find("mux oops") != std::string::npos,
            "logical session error not reported");
    send_frame(b, "again"); equal(read_frame(b), "again");
    // A disconnect retires its logical session; the rest of the hub survives.
    {
        RawSocket gone(gateway);
        send_frame(gone, "bye"); equal(read_frame(gone), "bye");
        send_frame(b, "bcast");
        equal(read_frame(b), "bc"); equal(read_frame(b), "bcast=5");
        equal(read_frame(a), "bc"); // the survivors read the broadcast twice
    }
    std::this_thread::sleep_for(300ms);
    send_frame(b, "bcast");
    equal(read_frame(b), "bc"); equal(read_frame(b), "bcast=4");
    equal(read_frame(a), "bc");
    // Nobody upgrades mid-session, whether through the gateway or directly.
    send_frame(a, "MLM1"); equal(read_frame(a), "MLM1");
    {
        RawSocket mid(gateway);
        send_frame(mid, "hi"); equal(read_frame(mid), "hi");
        send_frame(mid, "MLM1"); equal(read_frame(mid), "MLM1");
    }
    RawSocket plain(gameServer);
    send_frame(plain, "MLM1");
    equal(read_frame(plain), "MLM1 ERR UNEXPECTED");
    expect_close(plain);
    {
        RawSocket through(gateway);
        send_frame(through, "MLM1");
        equal(read_frame(through), "MLM1 ERR UNEXPECTED");
        expect_close(through);
    }
    // Multiplexing is an internal mode, so it needs the identity handshake.
    {
        auto& bad = c.spawn({self().string(), "--role", "gateway", "--listen", endpoint(port()),
                             "--upstream", endpoint(gameServer), "--upstream-multiplex", "1"});
        require(bad.wait() != 0 && bad.output().find("requires --upstream-token") != std::string::npos,
                "multiplex accepted without a service identity");
    }
    require(node.output().find("unexpected frame") == std::string::npos, "hub parsed a malformed frame");
}
// The coordinator role (MLR1) keeps a live registry instead of forwarding
// gameplay: members register with a scope and heartbeat readiness/load, a
// registry client lists or places against it, disconnects drop entries, and a
// token-requiring coordinator admits only identified members.
void coordinator_registry() {
    Context c;
    write(c.path("echo.lua"), correlate_lua);
    const auto registry = port(), metrics = port(), tokened = port(),
              first = port(), second = port(), third = port();
    auto& coord = c.node("coordinator", registry, {"--metrics-listen", endpoint(metrics)});
    c.node("coordinator", tokened, {"--service-token", "s3cret"});
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        const auto header = raw.receive(4);
        std::size_t length = 0;
        for (unsigned char ch : header) length = (length << 8) | ch;
        return raw.receive(length);
    };
    const auto expect_close = [](RawSocket& raw) {
        for (int i = 0; i < 40 && !raw.closed(); ++i) std::this_thread::sleep_for(50ms);
        require(raw.closed(), "connection stayed open after a refusal");
    };
    // The registry is not a forwarding role: unknown traffic is refused.
    RawSocket stranger(registry);
    send_frame(stranger, "hello");
    equal(read_frame(stranger), "MLR1 ERR UNEXPECTED");
    expect_close(stranger);
    // Malformed registrations report the shape.
    for (const auto& bad : {"MLR1 REGISTER", "MLR1 REGISTER data gs-x alpha 127.0.0.1:1 0",
                            "MLR1 REGISTER gameServer gs-x alpha not-an-endpoint 0"}) {
        RawSocket malformed(registry);
        send_frame(malformed, bad);
        equal(read_frame(malformed), "MLR1 ERR INVALID");
        expect_close(malformed);
    }
    // Members join two scopes; the first saturates under real load.
    auto& gs_a = c.node("gameServer", first, {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(registry), "--scope", "alpha", "--service-name", "gs-a",
        "--max-connections", "4", "--overload-ratio", "0.5", "--ready-hysteresis", "1"});
    auto& gs_b = c.node("gameServer", second, {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(registry), "--scope", "alpha", "--service-name", "gs-b"});
    auto& gs_c = c.node("gameServer", third, {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(registry), "--scope", "beta", "--service-name", "gs-c"});
    std::string body;
    const auto query = [&](const std::string& line) {
        RawSocket raw(registry);
        send_frame(raw, line);
        body = read_frame(raw);
        expect_close(raw);
        return body;
    };
    const auto place = [&](const std::string& line) {
        return query(line);
    };
    eventually([&] { return query("MLR1 QUERY").find("MLR1 MEMBERS 3") != std::string::npos; },
        5s, "members never reached the registry");
    require(body.find("member gameServer gs-a alpha") != std::string::npos &&
            body.find("member gameServer gs-b alpha") != std::string::npos &&
            body.find("member gameServer gs-c beta") != std::string::npos,
            "registry listing lost a member: "+body);
    // Scope filters narrow the listing and placement.
    query("MLR1 QUERY gameServer beta");
    require(body.find("gs-c") != std::string::npos && body.find("gs-a") == std::string::npos,
            "scope filter leaked another scope: "+body);
    const auto beta_place = "MLR1 PLACE gs-c 127.0.0.1:"+std::to_string(third)+" 0 ready 0/42";
    eventually([&] { return place("MLR1 PLACE gameServer beta") == beta_place; },
        5s, "gs-c never became placeable");
    equal(place("MLR1 PLACE gameServer beta"), beta_place);
    equal(place("MLR1 PLACE gameServer gamma"), "MLR1 EMPTY");
    // Placement prefers the less loaded member once real load differs.
    {
        RawSocket load(first);
        send_frame(load, "ping"); equal(read_frame(load), "ping");
        eventually([&] { return place("MLR1 PLACE gameServer alpha") ==
            "MLR1 PLACE gs-b 127.0.0.1:"+std::to_string(second)+" 0 ready 0/42"; },
            5s, "placement ignored member load");
        // Saturation flips readiness, and the registry follows the heartbeats.
        RawSocket full(first);
        send_frame(full, "ping"); equal(read_frame(full), "ping");
        eventually([&] { return query("MLR1 QUERY gameServer alpha").find(
            "member gameServer gs-a alpha 127.0.0.1:"+std::to_string(first)+" 0 notready 2/4")
            != std::string::npos; }, 5s, "readiness did not follow the heartbeat");
    }
    std::this_thread::sleep_for(600ms);
    eventually([&] { return query("MLR1 QUERY gameServer alpha").find(
        "member gameServer gs-a alpha 127.0.0.1:"+std::to_string(first)+" 0 ready 0/4")
        != std::string::npos; }, 5s, "readiness did not recover");
    // A disconnect drops the member from the registry.
    gs_c.signal(SIGTERM);
    eventually([&] { return query("MLR1 QUERY").find("MLR1 MEMBERS 2") != std::string::npos; },
        5s, "a dead member stayed registered");
    // The registry gauge follows the live membership.
    const auto live = http("http://"+endpoint(metrics)+"/metrics").body;
    require(live.find("mistlane_registry_members{role=\"coordinator\"} 2") != std::string::npos,
            "registry gauge missing from the exposition");
    // Identity stacks on the registry: an unnamed member is refused, an
    // identified one registers normally.
    RawSocket anonymous(tokened);
    send_frame(anonymous, "MLR1 REGISTER gameServer gs-x alpha 127.0.0.1:1 0");
    equal(read_frame(anonymous), "MLS1 ERR TOKEN");
    expect_close(anonymous);
    auto& gs_d = c.node("gameServer", port(), {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(tokened), "--upstream-token", "s3cret", "--service-name", "gs-d"});
    const auto identified = [&]() {
        RawSocket raw(tokened);
        send_frame(raw, "MLS1 registryClient query-1 s3cret");
        equal(read_frame(raw), "MLS1 OK");
        send_frame(raw, "MLR1 QUERY");
        body = read_frame(raw);
        expect_close(raw);
        return body;
    };
    eventually([&] { return identified().find("member gameServer gs-d main") != std::string::npos; },
        5s, "an identified member never registered");
    require(gs_a.output().find("registered") != std::string::npos &&
            gs_d.output().find("registered") != std::string::npos,
            "member registration not logged");
    // The coordinator is not a renamed router: it takes neither script nor
    // upstreams, and the member flags do not leak into other roles.
    {
        auto& scripted = c.spawn({self().string(), "--role", "coordinator", "--listen", endpoint(port()),
            "--script", c.path("echo.lua").string()});
        require(scripted.wait() != 0 && scripted.output().find("requires no script and no upstream") != std::string::npos,
                "coordinator accepted a script");
        auto& routed = c.spawn({self().string(), "--role", "gateway", "--listen", endpoint(port()),
            "--upstream", endpoint(registry), "--coordinator", endpoint(registry)});
        require(routed.wait() != 0 && routed.output().find("--coordinator applies to gameServer, crossServer and login") != std::string::npos,
                "gateway accepted a coordinator");
        auto& scoped = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(port()),
            "--script", c.path("echo.lua").string(), "--scope", "alpha"});
        require(scoped.wait() != 0 && scoped.output().find("--scope requires --coordinator") != std::string::npos,
                "scope accepted without a coordinator");
    }
    for (const auto* member : {&gs_a, &gs_b})
        require(member->output().find("callback failed") == std::string::npos, "registry member logged Lua failures");
    require(coord.output().find("unexpected frame") == std::string::npos, "coordinator log noise");
}
// login is a registry client, not a member: each placement ask owns one
// connection, and the callback answers through mistlane.session.send because
// the answer arrives after the handler returned.
void coordinator_placement() {
    Context c;
    write(c.path("echo.lua"), echo);
    write(c.path("place.lua"),
        "return {message=function(p,c)"
        " local role, scope = p:match('place (%S+) ?(%S*)')\n"
        " if not role then return 'usage' end\n"
        " mistlane.place(role, scope ~= '' and scope or nil, function(err, address)\n"
        "   mistlane.session.send(c.session, err and ('ERR '..err) or ('OK '..address))\n"
        " end)\n"
        " return nil end}");
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame + std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        std::size_t length = 0;
        for (unsigned char ch : raw.receive(4)) length = (length << 8) | ch;
        return raw.receive(length);
    };
    const auto place = [&](unsigned listen, std::string_view request) {
        RawSocket raw(listen);
        send_frame(raw, request);
        return read_frame(raw);
    };
    const auto members = [&](unsigned metrics) {
        const auto body = http("http://"+endpoint(metrics)+"/metrics").body;
        const auto needle = std::string("mistlane_registry_members{role=\"coordinator\"} ");
        const auto at = body.find(needle);
        return at == std::string::npos ? -1 : static_cast<int>(std::strtod(body.c_str()+at+needle.size(), nullptr));
    };
    const auto registry = port(), member_listen = port(), login = port(), metrics = port(),
             token_registry = port(), token_member = port(), token_login = port(),
             wrong_login = port(), dead_login = port();
    auto& coord = c.node("coordinator", registry, {"--metrics-listen", endpoint(metrics)});
    c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(member_listen),
             "--script", c.path("echo.lua").string(), "--coordinator", endpoint(registry)});
    eventually([&] { return members(metrics) == 1; }, 5s, "the member never registered");
    c.node("login", login, {"--script", c.path("place.lua").string(), "--coordinator", endpoint(registry)});
    // Registration precedes the first readiness heartbeat, so the ask waits
    // for the ready verdict instead of racing it.
    eventually([&] { return place(login, "place gameServer")
        == "OK 127.0.0.1:"+std::to_string(member_listen); }, 5s, "placement never saw the ready member");
    equal(place(login, "place gameServer nobody"), "ERR EMPTY");
    equal(place(login, "place crossServer"), "ERR EMPTY");
    equal(place(login, "where"), "usage");
    require(members(metrics) == 1, "login registered itself as a member");
    // A tokened coordinator gates the lookup behind MLS1.
    auto& token_coord = c.node("coordinator", token_registry, {"--service-token", "s3cret"});
    c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(token_member),
             "--script", c.path("echo.lua").string(), "--coordinator", endpoint(token_registry),
             "--upstream-token", "s3cret", "--service-name", "gs-token"});
    c.node("login", token_login, {"--script", c.path("place.lua").string(),
        "--coordinator", endpoint(token_registry), "--upstream-token", "s3cret", "--service-name", "login-1"});
    c.node("login", wrong_login, {"--script", c.path("place.lua").string(),
        "--coordinator", endpoint(token_registry), "--upstream-token", "nope"});
    eventually([&] { return place(token_login, "place gameServer")
        == "OK 127.0.0.1:"+std::to_string(token_member); }, 5s, "identity did not reach the tokened registry");
    equal(place(wrong_login, "place gameServer"), "ERR TOKEN");
    // A dead coordinator reports a reason instead of holding the session.
    c.node("login", dead_login, {"--script", c.path("place.lua").string(), "--coordinator", endpoint(port())});
    equal(place(dead_login, "place gameServer"), "ERR unreachable");
    // The registry stays the coordinator's job: data never presents itself.
    auto& data = c.spawn({self().string(), "--role", "data", "--listen", endpoint(port()),
        "--script", c.path("place.lua").string(), "--coordinator", endpoint(registry)});
    require(data.wait() != 0 && data.output().find("--coordinator applies to gameServer, crossServer and login")
        != std::string::npos, "data accepted registry placement");
    require(coord.output().find("member registered login") == std::string::npos, "login registered as a member");
    require(coord.output().find("unexpected frame") == std::string::npos, "coordinator log noise");
    require(token_coord.output().find("unexpected frame") == std::string::npos, "coordinator log noise");
}
void transport_push() {
    Context c;
    write(c.path("push.lua"), push_lua);
    const auto gameServer = port(), gateway = port(), ws_port = port();
    auto& node = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(gameServer),
        "--script", c.path("push.lua").string(),
        "--listener", "ws://127.0.0.1:"+std::to_string(ws_port)});
    c.node("gateway", gateway, {"--upstream", endpoint(gameServer)});
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        const auto header = raw.receive(4);
        std::size_t length = 0;
        for (unsigned char ch : header) length = (length << 8) | ch;
        return raw.receive(length);
    };
    RawSocket first(gameServer);
    send_frame(first, "self");
    equal(read_frame(first), "session=1");
    // A handler may answer nothing; the session must stay usable afterwards.
    send_frame(first, "silent");
    std::this_thread::sleep_for(300ms);
    require(!first.pending(), "nil handler reply produced a frame");
    send_frame(first, "self");
    equal(read_frame(first), "session=1");
    // Lua push reaches the client without a request; the reply follows the pushes.
    send_frame(first, "push");
    equal(read_frame(first), "pushed");
    equal(read_frame(first), "done");
    // Broadcast reaches every session; each client sees the initiator's id.
    RawSocket second(gameServer);
    send_frame(second, "self");
    equal(read_frame(second), "session=2");
    send_frame(first, "bcast");
    equal(read_frame(second), "bc:1");
    equal(read_frame(first), "bc:1");
    equal(read_frame(first), "bcast=2");
    // Push bounds: per-frame cap, unknown ids and the backlog report, not close.
    send_frame(first, "burst 64");
    std::string burst;
    std::size_t pushed = 0;
    for (;;) {
        burst = read_frame(first);
        if (burst.starts_with("burst=")) break;
        require(burst.size() == 64*1024 && burst.find_first_not_of('B') == std::string::npos,
            "pushed frame corrupted");
        ++pushed;
        require(pushed <= 64, "more push frames queued than attempts");
    }
    const auto colon = burst.find(':');
    const auto queued = std::stoul(burst.substr(6, colon-6));
    const auto refused = std::stoul(burst.substr(colon+1));
    require(queued == pushed && refused > 0 && queued+refused == 64, "push bounds not enforced: "+burst);
    // The same contract through a gateway: pushes relay unsolicited.
    RawSocket via(gateway);
    send_frame(via, "self");
    equal(read_frame(via), "session=3");
    send_frame(via, "push");
    equal(read_frame(via), "pushed");
    equal(read_frame(via), "done");
    // Pushes and broadcast also work on the ws game-session leg.
    {
        WsRaw over(ws_port);
        over.text("self");
        equal(over.read_text(), "session=4");
        over.text("push");
        equal(over.read_text(), "pushed");
        equal(over.read_text(), "done");
        over.text("bcast");
        equal(over.read_text(), "bc:4");
        equal(over.read_text(), "bcast=4");
    }
    require(node.output().find("callback failed") == std::string::npos, "push path logged Lua failures");
}
void lua_lifecycle() {
    Context c; const auto script = c.path("service.lua"); write(script, echo);
    const auto listen = port();
    auto& gameServer = c.node("gameServer", listen, {"--script", script.string()});
    auto client = tcp(listen); equal(client->request("v1"), "v1");
    write(script, "return {message=function(p) return 'v2:'..p end}");
    gameServer.signal(SIGHUP); gameServer.ready("script reloaded");
    equal(client->request("x"), "v2:x");
    write(script, "invalid lua !!!");
    gameServer.signal(SIGHUP); gameServer.ready("reload rejected");
    equal(client->request("x"), "v2:x");
    write(script, "return {}");
    gameServer.signal(SIGHUP); gameServer.ready("service.message is required");
    equal(client->request("x"), "v2:x");
    write(c.path("bad.lua"), "return {message=function(p) if p=='error' then error('bad') end "
        "if p=='loop' then while true do end end return p end}");
    const auto bad = port();
    c.node("gameServer", bad, {"--script", c.path("bad.lua").string()});
    for (const auto& payload : {"error", "loop"}) must_fail([&] { (void)tcp(bad)->request(payload); });
    equal(tcp(bad)->request("healthy"), "healthy");
    // The actual exported Lua source is embedded at configure time, so installed
    // binaries can run this regression without depending on a source checkout.
    write(c.path("heap.lua"), "local H=assert(load([==["+std::string(heap_lua)+"]==]))()\n"
        "assert(MinHeap==nil); local h=H:new(3,function(a,b) return a<b end)\n"
        "h:push(3);h:push(1);h:push(2);assert(h:pop()==1 and h:pop()==2 and h:pop()==3)\n"
        "return {message=function(p)return p end}");
    const auto heap = port(); c.node("gameServer", heap, {"--script", c.path("heap.lua").string()});
    equal(tcp(heap)->request("heap"), "heap");
    gameServer.signal(SIGTERM); require(gameServer.wait() == 0, "gameServer graceful shutdown failed");
}
void lua_http() {
    Context c; const auto script = c.path("http.lua"); write(script, http_lua);
    const auto listen = port();
    auto& server = c.node("apiServer", listen, {"--transport", "http", "--script", script.string()});
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
    Context c; write(c.path("cross.lua"),
        "return {message=function(p,c)"
        " local who = c.character and c.character..':'..c.generation..':'"
        " ..c.route.realm..'/'..c.route.group..'/'..c.route.room or 'anon'"
        " return c.realm..'('..who..'):'..c.group..'/'..c.room..':'..p end}");
    const auto s0=port(), s1=port(), ingress=port(), ingress2=port();
    auto& shard = c.node("crossServer", s0, {"--script",c.path("cross.lua").string(),"--max-room-sessions","2"});
    c.node("crossServer", s1, {"--script",c.path("cross.lua").string(),"--max-room-sessions","2"});
    std::vector<std::string> args{"--upstream",endpoint(s0),"--upstream",endpoint(s1),"--max-room-sessions","2"};
    c.cross_gateway( ingress, args); c.cross_gateway( ingress2, args);
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
        equal(a->request("x"),"a(anon):g/"+rooms[0]+":x"); equal(b->request("x"),"b(anon):g/"+rooms[0]+":x");
        for (auto entry : {ingress,ingress2}) {
            auto third=tcp(entry); equal(join(*third,"c",rooms[0]),"MLC1 ERR ROOM_FULL");
        }
        auto other=tcp(ingress); equal(join(*other,"c",rooms[1]),"MLC1 OK 1");
    }
    eventually([&] { auto released=tcp(ingress); return join(*released,"d",rooms[0])=="MLC1 OK 1"; });
    {
        // A full participant declaration reaches Lua; every partial one is refused.
        auto hero=tcp(ingress);
        equal(hero->request("MLC1 1 a g "+rooms[0]+" ch hero-3 gen 7 ret home arena r1"), "MLC1 OK 1");
        equal(hero->request("x"), "a(hero-3:7:home/arena/r1):g/"+rooms[0]+":x");
        auto partial=tcp(ingress);
        equal(partial->request("MLC1 1 a g "+rooms[0]+" ch hero-3"), "MLC1 ERR INVALID");
    }
    equal(tcp(ingress)->request("MLC1 2 a g r"),"MLC1 ERR EPOCH");
    equal(tcp(ingress)->request("invalid"),"MLC1 ERR INVALID");
    shard.signal(SIGTERM); require(shard.wait()==0,"shard shutdown failed");
    must_fail([&] { auto failed=tcp(ingress); (void)join(*failed,"a",rooms[0]); });
    auto healthy=tcp(ingress); equal(join(*healthy,"a",rooms[1]),"MLC1 OK 1");
    equal(healthy->request("ok"),"a(anon):g/"+rooms[1]+":ok");
}
// Character route records: a route is only written against a registered
// member, GET answers from the live member state, and a retiring member drops
// every route that pointed at it.
void coordinator_routes() {
    Context c;
    write(c.path("echo.lua"), correlate_lua);
    const auto registry = port(), metrics = port(), tokened = port(),
              first = port(), second = port();
    auto& coord = c.node("coordinator", registry, {"--metrics-listen", endpoint(metrics)});
    c.node("coordinator", tokened, {"--service-token", "s3cret"});
    auto& gs_a = c.node("gameServer", first, {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(registry), "--scope", "alpha", "--service-name", "gs-a"});
    auto& gs_b = c.node("gameServer", second, {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(registry), "--scope", "alpha", "--service-name", "gs-b"});
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        const auto header = raw.receive(4);
        std::size_t length = 0;
        for (unsigned char ch : header) length = (length << 8) | ch;
        return raw.receive(length);
    };
    const auto expect_close = [](RawSocket& raw) {
        for (int i = 0; i < 40 && !raw.closed(); ++i) std::this_thread::sleep_for(50ms);
        require(raw.closed(), "connection stayed open after a refusal");
    };
    std::string body;
    // One-shot legs: one frame in, one verdict out, then the coordinator closes.
    const auto ask = [&](const std::string& line) {
        RawSocket raw(registry);
        send_frame(raw, line);
        body = read_frame(raw);
        expect_close(raw);
        return body;
    };
    eventually([&] { return ask("MLR1 QUERY").find("MLR1 MEMBERS 2") != std::string::npos; },
        5s, "members never reached the registry");
    // GET before any SET is an honest empty, not an error.
    equal(ask("MLR1 ROUTE GET mist eve"), "MLR1 ROUTE EMPTY");
    // Malformed shapes and unknown verbs report the right word.
    for (const auto& [line, word] : std::vector<std::pair<std::string, std::string>>{
            {"MLR1 ROUTE SET alpha mist eve gameServer", "MLR1 ERR INVALID"},
            {"MLR1 ROUTE SET alpha mist eve gameServer gs-a extra", "MLR1 ERR INVALID"},
            {"MLR1 ROUTE SET alpha mi!t eve gameServer gs-a", "MLR1 ERR INVALID"},
            {"MLR1 ROUTE SET alpha mist eve data gs-a", "MLR1 ERR INVALID"},
            {"MLR1 ROUTE GET mist", "MLR1 ERR INVALID"},
            {"MLR1 ROUTE GET mist ev!", "MLR1 ERR INVALID"},
            {"MLR1 ROUTE LIST", "MLR1 ERR UNEXPECTED"},
            {"MLR1 ROUTE", "MLR1 ERR UNEXPECTED"}}) {
        RawSocket raw(registry);
        send_frame(raw, line);
        equal(read_frame(raw), word);
        expect_close(raw);
    }
    // A route needs a live host: an unregistered instance is refused by name.
    equal(ask("MLR1 ROUTE SET alpha mist eve gameServer gs-z"), "MLR1 ERR UNKNOWN");
    equal(ask("MLR1 ROUTE SET alpha mist eve gameServer gs-a"), "MLR1 ROUTE OK");
    auto live = http("http://"+endpoint(metrics)+"/metrics").body;
    require(live.find("mistlane_registry_routes{role=\"coordinator\"} 1") != std::string::npos,
            "route gauge did not follow the record count");
    const auto eve_at = "MLR1 ROUTE AT alpha gameServer gs-a 127.0.0.1:"+
        std::to_string(first)+" 0 ready 0/42";
    eventually([&] { return ask("MLR1 ROUTE GET mist eve") == eve_at; },
        5s, "route did not follow the live member state");
    // A second SET for the same character overwrites; the count stays at one.
    equal(ask("MLR1 ROUTE SET alpha mist eve gameServer gs-b"), "MLR1 ROUTE OK");
    const auto eve_moved = "MLR1 ROUTE AT alpha gameServer gs-b 127.0.0.1:"+
        std::to_string(second)+" 0 ready 0/42";
    eventually([&] { return ask("MLR1 ROUTE GET mist eve") == eve_moved; },
        5s, "route overwrite did not stick");
    // A registered member publishes routes on its own control leg and the leg
    // keeps heartbeating; GET mirrors the heartbeat state it announces.
    {
        RawSocket host(registry);
        send_frame(host, "MLR1 REGISTER gameServer gs-r alpha 127.0.0.1:1 0");
        equal(read_frame(host), "MLR1 OK");
        send_frame(host, "MLR1 ROUTE SET alpha mist rogue gameServer gs-r");
        equal(read_frame(host), "MLR1 ROUTE OK");
        send_frame(host, "MLR1 notready 0 42");
        equal(read_frame(host), "MLR1 OK");
        equal(ask("MLR1 ROUTE GET mist rogue"),
              "MLR1 ROUTE AT alpha gameServer gs-r 127.0.0.1:1 0 notready 0/42");
        send_frame(host, "MLR1 ready 1 42");
        equal(read_frame(host), "MLR1 OK");
        equal(ask("MLR1 ROUTE GET mist rogue"),
              "MLR1 ROUTE AT alpha gameServer gs-r 127.0.0.1:1 0 ready 1/42");
        live = http("http://"+endpoint(metrics)+"/metrics").body;
        require(live.find("mistlane_registry_routes{role=\"coordinator\"} 2") != std::string::npos,
                "route gauge missed the second record");
    } // the host retires here
    eventually([&] { return ask("MLR1 ROUTE GET mist rogue") == "MLR1 ROUTE EMPTY"; },
        5s, "a retired host kept its route");
    live = http("http://"+endpoint(metrics)+"/metrics").body;
    require(live.find("mistlane_registry_routes{role=\"coordinator\"} 1") != std::string::npos,
            "route gauge did not follow the retirement");
    // Placement beside the route verbs stays member placement.
    eventually([&] { return ask("MLR1 PLACE gameServer alpha") ==
        "MLR1 PLACE gs-a 127.0.0.1:"+std::to_string(first)+" 0 ready 0/42"; },
        5s, "placement broke beside the route verbs");
    // Identity gates routes like everything else on a tokened coordinator.
    RawSocket anonymous(tokened);
    send_frame(anonymous, "MLR1 ROUTE GET mist eve");
    equal(read_frame(anonymous), "MLS1 ERR TOKEN");
    expect_close(anonymous);
    c.node("gameServer", port(), {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(tokened), "--upstream-token", "s3cret", "--service-name", "gs-d"});
    const auto identified = [&](const std::string& line) {
        RawSocket raw(tokened);
        send_frame(raw, "MLS1 registryClient routes-1 s3cret");
        equal(read_frame(raw), "MLS1 OK");
        send_frame(raw, line);
        body = read_frame(raw);
        expect_close(raw);
        return body;
    };
    eventually([&] { return identified("MLR1 ROUTE SET main mist kate gameServer gs-d") ==
        "MLR1 ROUTE OK"; }, 5s, "an identified route publish never landed");
    body = identified("MLR1 ROUTE GET mist kate");
    require(body.starts_with("MLR1 ROUTE AT main gameServer gs-d "), "route answer wrong shape: "+body);
    require(gs_a.output().find("callback failed") == std::string::npos &&
            gs_b.output().find("callback failed") == std::string::npos,
            "registry member logged Lua failures");
    require(coord.output().find("unexpected frame") == std::string::npos, "coordinator log noise");
}
void cross_record() {
    Context c;
    write(c.path("echo.lua"), echo);
    const auto data = port(), metrics = port(), second = port(), tokened = port(), plain = port();
    c.node("data", data, {"--script", c.path("echo.lua").string(), "--metrics-listen", endpoint(metrics)});
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame + std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        std::size_t length = 0;
        for (unsigned char ch : raw.receive(4)) length = (length << 8) | ch;
        return raw.receive(length);
    };
    const auto ask = [&send_frame, &read_frame](RawSocket& raw, std::string_view payload) {
        send_frame(raw, payload);
        return read_frame(raw);
    };
    // One leg loads and submits many times over; the verdicts re-arm it.
    RawSocket leg(data);
    equal(ask(leg, "MLD1 LOAD home hero"), "MLD1 EMPTY");
    equal(ask(leg, "MLD1 SUBMIT home hero gen 5 seq 1 hp=10 mp=5"), "MLD1 RECEIVED gen 5 seq 1");
    equal(ask(leg, "MLD1 LOAD home hero"), "MLD1 STATE gen 5 seq 1 hp=10 mp=5");
    equal(ask(leg, "MLD1 SUBMIT home hero gen 5 seq 1 hp=99"), "MLD1 RECEIVED gen 5 seq 1");
    equal(ask(leg, "MLD1 LOAD home hero"), "MLD1 STATE gen 5 seq 1 hp=10 mp=5");
    equal(ask(leg, "MLD1 SUBMIT home hero gen 5 seq 3 gap"), "MLD1 ERR SEQ seq 2");
    equal(ask(leg, "MLD1 SUBMIT home hero gen 4 seq 2 late"), "MLD1 ERR STALE gen 5");
    equal(ask(leg, "MLD1 SUBMIT home hero gen 6 seq 2 taken"), "MLD1 RECEIVED gen 6 seq 2");
    equal(ask(leg, "MLD1 SUBMIT home hero gen 7 seq 9 jump"), "MLD1 ERR SEQ seq 3");
    // A record leg also carries plain script frames under the same session.
    equal(ask(leg, "ping"), "ping");
    equal(ask(leg, "MLD1 NOPE"), "MLD1 ERR INVALID");
    // Every verdict lands on the closed result label set, and nothing else.
    const auto results = [&](const char* sample) {
        const auto body = http("http://"+endpoint(metrics)+"/metrics").body;
        const auto needle = std::string("mistlane_record_results_total{role=\"data\",result=\"")
            + sample + "\"} ";
        const auto at = body.find(needle);
        require(at != std::string::npos, std::string("missing metric ")+sample);
        return std::strtod(body.c_str()+at+needle.size(), nullptr);
    };
    require(results("accepted") == 2, "accepted counter mismatch");
    require(results("loaded") == 2, "loaded counter mismatch");
    require(results("duplicate") == 1, "duplicate counter mismatch");
    require(results("seq") == 2, "seq counter mismatch");
    require(results("stale") == 1, "stale counter mismatch");
    require(results("invalid") == 1, "invalid counter mismatch");
    require(results("empty") == 1, "empty counter mismatch");
    // Records are process-local until a persistence chain exists: a fresh
    // data process answers EMPTY, never a copied state.
    c.node("data", second, {"--script", c.path("echo.lua").string()});
    {
        RawSocket fresh(second);
        equal(ask(fresh, "MLD1 LOAD home hero"), "MLD1 EMPTY");
    }
    // A tokened data node gates the record leg behind MLS1; a refused
    // handshake closes the leg, so the identified path takes a new one.
    c.node("data", tokened, {"--script", c.path("echo.lua").string(), "--service-token", "s3cret"});
    {
        RawSocket raw(tokened);
        equal(ask(raw, "MLD1 SUBMIT home hero gen 1 seq 1 x"), "MLS1 ERR TOKEN");
    }
    {
        RawSocket raw(tokened);
        equal(ask(raw, "MLS1 gameServer gs-1 s3cret"), "MLS1 OK");
        equal(ask(raw, "MLD1 SUBMIT home hero gen 1 seq 1 x"), "MLD1 RECEIVED gen 1 seq 1");
    }
    // Other roles keep MLD1 as script traffic; only data owns the endpoint.
    c.node("gameServer", plain, {"--script", c.path("echo.lua").string()});
    {
        RawSocket game(plain);
        equal(ask(game, "MLD1 LOAD home hero"), "MLD1 LOAD home hero");
    }
}
void probe_control() {
    Context c;
    write(c.path("echo.lua"), echo);
    // Capacity 2 with a 0.5 readiness gate saturates at one session; two ticks flip state.
    const auto work=port(), probe=port(), work_metrics=port(), guarded=port(), guard_probe=port();
    c.node("gameServer", work, {"--script", c.path("echo.lua").string(), "--max-connections", "2",
        "--overload-ratio", "0.5", "--ready-hysteresis", "2", "--probe-listen", endpoint(probe),
        "--metrics-listen", endpoint(work_metrics)});
    const auto ask = [](unsigned listen, const std::string& line) {
        RawSocket raw(listen);
        raw.send(line + "\n");
        return raw.receive_line();
    };
    const auto metric_value = [](const std::string& body, const std::string& sample) {
        const auto at = body.find(sample);
        require(at != std::string::npos, "missing metric " + sample);
        return std::strtod(body.c_str() + at + sample.size(), nullptr);
    };
    equal(ask(probe, "PROBE1 n1 liveness"), "PROBE1 n1 ok alive");
    equal(ask(probe, "PROBE9 n2 liveness"), "PROBE1 n2 err version");
    equal(ask(probe, "PROBE1 n3 status"), "PROBE1 n3 err command");
    equal(ask(probe, "PROBE1 n4"), "PROBE1 n4 err malformed");
    equal(ask(probe, "PROBE1 bad!n liveness"), "PROBE1 - err malformed");
    equal(ask(probe, "hello world"), "PROBE1 world err version");
    // Every ask is a fresh connection, so reply buffers and teardown must hold
    // up across repeated probe lifetimes.
    for (int i = 0; i < 50; ++i) {
        const auto nonce = "loop" + std::to_string(i);
        equal(ask(probe, "PROBE1 " + nonce + " liveness"), "PROBE1 " + nonce + " ok alive");
    }
    const auto readiness = ask(probe, "PROBE1 n5 readiness");
    require(readiness.starts_with("PROBE1 n5 ok ready active=0 capacity=2 "),
        "unexpected readiness reply: " + readiness);
    require(readiness.find(" lag_ms=") != std::string::npos, "readiness lag summary missing");
    // A probe connection that never asks must be closed by the server read deadline.
    {
        RawSocket silent(probe);
        eventually([&] { return silent.closed(); });
    }
    // Saturating flips to notready, releasing the session flips back.
    auto session = tcp(work);
    equal(session->request("busy"), "busy");
    eventually([&] {
        return ask(probe, "PROBE1 n6 readiness").find(" ok notready active=1 capacity=2 ") != std::string::npos;
    });
    // The readiness gauge must follow the admission gate, not just start/stop.
    {
        const auto busy = http("http://"+endpoint(work_metrics)+"/metrics");
        require(metric_value(busy.body, "mistlane_ready{role=\"gameServer\"} ") == 0,
            "readiness gauge must drop with the admission gate: " + busy.body);
    }
    session.reset();
    eventually([&] {
        return ask(probe, "PROBE1 n7 readiness").find(" ok ready active=0 capacity=2 ") != std::string::npos;
    });
    {
        const auto recovered = http("http://"+endpoint(work_metrics)+"/metrics");
        require(metric_value(recovered.body, "mistlane_ready{role=\"gameServer\"} ") == 1,
            "readiness gauge must recover with the admission gate: " + recovered.body);
    }
    {
        // Minimal identity check: a token env makes unauthenticated probes fail.
        ScopedEnv token("MISTLANE_PROBE_TOKEN", "s3cret");
        c.node("gameServer", guarded, {"--script", c.path("echo.lua").string(), "--max-connections", "5",
            "--probe-listen", endpoint(guard_probe)});
    }
    equal(ask(guard_probe, "PROBE1 n8 readiness"), "PROBE1 n8 err auth");
    equal(ask(guard_probe, "PROBE1 n9 wrong readiness"), "PROBE1 n9 err auth");
    const auto authenticated = ask(guard_probe, "PROBE1 n10 s3cret readiness");
    require(authenticated.starts_with("PROBE1 n10 ok ready active=0 capacity=5 "),
        "unexpected authenticated readiness: " + authenticated);

    // Cross consults shard readiness for new rooms; established rooms keep flowing.
    const auto shard=port(), shard_probe=port(), ingress=port(), ingress_metrics=port(),
        ingress2=port(), plain=port(), broken=port();
    c.node("crossServer", shard, {"--script", c.path("echo.lua").string(), "--max-connections", "4",
        "--overload-ratio", "0.5", "--ready-hysteresis", "2", "--probe-listen", endpoint(shard_probe)});
    const auto upstream = endpoint(shard) + ":" + std::to_string(shard_probe);
    c.cross_gateway( ingress, {"--upstream", upstream, "--metrics-listen", endpoint(ingress_metrics)});
    c.cross_gateway( ingress2, {"--upstream", upstream});
    const auto join = [](cli::Client& peer, const std::string& room) {
        return peer.request("MLC1 1 realm g " + room);
    };
    auto room1 = tcp(ingress), room2 = tcp(ingress2);
    equal(join(*room1, "r1"), "MLC1 OK 1");
    equal(join(*room2, "r2"), "MLC1 OK 1");
    eventually([&] {
        return ask(shard_probe, "PROBE1 nr readiness").find(" ok notready active=2 ") != std::string::npos;
    }, 5s, "shard to reach notready at active=2");
    equal(join(*tcp(ingress), "r3"), "MLC1 ERR NOT_READY");
    equal(join(*tcp(ingress2), "r4"), "MLC1 ERR NOT_READY");
    // Admission rejections include cross-gated joins, not only local limits.
    {
        const auto rejects = http("http://"+endpoint(ingress_metrics)+"/metrics");
        require(metric_value(rejects.body, "mistlane_admission_rejections_total{role=\"gateway\"} ") >= 1,
            "cross admission rejects are not counted: " + rejects.body);
    }
    room1.reset();
    eventually([&] {
        return ask(shard_probe, "PROBE1 nr2 readiness").find(" ok ready active=1 ") != std::string::npos;
    }, 5s, "shard to recover ready at active=1");
    equal(join(*tcp(ingress), "r5"), "MLC1 OK 1");
    // Without a probe port the cross node cannot gate, so it keeps forwarding.
    c.cross_gateway( plain, {"--upstream", endpoint(shard)});
    equal(join(*tcp(plain), "r7"), "MLC1 OK 1");
    // A dead probe listener is an upstream failure, not a silent accept.
    c.cross_gateway( broken, {"--upstream", endpoint(shard) + ":" + std::to_string(port())});
    equal(join(*tcp(broken), "r8"), "MLC1 ERR UPSTREAM");

    // Bracketed IPv6 listen and upstream addresses, gated and ungated.
    const auto v6_shard=port(), v6_probe=port(), v6_gated=port(), v6_plain=port();
    auto& v6_node = c.spawn({self().string(), "--role", "crossServer",
        "--listen", "[::1]:" + std::to_string(v6_shard),
        "--script", c.path("echo.lua").string(), "--max-connections", "4",
        "--overload-ratio", "0.5", "--ready-hysteresis", "2",
        "--probe-listen", "[::1]:" + std::to_string(v6_probe)});
    v6_node.ready("ready crossServer");
    c.cross_gateway( v6_gated, {"--upstream",
        "[::1]:" + std::to_string(v6_shard) + ":" + std::to_string(v6_probe)});
    equal(join(*tcp(v6_gated), "v1"), "MLC1 OK 1");
    c.cross_gateway( v6_plain, {"--upstream", "[::1]:" + std::to_string(v6_shard)});
    equal(join(*tcp(v6_plain), "v2"), "MLC1 OK 1");

    // An oversized probe request line is closed without any reply bytes.
    {
        RawSocket long_line(probe);
        long_line.send(std::string(300, 'A') + "\n");
        must_fail([&] { (void)long_line.receive_line(); });
    }
    // Concurrent probe connections are capped; rejected sockets close at once.
    {
        std::vector<std::unique_ptr<RawSocket>> held;
        for (int i = 0; i < 64; ++i) held.push_back(std::make_unique<RawSocket>(probe));
        const auto deadline = std::chrono::steady_clock::now() + 1200ms;
        bool rejected = false;
        RawSocket over(probe);
        while (std::chrono::steady_clock::now() < deadline) {
            if (over.closed()) { rejected = true; break; }
            std::this_thread::sleep_for(20ms);
        }
        require(rejected, "peer cap did not close the 65th probe connection");
        held.clear();
        eventually([&] {
            return ask(probe, "PROBE1 freed liveness") == "PROBE1 freed ok alive";
        }, 5s, "probe slots did not free after held peers closed");
    }
}
void config_file() {
    Context c;
    write(c.path("echo.lua"), echo);
    const auto config = c.path("gameServer.env");
    const auto listen = port();
    write(config, "# mistlane-config 1\nROLE=gameServer\nLISTEN=" + endpoint(listen) +
        "\nSCRIPT=echo.lua\nTRANSPORT=tcp\nMAX_CONNECTIONS=5\n");
    {
        auto& node = c.spawn({self().string(), "--config", config.string()});
        node.ready("ready gameServer " + endpoint(listen));
        require(node.output().find("session admission limit 5") != std::string::npos,
            "config max_connections missing: " + node.output());
        equal(tcp(listen)->request("hello"), "hello");
        node.signal(SIGTERM);
        require(node.wait() == 0, "config node should stop cleanly");
    }
    {
        // A version header is required so a random shell env file is not consumed silently.
        write(config, "ROLE=gameServer\nLISTEN=" + endpoint(listen) + "\n");
        auto& bad = c.spawn({self().string(), "--config", config.string()});
        require(bad.wait() == 1, "missing version line must fail");
        require(bad.output().find("mistlane-config 1") != std::string::npos,
            "version error missing: " + bad.output());
    }
    {
        write(config, "# mistlane-config 1\nNOPE=1\n");
        auto& bad = c.spawn({self().string(), "--config", config.string()});
        require(bad.wait() == 1, "unknown key must fail");
        require(bad.output().find("unknown key NOPE") != std::string::npos,
            "unknown key error missing: " + bad.output());
    }
    {
        write(config, "# mistlane-config 1\nMAX_CONNECTIONS=5\nMAX_CONNECTIONS=6\n");
        auto& bad = c.spawn({self().string(), "--config", config.string()});
        require(bad.wait() == 1, "duplicate key must fail");
        require(bad.output().find("duplicate key MAX_CONNECTIONS") != std::string::npos,
            "duplicate key error missing: " + bad.output());
    }
    {
        // Environment beats the file; an explicit flag beats the environment.
        write(config, "# mistlane-config 1\nROLE=gameServer\nLISTEN=" + endpoint(listen) +
            "\nSCRIPT=echo.lua\nTRANSPORT=tcp\nMAX_CONNECTIONS=5\n");
        ScopedEnv around("MISTLANE_MAX_CONNECTIONS", "7");
        auto& node = c.spawn({self().string(), "--config", config.string()});
        node.ready("ready gameServer " + endpoint(listen));
        require(node.output().find("session admission limit 7") != std::string::npos,
            "environment override missing: " + node.output());
        node.signal(SIGTERM);
        require(node.wait() == 0, "overridden node should stop cleanly");
    }
    {
        auto& node = c.spawn({self().string(), "--config", config.string(), "--max-connections", "3"});
        node.ready("ready gameServer " + endpoint(listen));
        require(node.output().find("session admission limit 3") != std::string::npos,
            "flag override missing: " + node.output());
        node.signal(SIGTERM);
        require(node.wait() == 0, "flag node should stop cleanly");
    }
    {
        // ROOT keeps scaffold-style project-root-relative entries readable.
        const auto root = c.path("app");
        fs::create_directory(root);
        write(root / "echo.lua", echo);
        const auto rooted_port = port();
        write(c.path("rooted.env"), "# mistlane-config 1\nROOT=app\nROLE=gameServer\nLISTEN=" +
            endpoint(rooted_port) + "\nSCRIPT=echo.lua\n");
        auto& node = c.spawn({self().string(), "--config", c.path("rooted.env").string()});
        node.ready("ready gameServer " + endpoint(rooted_port));
        equal(tcp(rooted_port)->request("ping"), "ping");
        node.signal(SIGTERM);
        require(node.wait() == 0, "rooted node should stop cleanly");
    }
    {
        const auto metrics = port(), probe = port();
        write(c.path("diag.env"), "# mistlane-config 1\nROLE=gameServer\nLISTEN=" + endpoint(port()) +
            "\nSCRIPT=echo.lua\nMETRICS_LISTEN=" + endpoint(metrics) + "\nPROBE_LISTEN=" + endpoint(probe) + "\n");
        auto& node = c.spawn({self().string(), "--config", c.path("diag.env").string()});
        node.ready("ready gameServer");
        { RawSocket raw(probe); raw.send("PROBE1 cf liveness\n"); equal(raw.receive_line(), "PROBE1 cf ok alive"); }
        const auto scrape = http("http://"+endpoint(metrics)+"/metrics");
        require(scrape.status == 200 && scrape.body.find("mistlane_messages_total") != std::string::npos,
            "config metrics listener missing");
        node.signal(SIGTERM);
        require(node.wait() == 0, "diagnostics node should stop cleanly");
    }
}
void contract_tool() {
    Context c;
    const std::string avatar = R"(<Contracts>
  <Types>
    <Item><itemId id="0" type="UINT32"/><count id="1" type="UINT32" default="1"/></Item>
    <Inventory><capacity id="0" type="UINT16" default="20"/><items id="1" type="ARRAY" of="Item"/></Inventory>
  </Types>
  <Entity name="Avatar" id="1">
    <Properties>
      <level id="1" type="UINT16" default="1" flags="OWN_CLIENT" persistent="true"/>
      <gold id="2" type="INT64" default="-5" flags="OWN_CLIENT" persistent="true"/>
      <inventory id="3" type="Inventory" flags="OWN_CLIENT"/>
      <active id="4" type="BOOL" default="true"/>
    </Properties>
    <ServerMethods><Method name="attack" id="1" mode="notify" caller="owner">
      <Arg name="target" id="0" type="EntityRef"/>
    </Method><Method name="getProfile" id="2" mode="request" caller="server">
      <Returns><Field name="level" id="0" type="UINT16"/></Returns>
    </Method></ServerMethods>
    <ClientMethods><Method name="onReward" id="1" mode="notify" audience="owner"/></ClientMethods>
  </Entity>
</Contracts>)";
    const auto good = c.path("avatar.def");
    write(good, avatar);
    require(c.command({self().string(), "contract", "check", good.string()}) == 0,
            "check rejected a valid document");
    const auto broken = c.path("broken.def");
    write(broken, std::string("<Contracts><Entity name=\"A\" id=\"1\">"
        "<Properties><x id=\"1\" type=\"WAT\"/></Properties></Entity></Contracts>"));
    auto& rejected = c.spawn({self().string(), "contract", "check", broken.string()});
    require(rejected.wait() != 0, "check accepted an invalid document");
    require(rejected.output().find("neither built-in") != std::string::npos,
            "check did not name the failing rule: "+rejected.output());
    // build emits the three generators, byte-for-byte reproducible.
    const auto one = c.path("bundle");
    require(c.command({self().string(), "contract", "build", "--out", one.string(), good.string()}) == 0,
            "build failed on a valid document");
    for (const auto& name : {"avatar.xsd", "avatar.lock", "avatar.digest", "avatar.sproto"})
        require(fs::is_regular_file(one/name), std::string("missing artifact: ")+name);
    const auto two = c.path("rebuild");
    require(c.command({self().string(), "contract", "build", "--out", two.string(), good.string()}) == 0,
            "rebuild failed");
    equal(read(one/"avatar.xsd"), read(two/"avatar.xsd"));
    equal(read(one/"avatar.lock"), read(two/"avatar.lock"));
    equal(read(one/"avatar.digest"), read(two/"avatar.digest"));
    equal(read(one/"avatar.sproto"), read(two/"avatar.sproto"));
    require(read(one/"avatar.sproto").find("E1_Server_1 ") != std::string::npos &&
            read(one/"avatar.sproto").find("E1_Client_1 ") != std::string::npos &&
            read(one/"avatar.sproto").find("items 1 : *Item") != std::string::npos &&
            read(one/"avatar.sproto").find("active 4 : boolean") != std::string::npos &&
            read(one/"avatar.sproto").find("E1_Server_2 131074") != std::string::npos,
            "sproto schema lost stable E1 side/method identifiers");
    // The lock's digest line, the digest file and the library digest agree.
    const auto lock_text = read(one/"avatar.lock");
    const auto at = lock_text.find("# digest ");
    require(at != std::string::npos, "lock lost its digest line: "+lock_text);
    equal(lock_text.substr(at+9, 16), read(one/"avatar.digest").substr(0, 16));
    std::ostringstream hex;
    hex << std::hex << std::setw(16) << std::setfill('0') << EntityContract(avatar).digest();
    equal(hex.str(), read(one/"avatar.digest").substr(0, 16));
    require(read(one/"avatar.xsd").find("name=\"Avatar\"") != std::string::npos,
            "schema lost the entity");
    // A failed build writes nothing, so a broken change cannot clobber a bundle.
    const auto before_broken = c.path("before-broken");
    require(c.command({self().string(), "contract", "build", "--out", before_broken.string(),
                       good.string(), broken.string()}) != 0, "build accepted a broken document");
    require(!fs::exists(before_broken) || fs::is_empty(before_broken),
            "a failed build must not write artifacts");
    // Usage errors are non-zero.
    require(c.command({self().string(), "contract"}) != 0, "missing verb accepted");
    require(c.command({self().string(), "contract", "frobnicate", good.string()}) != 0,
            "unknown verb accepted");
    require(c.command({self().string(), "contract", "build", good.string()}) != 0,
            "build without --out accepted");
}
void sproto_golden() {
    // Fixture-only canonical TLV vectors. These intentionally are NOT sproto
    // wire bytes; they lock value shape/widths until an upstream codec replaces
    // this adapter. Each record is tag:u8, kind:u8, length:u16be, bytes.
    auto record = [](unsigned tag, unsigned kind, std::string_view value) {
        std::string out{static_cast<char>(tag), static_cast<char>(kind),
            static_cast<char>((value.size() >> 8) & 255), static_cast<char>(value.size() & 255)};
        out.append(value); return out;
    };
    auto hex = [](std::string_view bytes) {
        constexpr char digits[]="0123456789abcdef"; std::string out;
        for (unsigned char b : bytes) { out.push_back(digits[b>>4]); out.push_back(digits[b&15]); }
        return out;
    };
    const auto scalar = record(0, 1, std::string("\x7f\xff\xff\xff", 4));
    const auto array = record(1, 2, std::string("\x00\x02\x00\x00\xff\xff", 6));
    const auto nested = record(2, 3, record(0, 1, std::string("\x00\x00\x00\x01", 4)));
    const auto boundary = record(3, 1, std::string("\x80\x00\x00\x00", 4));
    equal(hex(scalar), "000100047fffffff");
    equal(hex(array), "0102000600020000ffff");
    equal(hex(nested), "020300080001000400000001");
    equal(hex(boundary), "0301000480000000");
    // The length parser is the decoding seam: decode and re-encode exactly.
    auto roundtrip = [&](const std::string& bytes) {
        require(bytes.size() >= 4, "truncated vector header");
        const auto len = (static_cast<unsigned char>(bytes[2]) << 8) |
            static_cast<unsigned char>(bytes[3]);
        require(bytes.size() == len + 4, "vector length mismatch");
        return record(static_cast<unsigned char>(bytes[0]), static_cast<unsigned char>(bytes[1]),
                      std::string_view(bytes).substr(4));
    };
    equal(roundtrip(scalar), scalar); equal(roundtrip(array), array);
    equal(roundtrip(nested), nested); equal(roundtrip(boundary), boundary);
}

void scaffold_init() {
    Context c; const auto root=c.path("sample service");
    require(c.command({self().string(),"init",root.string()})==0,"initializer failed");
    for (const auto& name : {"scripts/echo.lua","scripts/http.lua","scripts/cross.lua",
        "config/instances/gateway.env","config/instances/gateway-ws.env","config/instances/gateway-wss.env",
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
    {
        ScopedEnv real_binary("MISTLANE_BIN",self().string());
        const auto listen=port();
        write(root/"config/instances/gameServer.env","ROLE=gameServer\nLISTEN="+endpoint(listen)+"\nSCRIPT=scripts/echo.lua\nTRANSPORT=tcp\nUPSTREAMS=\nROUTE_EPOCH=\n");
        auto& node=c.spawn({(root/"ops/run-instance.sh").string(),"gameServer"});
        node.ready(); equal(tcp(listen)->request("template-path-check"),"template-path-check");
        node.signal(SIGTERM); require(node.wait()==0,"generated gameServer shutdown failed");
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
void cli_bot() {
    Context c; write(c.path("echo.lua"),echo);
    const auto listen=port(); c.node("gameServer",listen,{"--script",c.path("echo.lua").string()});
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
