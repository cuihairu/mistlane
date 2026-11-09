// SPDX-License-Identifier: Apache-2.0
#include "mistlane/api.hpp"
#include "mistlane/node.hpp"
#include "mistlane/cross.hpp"
#include "mistlane/diagnostics.hpp"
#include "mistlane/lua_service.hpp"
#include "mistlane/token_bucket.hpp"
#include "mistlane/transport.hpp"
#include "mistlane/roles.hpp"
#include "mistlane/record.hpp"
#include "mistlane/migration.hpp"
#include "mistlane/journal.hpp"
#include "mistlane/api_store.hpp"
#include "mistlane/entity_contract.hpp"
#include "mistlane/entity_codec.hpp"
#include "mistlane/entity_endpoint.hpp"
#include "mistlane/dashboard.hpp"
#include "node_internal.hpp"
#include "observability.hpp"
#include "tcp_channel.hpp"
#include "return_sender.hpp"
#include "registry_service.hpp"
#include "mistlane/entity_gateway.hpp"
#include <openssl/crypto.h>
#include <utility>
#include <boost/asio.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <deque>
#include <map>
#include <limits>
#include <optional>
#include <memory>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mistlane {
namespace {
namespace fs = std::filesystem;
using Completion = Channel::Completion;
// Replaces the old mixed TCP/WebSocket framing. Wire format: u32 BE + payload.
// Correlated sessions (MLC2) carry a u32 BE request id inside the frame
// payload; pushes keep id 0 so clients can tell solicitations from replies.
std::string correlated_frame(std::uint64_t request, std::string_view payload) {
    std::string frame(4, '\0');
    auto id = request;
    for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(id & 255); id >>= 8; }
    frame.append(payload);
    return frame;
}
// MLA1 credential rows, ';'-separated: "principal:key_id:<64 lowercase hex
// secret>:realm[,realm...][:until]". The optional tail is the rotation bound
// as decimal epoch seconds (0 or absent = unbounded). Injected through the
// environment so no loader, CLI secret flag or real credential ships inside
// the framework.
std::vector<ApiCredential> api_credentials(std::string_view text) {
    std::vector<ApiCredential> credentials;
    auto row = text;
    while (!row.empty()) {
        const auto end = row.find(';');
        const auto body = row.substr(0, end);
        row = end == std::string_view::npos ? std::string_view{} : row.substr(end+1);
        const auto first = body.find(':');
        const auto second = first == std::string_view::npos ? std::string::npos : body.find(':', first+1);
        const auto third = second == std::string::npos ? std::string::npos : body.find(':', second+1);
        if (first == std::string_view::npos || second == std::string_view::npos || third == std::string::npos)
            throw std::invalid_argument("MLA1 credential row needs principal:key_id:secret:realms");
        ApiCredential credential;
        credential.principal = body.substr(0, first);
        credential.key_id = body.substr(first+1, second-first-1);
        const auto fourth = third == std::string::npos ? std::string::npos : body.find(':', third+1);
        const auto hex = body.substr(second+1, third-second-1);
        const auto digit = [hex](std::size_t at) {
            const char ch = hex[at];
            if (ch >= '0' && ch <= '9') return static_cast<int>(ch-'0');
            if (ch >= 'a' && ch <= 'f') return static_cast<int>(ch-'a'+10);
            throw std::invalid_argument("MLA1 credential secret must be 64 lowercase hex digits");
        };
        if (hex.size() != 64)
            throw std::invalid_argument("MLA1 credential secret must be 64 lowercase hex digits");
        for (std::size_t i = 0; i < hex.size(); i += 2)
            credential.secret[i/2] = static_cast<unsigned char>(digit(i)*16 + digit(i+1));
        auto realms = body.substr(third+1, fourth == std::string_view::npos
            ? std::string_view::npos : fourth-third-1);
        while (!realms.empty()) {
            const auto comma = realms.find(',');
            credential.realms.emplace(realms.substr(0, comma));
            realms = comma == std::string_view::npos ? std::string_view{} : realms.substr(comma+1);
        }
        if (fourth != std::string_view::npos) {
            const auto until = body.substr(fourth+1);
            const auto parsed = std::from_chars(until.data(), until.data()+until.size(), credential.valid_until);
            if (until.empty() || parsed.ec != std::errc() || parsed.ptr != until.data()+until.size())
                throw std::invalid_argument("MLA1 credential rotation bound must be decimal seconds");
        }
        credentials.push_back(std::move(credential));
    }
    return credentials;
}
// MLT1 per-realm secrets, comma-separated "realm:<64 lowercase hex>" rows.
// Injected through the environment like the MLA1 credentials: no loader, CLI
// secret flag or real secret ships inside the framework.
std::map<std::string, ApiSecret> ticket_secrets(std::string_view text) {
    std::map<std::string, ApiSecret> secrets;
    auto row = text;
    while (!row.empty()) {
        const auto end = row.find(',');
        const auto body = row.substr(0, end);
        row = end == std::string_view::npos ? std::string_view{} : row.substr(end+1);
        const auto colon = body.find(':');
        if (colon == std::string_view::npos)
            throw std::invalid_argument("MISTLANE_TICKET_SECRETS row needs realm:secret");
        const auto realm = std::string(body.substr(0, colon));
        const auto hex = body.substr(colon+1);
        if (hex.size() != 64)
            throw std::invalid_argument("MISTLANE_TICKET_SECRETS secret must be 64 lowercase hex digits");
        const auto digit = [hex](std::size_t at) {
            const char ch = hex[at];
            if (ch >= '0' && ch <= '9') return static_cast<int>(ch-'0');
            if (ch >= 'a' && ch <= 'f') return static_cast<int>(ch-'a'+10);
            throw std::invalid_argument("MISTLANE_TICKET_SECRETS secret must be 64 lowercase hex digits");
        };
        ApiSecret secret{};
        for (std::size_t i = 0; i < hex.size(); i += 2)
            secret[i/2] = static_cast<unsigned char>(digit(i)*16 + digit(i+1));
        if (!record_identifier(realm))
            throw std::invalid_argument("MISTLANE_TICKET_SECRETS realm must be an identifier");
        if (!secrets.emplace(realm, secret).second)
            throw std::invalid_argument("MISTLANE_TICKET_SECRETS names a realm twice");
    }
    return secrets;
}
// The reason stays on the operator's side of the log; the join answers one word.
std::string_view ticket_verdict_name(TicketVerdict verdict) {
    switch (verdict) {
        case TicketVerdict::Ok: return "ok";
        case TicketVerdict::Malformed: return "malformed";
        case TicketVerdict::BadSignature: return "bad-signature";
        case TicketVerdict::Expired: return "expired";
        case TicketVerdict::WrongInstance: return "wrong-instance";
        case TicketVerdict::WrongCharacter: return "wrong-character";
        case TicketVerdict::WrongGeneration: return "wrong-generation";
        case TicketVerdict::Replay: return "replay";
        case TicketVerdict::Full: return "claim-table-full";
        case TicketVerdict::UnknownRealm: return "unknown-realm";
        case TicketVerdict::Clock: return "clock";
    }
    return "unknown";
}
// Envelope of a correlated or multiplexed frame: the first four bytes are a
// u32 BE id. Zero and "short" both mean "no id", so a truncated frame is
// reported by the caller instead of silently becoming id 0.
std::uint32_t frame_id(const std::string& payload) {
    if (payload.size() < 4) return 0;
    std::uint32_t id = 0;
    for (int i = 0; i < 4; ++i) id = (id << 8) | static_cast<unsigned char>(payload[i]);
    return id;
}
// Entity frames are directional: a request carries its correlation inside the
// envelope at frame bytes [27,35) (the MLE1 marker plus scope, type, entity,
// incarnation, side and method), while a reply carries it directly after the
// marker at [4,12). The gateway rewrites the matching slice per direction, so
// request and reply stay opaque to the relay.
constexpr std::size_t kEntityRequestCorrelationAt = 4 + 23;
constexpr std::size_t kEntityReplyCorrelationAt = 4;
// The marker plus the correlation slice being rewritten is all a frame needs;
// a malformed request (too short to be a valid envelope) still carries its
// correlation bytes and passes through like any other frame. A correlated
// (MLC2) frame carries a 4-byte transport id before the payload, so entity
// traffic through such a session has its MLE1 marker at offset 4. Return the
// marker offset (0 or 4) for entity traffic, else npos.
constexpr std::size_t kEntityMlc2Prefix = 4;
std::size_t entity_frame_offset(std::string_view frame) {
    if (frame.starts_with("MLE1")) return 0;
    if (frame.size() >= kEntityMlc2Prefix
            && frame.substr(kEntityMlc2Prefix).starts_with("MLE1")) return kEntityMlc2Prefix;
    return std::string_view::npos;
}
std::uint64_t entity_correlation(std::string_view frame, std::size_t at) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i)
        value = (value << 8) | static_cast<unsigned char>(frame[at + i]);
    return value;
}
void set_entity_correlation(std::string& frame, std::size_t at, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i)
        frame[at + i] = static_cast<char>(value >> (8 * (7 - i)));
}
// Framed game sessions of a script role, addressable by Lua through
// mistlane.session. Single-threaded like every other node component; session
// ids are assigned per node and unrelated to websocket lifecycle ids.
class GameSessions : public GameSessionSink {
public:
    using Failure = std::function<void()>;
    std::uint64_t attach(std::shared_ptr<Channel> channel, Failure fail) {
        const auto id = next_id_++;
        sessions_.emplace(id, Entry{std::move(channel), std::move(fail)});
        return id;
    }
    void detach(std::uint64_t id) { sessions_.erase(id); }
    // Flips once the session completes the MLC2 handshake; pushes then carry
    // the id-0 prefix so the client can tell them from replies.
    void set_correlated(std::uint64_t id) {
        const auto found = sessions_.find(id);
        if (found != sessions_.end()) found->second.correlated = true;
    }
private:
    struct Entry { std::shared_ptr<Channel> channel; Failure fail; bool correlated = false; };
    std::string session_send(std::uint64_t id, std::string_view payload) override {
        const auto found = sessions_.find(id);
        if (found == sessions_.end()) return "unknown session id";
        const auto& entry = found->second;
        const bool oversize = entry.correlated ? payload.size()+4 > max_frame : payload.size() > max_frame;
        if (oversize) return "payload exceeds 1 MiB";
        // Refusals report synchronously before write() returns, so the returned
        // string carries them; a write that fails later runs after this stack
        // frame is gone, so the callback writes into a shared reason owned by
        // the capture instead of a destroyed local.
        auto fail = entry.fail;
        auto reason = std::make_shared<std::string>();
        entry.channel->write(entry.correlated ? correlated_frame(0, payload) : std::string(payload),
            [reason, fail](Error ec) {
            if (!ec) return;
            if (ec == asio::error::message_size) { *reason = "session backlogged"; return; }
            if (ec == asio::error::operation_aborted) { *reason = "unknown session id"; return; }
            fail();
        });
        return *reason;
    }
    std::size_t session_broadcast(std::string_view payload) override {
        // Both channel types refuse synchronously; success completes
        // asynchronously on TCP and synchronously on the websocket bridge, so
        // "accepted" means "not refused by the time write() returned".
        std::size_t queued = 0;
        for (const auto& [id, entry] : sessions_) {
            (void)id;
            const bool oversize = entry.correlated ? payload.size()+4 > max_frame : payload.size() > max_frame;
            if (oversize) continue;
            auto accepted = std::make_shared<bool>(true);
            entry.channel->write(entry.correlated ? correlated_frame(0, payload) : std::string(payload),
                [accepted, fail = entry.fail](Error ec) {
                if (!ec) return;
                if (ec == asio::error::message_size) { *accepted = false; return; }
                fail(); // a dead connection retires its session
            });
            if (*accepted) ++queued;
        }
        return queued;
    }
    std::map<std::uint64_t, Entry> sessions_;
    std::uint64_t next_id_ = 1;
};
// Gateway side of MLM1: one upstream connection carries every logical session
// that shares an endpoint, so a busy gateway stops opening a socket per client.
// After MLS1 the gateway asks for "MLM1" and the peer answers "MLM1 OK"; every
// later frame is "u32 BE id | payload", and id 0 carries the "CLOSE <id>"
// control. Ids are gateway-assigned and 0 is kept for the control plane.
class MuxLink : public std::enable_shared_from_this<MuxLink> {
public:
    struct Logical {
        std::function<void(std::string)> deliver; // one frame addressed to this session
        std::function<void()> close;              // this session is gone
    };
    MuxLink(const Endpoint& endpoint, asio::io_context& io, const Config& config,
            Diagnostics& diagnostics)
      : endpoint_(tcp::endpoint(parse_address(endpoint.address), endpoint.port)),
        io_(io), config_(config), diagnostics_(diagnostics), timer_(io) {}
    bool alive() const { return started_ && !dead_; }
    std::uint32_t attach(Logical logical) {
        const std::uint32_t id = next_id_++;
        logicals_[id] = std::move(logical);
        if (!started_) start_connect();
        return id;
    }
    // A detaching session tells the peer so it can retire the logical session
    // and stop counting it; the last one drops the shared connection, which the
    // peer sees as a read failure.
    void detach(std::uint32_t id) {
        if (id == 0 || logicals_.erase(id) == 0) return;
        if (logicals_.empty()) return teardown();
        if (phase_ == Phase::ready)
            write_enveloped(0, "CLOSE " + std::to_string(id), [](Error) {});
    }
    // Refusals are synchronous (like TcpChannel), so a caller can report them
    // straight to Lua; everything else settles on the event loop.
    void send(std::uint32_t id, std::string payload, Completion completion) {
        if (dead_ || id == 0) return completion(asio::error::not_connected);
        if (payload.size() + 4 > max_frame) return completion(asio::error::message_size);
        if (queued_ + payload.size() + 8 > write_backlog) return completion(asio::error::message_size);
        if (phase_ != Phase::ready) {
            queued_ += payload.size();
            outbox_.push_back({id, std::move(payload), std::move(completion)});
            return;
        }
        write_enveloped(id, std::move(payload), std::move(completion));
    }
private:
    enum class Phase { connecting, identifying, switching, ready };
    void start_connect() {
        dead_ = false;
        started_ = true;
        phase_ = Phase::connecting;
        channel_ = std::make_shared<TcpChannel>(tcp::socket(io_));
        channel_->start([self=shared_from_this()](Error) { self->pump(); });
    }
    void pump() {
        if (dead_) return;
        if (!channel_) return;
        if (phase_ == Phase::connecting) return connect();
        if (reading_) return;
        reading_ = true;
        channel_->read([self=shared_from_this()](Error ec, std::string payload) {
            self->reading_ = false;
            if (self->dead_) return;
            if (ec) return self->fail(ec.message().c_str());
            self->dispatch(std::move(payload));
        });
    }
    void connect() {
        channel_->socket().async_connect(endpoint_,
          [self=shared_from_this()](Error ec) {
            if (self->dead_) return;
            if (ec) return self->fail("upstream connect");
            // Only identified peers may upgrade, so the token always leads; a
            // tokenless link goes straight to the mode switch.
            self->phase_ = self->config_.upstream_token.empty() ? Phase::switching : Phase::identifying;
            self->deadline();
            if (self->phase_ == Phase::identifying)
                self->write_raw(service_hello(self->config_), [self](Error) { self->pump(); });
            else
                self->write_raw("MLM1", [self](Error) { self->pump(); });
        });
    }
    void dispatch(std::string payload) {
        if (phase_ == Phase::identifying) {
            if (payload != "MLS1 OK") return fail(payload.c_str());
            phase_ = Phase::switching;
            deadline();
            return write_raw("MLM1", [self=shared_from_this()](Error) { self->pump(); });
        }
        if (phase_ == Phase::switching) {
            if (payload != "MLM1 OK") return fail(payload.c_str());
            phase_ = Phase::ready;
            deadline();
            flush();
            return pump();
        }
        if (payload.size() < 4) return teardown(); // no id, no routing possible
        const auto id = frame_id(payload);
        payload.erase(0, 4);
        if (id == 0) return control(std::move(payload)); // re-arms or tears down
        const auto found = logicals_.find(id);
        if (found == logicals_.end()) {
            std::cerr << "unexpected frame\n";
            return teardown();
        }
        found->second.deliver(std::move(payload));
        pump(); // keep reading: deliver alone never touches the socket
    }
    // "CLOSE <id>": the peer retired a logical session. The session's own
    // detach is a no-op because the entry is already gone.
    void control(std::string payload) {
        constexpr std::string_view magic = "CLOSE ";
        if (!payload.starts_with(magic)) {
            std::cerr << "unexpected frame\n";
            return teardown();
        }
        char* end = nullptr;
        const auto target = std::strtoul(payload.data()+magic.size(), &end, 10);
        if (end == payload.data() || *end != '\0' || target == 0) {
            std::cerr << "unexpected frame\n";
            return teardown();
        }
        const auto found = logicals_.find(target);
        if (found == logicals_.end()) {
            std::cerr << "unexpected frame\n";
            return teardown();
        }
        auto close = std::move(found->second.close);
        logicals_.erase(found);
        close();
        if (logicals_.empty()) return teardown();
        pump();
    }
    void flush() {
        while (!outbox_.empty()) {
            auto pending = std::move(outbox_.front());
            outbox_.pop_front();
            queued_ -= pending.payload.size();
            send(pending.id, std::move(pending.payload), std::move(pending.completion));
        }
    }
    void write_raw(std::string payload, Completion completion) {
        channel_->write(std::move(payload), [self=shared_from_this(), cb=std::move(completion)](Error ec) mutable {
            cb(ec);
            self->pump();
        });
    }
    void write_enveloped(std::uint32_t id, std::string payload, Completion completion) {
        channel_->write(correlated_frame(id, payload),
          [self=shared_from_this(), cb=std::move(completion)](Error ec) mutable {
            if (!ec) self->deadline();
            cb(ec);
            self->pump();
        });
    }
    void deadline(int seconds = 10) {
        const auto generation = ++generation_;
        timer_.expires_after(std::chrono::seconds(seconds));
        timer_.async_wait([self=shared_from_this(), generation](Error ec) {
            if (!ec && self->generation_ == generation) {
                self->diagnostics_.upstream_timeout();
                self->fail("handshake timeout");
            }
        });
    }
    void fail(const char* why) {
        diagnostics_.upstream_error();
        std::cerr << "multiplex link failed: " << why << '\n';
        teardown();
    }
    void teardown() {
        if (dead_) return;
        dead_ = true;
        started_ = false;
        auto failed = std::move(outbox_);
        queued_ = 0;
        timer_.cancel();
        if (channel_) { channel_->close(); channel_ = nullptr; }
        for (auto& pending : failed) pending.completion(asio::error::not_connected);
        auto logicals = std::move(logicals_);
        logicals_.clear();
        for (auto& [id, logical] : logicals) {
            (void)id;
            if (logical.close) logical.close();
        }
        if (on_close_) on_close_(this);
    }
    struct Pending { std::uint32_t id; std::string payload; Completion completion; };
    tcp::endpoint endpoint_;
    asio::io_context& io_;
    const Config& config_;
    Diagnostics& diagnostics_;
    asio::steady_timer timer_;
    std::shared_ptr<TcpChannel> channel_;
    std::unordered_map<std::uint32_t, Logical> logicals_;
    std::deque<Pending> outbox_;
    std::size_t queued_ = 0;
    std::uint32_t next_id_ = 1;
    Phase phase_ = Phase::connecting;
    std::uint64_t generation_ = 0;
    bool started_ = false, reading_ = false, dead_ = false;
    std::function<void(MuxLink*)> on_close_;
};
// One shared link per upstream endpoint; sessions look their endpoint up so a
// dead link is replaced lazily instead of by a timer.
class MuxLinks {
public:
    explicit MuxLinks(asio::io_context& io) : io_(io) {}
    std::shared_ptr<MuxLink> get(const Endpoint& endpoint, const Config& config,
                                 Diagnostics& diagnostics) {
        const auto found = links_.find(&endpoint);
        if (found != links_.end() && found->second->alive()) return found->second;
        auto link = std::make_shared<MuxLink>(endpoint, io_, config, diagnostics);
        links_[&endpoint] = link;
        return link;
    }
private:
    asio::io_context& io_;
    std::unordered_map<const Endpoint*, std::shared_ptr<MuxLink>> links_;
};
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(std::shared_ptr<Channel> client, std::shared_ptr<LuaService>& service,
            const Endpoint* upstream, const Config& config, CrossAdmission& admission,
            Diagnostics& diagnostics, GameSessions* registry,
            std::function<void(Session*)> on_close, MuxLinks* mux_links = nullptr,
            std::function<void(std::shared_ptr<Channel>)> mux_upgrade = {},
            bool internal = false, RecordStore* records = nullptr,
            const TicketVerifier* tickets = nullptr, EntityEndpoint* entities = nullptr,
            GatewayEntityCorrelation* gateway = nullptr, std::uint64_t connection = 0)
      : client_(std::move(client)), service_(service),
        timer_(client_->executor()), config_(config), admission_(admission), diagnostics_(diagnostics),
        registry_(registry), on_close_(std::move(on_close)), mux_upgrade_(std::move(mux_upgrade)),
        internal_(internal), records_(records), ticket_verifier_(tickets), entities_(entities),
        gateway_(gateway), connection_(connection) {
        if (upstream) {
            endpoint_ = tcp::endpoint(parse_address(upstream->address), upstream->port);
            // Multiplexing keeps one socket per upstream: the link owns the
            // connection and this session rides it under an assigned id.
            if (mux_links) link_ = mux_links->get(*upstream, config_, diagnostics_);
            else upstream_ = std::make_shared<TcpChannel>(tcp::socket(client_->executor()));
        }
    }
    void start() {
        // The link connection opens with the first attached session, and its
        // verdict may arrive before the client sends anything.
        if (link_)
            mux_id_ = link_->attach({
                [self=shared_from_this()](std::string payload) { self->forward_to_client(std::move(payload)); },
                [self=shared_from_this()] { self->stop(); }});
        client_->start([self=shared_from_this()](Error ec) {
            if (self->closed_) return;
            if (ec) return self->stop();
            // Identity exists before the first frame so Lua can push to the
            // session at any time.
            if (self->registry_)
                self->context_.session = self->registry_->attach(self->client_,
                    [self] { if (!self->closed_) self->stop(); });
            self->pump_client(10);
        });
    }
    void stop() {
        if (closed_) return;
        closed_ = true;
        timer_.cancel();
        client_->close();
        if (upstream_) upstream_->close();
        if (link_) link_->detach(mux_id_);
        if (gateway_) gateway_->drop(connection_); // no reply can match a gone client
        if (registry_ && context_.session) registry_->detach(context_.session);
        on_close_(this);
    }
private:
    void deadline(int seconds) {
        auto generation = ++generation_;
        timer_.expires_after(std::chrono::seconds(seconds));
        timer_.async_wait([self=shared_from_this(), generation](Error ec) {
            if (!ec && self->generation_ == generation) {
                if (self->upstream_) self->diagnostics_.upstream_timeout();
                self->stop();
            }
        });
    }
    // Client leg: frames arrive independently of replies, so pipelining is
    // allowed and the frame rate stays the only request-side bound. One frame
    // read at a time; writers re-arm it through this idempotent pump.
    void pump_client(int deadline_seconds = 60) {
        if (closed_ || client_reading_) return;
        client_reading_ = true;
        deadline(deadline_seconds);
        client_->read([self=shared_from_this()](Error ec, std::string payload) {
            self->client_reading_ = false;
            if (self->closed_) return;
            if (ec || !self->bucket_.consume()) return self->stop();
            self->diagnostics_.add_message("rx");
            self->diagnostics_.add_payload_bytes("rx", payload.size());
            self->request_started_ = std::chrono::steady_clock::now();
            self->route(std::move(payload));
        });
    }
    void route(std::string payload) {
        // Record frames belong to the framework at any point of the session:
        // the data role is the home side of the cross return protocol, and
        // the fence plus the sequence discipline live here, not in the
        // script. Identity still comes first — a tokened node refuses MLD1
        // like any other frame until MLS1 is presented (a logical session
        // rides the hub's identity and is not asked again).
        if (records_ && payload.starts_with("MLD1")) {
            if (!internal_ && !config_.service_token.empty() && !service_ok_)
                return refuse_service("MLS1", "TOKEN");
            return serve_record(std::move(payload));
        }
        // Handshake window: MLS1 identifies an internal peer, MLC2 turns on
        // correlation and MLM1 upgrades an identified leg to the shared link,
        // all only until the first business payload routes. A logical session
        // inside a hub reuses the hub's identity, so it keeps only MLC2.
        // Cross routing keeps its own MLC1 join as the identity; a plain
        // gateway relays MLC2 upstream like any other frame.
        if (!routed_ && !config_.is_cross_gateway() && config_.role != "crossServer") {
            if (!internal_) {
                // Nobody upgrades from the client side: the shared leg belongs
                // to the hub, which answers for every logical session it owns.
                if (payload == "MLM1") {
                    if (!service_ok_ || !mux_upgrade_) return refuse_service("MLM1", "UNEXPECTED");
                    return become_mux();
                }
                if (!config_.service_token.empty() && !service_ok_) {
                    // The first frame must carry the identity; afterwards the
                    // session is business traffic (MLC2 may stack on acceptance).
                    if (!payload.starts_with("MLS1")) return refuse_service("MLS1", "TOKEN");
                    std::string role, instance, token;
                    if (!service_fields(payload, role, instance, token)) return refuse_service("MLS1", "INVALID");
                    // Constant-time token check: the service identity is not a
                    // timing oracle for a peer that probes with a guessed token.
                    if (token.size() != config_.service_token.size()
                            || CRYPTO_memcmp(token.data(), config_.service_token.data(), token.size()) != 0)
                        return refuse_service("MLS1", "TOKEN");
                    service_ok_ = true;
                    return reply("MLS1 OK");
                }
                if (config_.service_token.empty() && payload.starts_with("MLS1"))
                    return refuse_service("MLS1", "UNEXPECTED"); // surfaces a stray token config
            }
            // A gateway owns the shared leg, so it must not answer MLC2 for a
            // logical session it only forwards.
            if (!upstream_ && !link_ && payload == "MLC2") {
                reply("MLC2 OK"); // queued unprefixed while correlation is still off
                correlated_ = true;
                if (registry_ && context_.session) registry_->set_correlated(context_.session);
                return;
            }
        }
        if ((config_.is_cross_gateway() || config_.role == "crossServer") && !joined_) {
            try {
                context_ = cross_context(payload);
                if (context_.epoch != config_.route_epoch) return reject("EPOCH");
                if (ticket_verifier_ && context_.ticket) {
                    // The ticket keys to the participant's home realm, the
                    // host's own instance name and the declared hosting
                    // generation; a failed verdict refuses the whole join —
                    // the context never drops a claimed ticket — with one
                    // word, and the specific reason goes to the log only.
                    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    const auto verdict = ticket_verifier_->verify(*context_.ticket,
                        {context_.return_realm, context_.character}, context_.generation, now);
                    if (verdict != TicketVerdict::Ok) {
                        std::cerr << "entry ticket refused: " << ticket_verdict_name(verdict) << '\n';
                        return reject("TICKET");
                    }
                    context_.verified = true;
                }
                auto index = config_.is_cross_gateway()
                    ? cross_shard(context_, config_.upstreams.size()) : 0;
                auto error = admission_.acquire(this, context_, index);
                if (!error.empty()) return reject(error);
                joined_ = true;
                awaiting_join_ = config_.is_cross_gateway();
                if (config_.role == "crossServer")
                    return reply("MLC1 OK " + std::to_string(config_.route_epoch));
                const auto& endpoint = config_.upstreams[index];
                endpoint_ = tcp::endpoint(parse_address(endpoint.address), endpoint.port);
                probe_port_ = endpoint.probe_port;
                upstream_ = std::make_shared<TcpChannel>(tcp::socket(client_->executor()));
            } catch (const std::exception&) { return reject("INVALID"); }
        }
        routed_ = true; // the handshake window ends at the first business payload
        if (link_) return forward_link(std::move(payload), 0);
        if (!upstream_) {
            // Correlated requests carry their id in the first payload bytes;
            // the reply echoes it, pushes answer with id 0.
            if (!correlated_) return serve_local_route(std::move(payload), 0);
            if (payload.size() < 4) return stop(); // malformed correlated frame
            const auto request = frame_id(payload);
            payload.erase(0, 4);
            return serve_local_route(std::move(payload), request);
        }
        if (awaiting_join_ || awaiting_mls1_) deadline(10); // a verdict is bounded, not the session
        if (upstream_->socket().is_open()) return forward_upstream(std::move(payload));
        if (probe_port_ && !probed_) return probe(std::move(payload));
        upstream_->socket().async_connect(endpoint_,
          [self=shared_from_this(), payload=std::move(payload)](Error error) mutable {
            if (self->closed_) return;
            if (error) { self->diagnostics_.upstream_error(); return self->stop(); }
            self->open_upstream(std::move(payload));
          });
    }
    // Entity traffic is a framework payload kind, not a script message: a
    // frame beginning with MLE1 answers through the endpoint (closed typed
    // errors included) and never reaches the script. The trusted connection is
    // the only caller identity the transport can assert, so the caller never
    // comes from the envelope.
    void serve_local_route(std::string payload, std::uint64_t request) {
        if (entities_ && payload.starts_with("MLE1")) {
            std::string result;
            auto answer = entities_->handle(payload, caller(), result);
            if (!answer) return serve_local(std::move(payload), request); // not entity traffic
            diagnostics_.entity_result(result);
            if (answer->empty()) return pump_client(); // a notify has no reply
            return reply(std::move(*answer), request);
        }
        return serve_local(std::move(payload), request);
    }
    EntityCaller caller() const {
        // An MLS1-identified peer (or a mux logical) is an internal service;
        // everything else is a client leg. Ownership attribution is a separate
        // concern from the transport binding, so the caller carries the
        // trusted session identity, never a self-reported owner field.
        return {(internal_ || service_ok_) ? "server" : "client", context_.session};
    }
    // Local script path: one Lua call per frame; a handler that returns nil
    // sends nothing and the session keeps reading.
    void serve_local(std::string payload, std::uint64_t request = 0) {
        try {
            context_.request = request;
            const auto started = std::chrono::steady_clock::now();
            auto response = service_->message(payload, &context_);
            diagnostics_.observe_lua_callback("message", std::chrono::steady_clock::now() - started);
            if (response) return reply(std::move(*response), request);
            pump_client();
        }
        catch (const std::exception& e) {
            std::cerr << "callback failed: " << e.what() << '\n';
            stop();
        }
    }
    void reject(const std::string& reason) {
        std::cerr << "cross admission rejected: " << reason << '\n';
        diagnostics_.reject(reason);
        client_->write("MLC1 ERR " + reason, [self=shared_from_this()](Error) { self->stop(); });
    }
    // Record legs are long-lived: one MLS1-identified peer submits and loads
    // many times over, so every verdict re-arms the client pump.
    void serve_record(std::string payload) {
        std::string_view result;
        auto verdict = records_->handle(payload, result);
        diagnostics_.record_result(result);
        reply(std::move(verdict));
    }
    // A refused service handshake closes the connection after the verdict:
    // INVALID means a malformed frame, TOKEN a missing or wrong secret,
    // UNEXPECTED a server that asks for no MLS1 at all.
    void refuse_service(const char* magic, const char* reason) {
        std::cerr << "service handshake rejected: " << reason << '\n';
        client_->write(std::string(magic) + " ERR " + reason,
            [self=shared_from_this()](Error) { self->stop(); });
    }
    // A new room only joins a shard the probe just saw ready. Probe failures are
    // conservative: an explicit notready rejects with NOT_READY, anything else
    // (refused, timeout, garbage) with UPSTREAM. Existing rooms never re-probe,
    // so a probe never evicts or remaps established state.
    void probe(std::string payload) {
        probed_ = true;
        auto self = shared_from_this();
        auto socket = std::make_shared<tcp::socket>(client_->executor());
        auto timer = std::make_shared<asio::steady_timer>(client_->executor());
        auto buffer = std::make_shared<std::string>();
        const std::string request = [] {
            if (const char* token = ::getenv("MISTLANE_PROBE_TOKEN"); token && *token)
                return std::string("PROBE1 join readiness ") + token + "\n";
            return std::string("PROBE1 join readiness\n");
        }();
        auto done = std::make_shared<bool>(false);
        auto fail = [self=shared_from_this(), timer, socket, done](const char* reason) {
            if (*done || self->closed_) return;
            *done = true;
            Error ignored;
            timer->cancel();
            socket->close(ignored);
            if (std::string_view(reason) == "NOT_READY") return self->reject("NOT_READY");
            self->diagnostics_.upstream_error();
            self->reject("UPSTREAM");
        };
        auto succeed = [self=shared_from_this(), timer, socket, done, payload=std::move(payload)]() mutable {
            if (*done || self->closed_) return;
            *done = true;
            Error ignored;
            timer->cancel();
            socket->close(ignored);
            self->upstream_->socket().async_connect(self->endpoint_,
              [self, payload=std::move(payload)](Error error) mutable {
                if (self->closed_) return;
                if (error) { self->diagnostics_.upstream_error(); return self->stop(); }
                self->open_upstream(std::move(payload));
              });
        };
        timer->expires_after(std::chrono::seconds(1));
        timer->async_wait([socket, done, fail](Error ec) {
            if (!ec && !*done) fail("TIMEOUT");
        });
        const tcp::endpoint probe_target{endpoint_.address(), probe_port_};
        socket->async_connect(probe_target, [self, socket, timer, request, buffer, done, fail, succeed](Error error) {
            if (error || *done) return fail("CONNECT");
            timer->expires_after(std::chrono::seconds(1));
            asio::async_write(*socket, asio::buffer(request),
              // asio::buffer points into `request`: keep the capture alive until
              // the write completes instead of letting the outer frame free it.
              [socket, timer, buffer, done, fail, succeed, request](Error write_error, std::size_t) {
                if (write_error || *done) return fail("WRITE");
                asio::async_read_until(*socket, asio::dynamic_buffer(*buffer), '\n',
                  [socket, timer, buffer, done, fail, succeed](Error read_error, std::size_t) mutable {
                    if (read_error || *done) return fail("READ");
                    const auto end = buffer->find('\n');
                    std::istringstream line(buffer->substr(0, end == buffer->npos ? buffer->size() : end));
                    std::string magic, nonce, verdict, state;
                    if (line >> magic >> nonce >> verdict >> state && magic == "PROBE1" &&
                        nonce == "join" && verdict == "ok") {
                        if (state == "ready") return succeed();
                        return fail("NOT_READY");
                    }
                    fail("GARBAGE");
                });
            });
        });
    }
    // Upstream leg: one frame in flight per direction, so a slow consumer
    // applies back pressure instead of growing the relay. Unsolicited frames
    // reach the client as they arrive; a handler that pushes never needs a
    // matching request. Re-armed by client-write completions.
    void pump_upstream() {
        if (closed_ || !upstream_ || !upstream_->socket().is_open() || upstream_reading_) return;
        upstream_reading_ = true;
        upstream_->read([self=shared_from_this()](Error ec, std::string frame) {
            self->upstream_reading_ = false;
            if (self->closed_) return;
            if (ec) { self->diagnostics_.upstream_error(); return self->stop(); }
            self->deadline(60);
            if (self->awaiting_mls1_) {
                self->awaiting_mls1_ = false;
                return self->handle_service_verdict(std::move(frame));
            }
            if (self->awaiting_join_) {
                self->awaiting_join_ = false;
                return self->handle_join(std::move(frame));
            }
            self->forward_to_client(std::move(frame));
        });
    }
    // The join verdict is the first upstream frame of a cross gateway session;
    // it relays to the client like any other frame and settles the session.
    void handle_join(std::string response) {
        if (response != "MLC1 OK " + std::to_string(config_.route_epoch)) {
            diagnostics_.upstream_error();
            if (!response.starts_with("MLC1 ERR ")) return reject("UPSTREAM");
            return client_->write(std::move(response), [self=shared_from_this()](Error) { self->stop(); });
        }
        deadline(60);
        diagnostics_.add_message("tx");
        diagnostics_.add_payload_bytes("tx", response.size());
        client_->write(std::move(response), [self=shared_from_this()](Error ec) {
            if (self->closed_) return;
            if (ec) return self->stop();
            self->pump_upstream();
            self->pump_client();
        });
    }
    // The upstream leg opens either into the MLS1 handshake (when the gateway
    // carries a token) or straight into relaying. Business frames queue behind
    // MLS1 in wire order; a refused verdict kills the session and everything
    // queued with it, and the reason stays on the operator's side.
    void open_upstream(std::string payload) {
        pump_upstream(); // the verdict may arrive before any reply
        if (!config_.upstream_token.empty() && !config_.is_cross_gateway() && !awaiting_mls1_) {
            awaiting_mls1_ = true;
            deadline(10); // the verdict is bounded, not the session
            forward_upstream(service_hello(config_));
        }
        forward_upstream(std::move(payload));
    }
    // Only the gateway leg sees the MLS1 verdict: success settles into the
    // normal idle cadence, anything else closes the session (the client saw
    // nothing service-level and cannot cause or fix the failure).
    void handle_service_verdict(std::string verdict) {
        if (verdict != "MLS1 OK") {
            diagnostics_.upstream_error();
            std::cerr << "service handshake failed: " << verdict << '\n';
            return stop();
        }
        deadline(60);
        pump_upstream();
        pump_client();
    }
    // An accepted peer asks to share this connection: hand the client channel
    // to the hub, which answers "MLM1 OK" and owns every logical session from
    // here. This session's slot is released so logicals count for themselves.
    void become_mux() {
        closed_ = true;
        timer_.cancel();
        if (gateway_) gateway_->drop(connection_); // the hub owns new ids, not these
        if (registry_ && context_.session) {
            registry_->detach(context_.session);
            context_.session = 0;
        }
        on_close_(this);
        mux_upgrade_(client_);
    }
    void forward_to_client(std::string frame) {
        // A reply carries the inner entity correlation the upstream saw; map it
        // back to the outer id this client used, or drop it when nobody is
        // waiting (stray, duplicate or a client that went away mid-flight).
        if (gateway_) {
            const auto offset = entity_frame_offset(frame);
            const auto at = offset == std::string_view::npos ? 0 : offset + kEntityReplyCorrelationAt;
            // A marker alone is not a reply: read the correlation only after
            // the frame is long enough to contain it. A zero/unknown id keeps
            // the prior behaviour (forward as-is / drop the stray reply).
            if (offset != std::string_view::npos && frame.size() >= at + 8) {
                const auto inner = entity_correlation(frame, at);
                if (inner) {
                    const auto mapped = gateway_->resolve(inner);
                    if (!mapped || mapped->first != connection_) return;
                    set_entity_correlation(frame, at, mapped->second);
                }
            }
        }
        diagnostics_.add_message("tx");
        diagnostics_.add_payload_bytes("tx", frame.size());
        client_->write(std::move(frame), [self=shared_from_this()](Error ec) {
            if (self->closed_) return;
            if (ec) return self->stop();
            self->pump_upstream();
            self->pump_client();
        });
    }
    // Shared upstream leg: the link adds this session's id and leaves any MLC2
    // id inside the payload, so correlation and multiplexing stay independent.
    void forward_link(std::string payload, std::uint64_t request) {
        if (request) payload = correlated_frame(request, payload);
        diagnostics_.add_message("tx");
        diagnostics_.add_payload_bytes("tx", payload.size());
        link_->send(mux_id_, std::move(payload), [self=shared_from_this()](Error ec) {
            if (self->closed_) return;
            if (ec) { self->diagnostics_.upstream_error(); return self->stop(); }
            self->pump_client();
        });
    }
    void forward_upstream(std::string payload) {
        // The gateway's entity path swaps the client's outer correlation for a
        // fresh inner id before forwarding: replies cannot collide across
        // client channels sharing one upstream, and a stalled upstream cannot
        // leak one client's correlation to another. A notify (correlation 0)
        // has no reply and forwards unchanged. Refusal answers CAPACITY to the
        // client without forwarding — the table never evicts a live request.
        if (gateway_) {
            const auto offset = entity_frame_offset(payload);
            const auto at = offset == std::string_view::npos ? 0 : offset + kEntityRequestCorrelationAt;
            // A marker alone is not an entity request: forward malformed or
            // zero-correlation frames unchanged only after the length check,
            // never reading the correlation slice out of bounds.
            if (offset != std::string_view::npos && payload.size() >= at + 8) {
                const auto outer = entity_correlation(payload, at);
                if (outer) {
                    const auto inner = gateway_->allocate(connection_, outer);
                    if (!inner) {
                        diagnostics_.entity_result("capacity");
                        std::string frame = "MLE1";
                        envelope_push_be(frame, outer, 8);
                        frame += '\x01';
                        frame += static_cast<char>(EntityError::CAPACITY);
                        return reply(std::move(frame));
                    }
                    set_entity_correlation(payload, at, inner);
                }
            }
        }
        upstream_->write(std::move(payload), [self=shared_from_this()](Error ec) {
            if (self->closed_) return;
            if (ec) { self->diagnostics_.upstream_error(); return self->stop(); }
            self->pump_upstream();
            self->pump_client();
        });
    }
    void reply(std::string payload, std::uint64_t request = 0) {
        if (request) payload = correlated_frame(request, payload); // echo the request id
        diagnostics_.add_message("tx");
        diagnostics_.add_payload_bytes("tx", payload.size());
        diagnostics_.observe_request(std::chrono::steady_clock::now() - request_started_);
        client_->write(std::move(payload), [self=shared_from_this()](Error ec) {
            if (self->closed_) return;
            if (ec) return self->stop();
            self->pump_client();
        });
    }
    std::shared_ptr<Channel> client_;
    std::shared_ptr<TcpChannel> upstream_;
    std::shared_ptr<MuxLink> link_;
    std::uint32_t mux_id_ = 0;
    std::shared_ptr<LuaService>& service_;
    asio::steady_timer timer_;
    tcp::endpoint endpoint_;
    const Config& config_;
    CrossAdmission& admission_;
    // Null unless this node verifies MLT1 tickets on joins; the claims live on
    // the node so a replay across connections on one process is still caught.
    const TicketVerifier* ticket_verifier_ = nullptr;
    Diagnostics& diagnostics_;
    GameSessions* registry_;
    RecordStore* records_ = nullptr;
    // Non-null only on a node that loaded an entity contract: MLE1 frames go to
    // the endpoint instead of the script, and non-entity frames pass through.
    // Non-const: handle() dispatches into the Lua state, which mutates it.
    EntityEndpoint* entities_ = nullptr;
    // Non-null only on a plain gateway session that owns a dedicated upstream
    // socket: the outer-to-inner correlation table. Sessions that ride a shared
    // link (upstream_multiplex) keep entity ids untouched and pass null here,
    // since a link reply is keyed by the logical session, not the table.
    GatewayEntityCorrelation* gateway_ = nullptr;
    std::uint64_t connection_ = 0;
    CrossContext context_;
    std::chrono::steady_clock::time_point request_started_{};
    std::uint16_t probe_port_ = 0;
    bool probed_ = false, joined_ = false, awaiting_join_ = false;
    bool client_reading_ = false, upstream_reading_ = false;
    // routed_ replaces "first frame": MLS1 and MLC2 stay valid until the first
    // business payload routes, so MLC2 can stack on an accepted MLS1 session.
    bool routed_ = false, correlated_ = false, service_ok_ = false, awaiting_mls1_ = false;
    std::function<void(Session*)> on_close_;
    std::function<void(std::shared_ptr<Channel>)> mux_upgrade_;
    TokenBucket bucket_;
    std::uint64_t generation_ = 0;
    bool closed_ = false, internal_ = false;
};
// Server side of MLM1: one inbound connection becomes many logical sessions,
// each with its own id and a Session on top of a VirtualChannel. Ids come from
// the peer, so capacity is checked before a logical session is born and an
// over-capacity request gets a control frame instead of a session.
class MuxHub : public std::enable_shared_from_this<MuxHub> {
public:
    struct Logical {
        std::shared_ptr<Channel> channel;
        std::function<void(std::string)> deliver;
    };
    using Spawner = std::function<std::shared_ptr<Logical>(std::uint32_t)>;
    MuxHub(std::shared_ptr<Channel> channel, Diagnostics& diagnostics,
           std::function<void(MuxHub*)> on_close)
      : channel_(std::move(channel)), diagnostics_(diagnostics),
        timer_(channel_->executor()), on_close_(std::move(on_close)) {}
    asio::any_io_executor executor() const { return channel_->executor(); }
    void start() {
        channel_->start([self=shared_from_this()](Error ec) {
            if (ec) { self->diagnostics_.upstream_error(); return self->teardown(); }
            self->write("MLM1 OK", [](Error) {}); // the verdict, then the first frame
            self->pump();
        });
    }
    // Node shutdown: retire the shared leg and every logical session on it.
    void stop() { teardown(); }
    // Who builds logical sessions; set once after construction because the
    // builder needs a weak reference back to this hub.
    void set_spawner(Spawner spawner) { spawn_ = std::move(spawner); }
    void send(std::uint32_t id, std::string payload, Completion completion) {
        if (dead_) return completion(asio::error::not_connected);
        write(correlated_frame(id, payload), std::move(completion));
    }
    // A logical session ended on this side; the control frame closes the loop
    // without tearing the shared connection down.
    void retire(std::uint32_t id) {
        if (dead_ || logicals_.erase(id) == 0) return;
        write(correlated_frame(0, "CLOSE " + std::to_string(id)), [](Error) {});
    }
private:
    void pump() {
        if (dead_ || reading_) return;
        reading_ = true;
        channel_->read([self=shared_from_this()](Error ec, std::string payload) {
            self->reading_ = false;
            if (self->dead_) return;
            if (ec) return self->teardown();
            self->deadline();
            self->dispatch(std::move(payload));
        });
    }
    void dispatch(std::string payload) {
        if (payload.size() < 4) return teardown(); // no id, no routing possible
        const auto id = frame_id(payload);
        payload.erase(0, 4);
        if (id == 0) return control(std::move(payload)); // re-arms or tears down
        const auto found = logicals_.find(id);
        if (found == logicals_.end()) {
            // The peer opened a new logical session; capacity is refused with a
            // control frame instead of a session.
            auto spawned = spawn_ ? spawn_(id) : nullptr;
            if (!spawned) return write(correlated_frame(0, "CLOSE " + std::to_string(id)), [](Error) {});
            auto logical = *spawned;
            logicals_[id] = logical;
            logical.deliver(std::move(payload));
        } else {
            found->second.deliver(std::move(payload));
        }
        pump(); // keep reading: deliver alone never touches the socket
    }
    // "CLOSE <id>" retires one logical session. It is erased before its channel
    // closes, so that close cannot bounce another control down the connection.
    void control(std::string payload) {
        constexpr std::string_view magic = "CLOSE ";
        char* end = nullptr;
        const auto target = payload.starts_with(magic)
            ? std::strtoul(payload.data()+magic.size(), &end, 10) : 0;
        if (!payload.starts_with(magic) || end == payload.data() || *end != '\0' || target == 0) {
            std::cerr << "unexpected frame\n";
            return teardown();
        }
        const auto found = logicals_.find(target);
        if (found == logicals_.end()) {
            std::cerr << "unexpected frame\n";
            return teardown();
        }
        auto channel = found->second.channel;
        logicals_.erase(found);
        channel->close();
        pump();
    }
    void write(std::string payload, Completion completion) {
        if (dead_) return completion(asio::error::not_connected);
        channel_->write(std::move(payload), [self=shared_from_this(), cb=std::move(completion)](Error ec) mutable {
            if (!ec) self->pump();
            cb(ec);
        });
    }
    void deadline() {
        const auto generation = ++generation_;
        timer_.expires_after(std::chrono::seconds(60));
        timer_.async_wait([self=shared_from_this(), generation](Error ec) {
            if (!ec && self->generation_ == generation) {
                self->diagnostics_.upstream_timeout();
                self->teardown();
            }
        });
    }
    void teardown() {
        if (dead_) return;
        dead_ = true;
        timer_.cancel();
        channel_->close();
        auto logicals = std::move(logicals_);
        logicals_.clear();
        for (auto& [id, logical] : logicals) { (void)id; logical.channel->close(); }
        if (on_close_) on_close_(this);
    }
    Spawner spawn_;
    std::shared_ptr<Channel> channel_;
    Diagnostics& diagnostics_;
    asio::steady_timer timer_;
    std::unordered_map<std::uint32_t, Logical> logicals_;
    std::uint64_t generation_ = 0;
    bool dead_ = false, reading_ = false;
    std::function<void(MuxHub*)> on_close_;
};
// One logical session of a MuxHub: the Channel a Session talks to, carrying an
// id and borrowing the hub's connection and bounds. Ingress stays bounded like
// a socket buffer so a stalled logical session stops the link, not memory.
class VirtualChannel : public Channel, public std::enable_shared_from_this<VirtualChannel> {
public:
    VirtualChannel(std::uint32_t id, std::weak_ptr<MuxHub> hub, asio::any_io_executor executor)
      : hub_(std::move(hub)), executor_(executor), id_(id) {}
    asio::any_io_executor executor() override { return executor_; }
    void start(Completion callback) override { callback({}); }
    void read(Reader callback) override {
        auto self = shared_from_this(); // the posted handoff must keep the channel alive
        if (closed_) return callback(asio::error::not_connected, {});
        if (!ingress_.empty()) {
            auto payload = std::move(ingress_.front());
            ingress_.pop_front();
            asio::post(executor_, [self, cb=std::move(callback), payload=std::move(payload)]() mutable {
                cb({}, std::move(payload));
            });
            return;
        }
        reader_ = std::move(callback);
    }
    void write(std::string payload, Completion callback) override {
        if (closed_) return callback(asio::error::not_connected);
        if (payload.size()+4 > max_frame) return callback(asio::error::message_size);
        const auto hub = hub_.lock();
        if (!hub) return callback(asio::error::not_connected);
        hub->send(id_, std::move(payload), [self=shared_from_this(), cb=std::move(callback)](Error ec) mutable {
            if (ec) self->close(); // a refused or dead leg ends the logical session
            cb(ec);                // the caller re-arms its own reader with this
        });
    }
    void close() override {
        if (closed_) return;
        closed_ = true;
        ingress_.clear();
        auto hub = hub_.lock();
        if (hub) hub->retire(id_);
        auto reader = std::move(reader_);
        reader_ = {};
        if (reader) reader(asio::error::not_connected, {});
    }
    // Delivered by the hub off the shared connection.
    void deliver(std::string payload) {
        if (closed_) return;
        auto reader = std::move(reader_);
        if (reader) {
            reader_ = {};
            asio::post(executor_, [cb=std::move(reader), payload=std::move(payload)]() mutable {
                cb({}, std::move(payload));
            });
            return;
        }
        if (ingress_.size()+payload.size() > write_backlog) return close();
        ingress_.push_back(std::move(payload));
    }
private:
    std::weak_ptr<MuxHub> hub_;
    asio::any_io_executor executor_;
    std::uint32_t id_;
    std::deque<std::string> ingress_;
    Reader reader_;
    bool closed_ = false;
};
// Bridges the script's mistlane.metrics.gauge onto the node's diagnostics
// registry; lives only as long as the LuaService that borrows it.
class ScriptGaugeSink final : public ScriptMetricsSink {
public:
    explicit ScriptGaugeSink(Diagnostics& diagnostics) : diagnostics_(diagnostics) {}
    void script_gauge(std::string_view name, double value) override {
        diagnostics_.set_script_gauge(name, value);
    }
private:
    Diagnostics& diagnostics_;
};
// Lua websocket lifecycle sessions (script roles with an http/https listener).
// Every method runs on the node's event loop thread, matching the WebsocketSink
// contract, so no locking is needed. Sessions share max_connections with the
// framed game sessions.
class WebsocketService : public WebsocketSink, public std::enable_shared_from_this<WebsocketService> {
public:
    using Admission = std::function<bool()>;
    using Publish = std::function<void()>;
    WebsocketService(std::shared_ptr<LuaService>& service, Admission admit, Publish publish)
      : service_(service), admit_(std::move(admit)), publish_(std::move(publish)) {}
    std::size_t size() const { return sessions_.size(); }
    void attach(std::shared_ptr<Channel> channel, std::string path) {
        if (!admit_()) return channel->close();
        const auto id = next_id_++;
        sessions_.emplace(id, channel);
        publish_();
        try {
            service_->websocket_open(id, path);
        } catch (const std::exception& e) {
            // A failing open never reaches the message/close callbacks; drop it.
            std::cerr << "websocket open failed: " << e.what() << '\n';
            sessions_.erase(id);
            publish_();
            channel->close();
            return;
        }
        pump(id);
    }
    void close_all() {
        // WebChannel::close() completes the pending read inline, which erases
        // from sessions_ while Lua's websocket_close callback runs.
        while (!sessions_.empty()) sessions_.begin()->second->close();
    }
private:
    std::string websocket_send(std::uint64_t id, std::string_view payload) override {
        if (payload.size() > max_frame) return "payload exceeds 1 MiB";
        const auto found = sessions_.find(id);
        if (found == sessions_.end()) return "unknown websocket id";
        auto channel = found->second;
        channel->write(std::string(payload), [self=shared_from_this(), id](Error ec) {
            if (ec) self->closed(id);
        });
        return "";
    }
    void pump(std::uint64_t id) {
        const auto found = sessions_.find(id);
        if (found == sessions_.end()) return;
        auto channel = found->second;
        channel->read([self=shared_from_this(), id](Error ec, std::string payload) {
            if (ec) return self->closed(id);
            try {
                self->service_->websocket_message(id, payload);
            } catch (const std::exception& e) {
                std::cerr << "websocket message failed: " << e.what() << '\n';
                // Lua failures close the session; the close callback is not re-fired.
                self->drop(id);
                return;
            }
            self->pump(id);
        });
    }
    void closed(std::uint64_t id) {
        if (sessions_.erase(id) == 0) return;
        publish_();
        try {
            service_->websocket_close(id);
        } catch (const std::exception& e) { std::cerr << "websocket close failed: " << e.what() << '\n'; }
    }
    void drop(std::uint64_t id) {
        const auto found = sessions_.find(id);
        if (found == sessions_.end()) return;
        auto channel = found->second;
        sessions_.erase(found);
        publish_();
        channel->close(); // the aborted read lands in closed(), a no-op on the erased id
    }
    std::shared_ptr<LuaService>& service_;
    Admission admit_;
    Publish publish_;
    std::map<std::uint64_t, std::shared_ptr<Channel>> sessions_;
    std::uint64_t next_id_ = 1;
};
class Node : public PlacementSink {
public:
    // One placement question, one connection: login handlers ask the registry
    // through Lua; the answer callback runs back on this event loop.
    void place(std::string_view role, std::string_view namespace_, std::string_view scope,
        std::function<void(std::string_view address, std::string_view reason)> answer) override {
        if (!config_.coordinator_enabled) return answer({}, "disabled");
        RegistryClient::lookup(io_, config_, role, namespace_, scope, std::move(answer));
    }
    explicit Node(Config config)
      : config_(std::move(config)), signals_(io_, SIGINT, SIGTERM, SIGHUP), tick_(io_),
        admission_(std::max<std::size_t>(1, config_.upstreams.size()), config_.max_room_sessions,
                   config_.max_group_sessions, config_.max_shard_sessions) {
        diagnostics_ = std::make_unique<Diagnostics>(config_);
        metrics_sink_ = std::make_unique<ScriptGaugeSink>(*diagnostics_);
        if (config_.role == "data" && !config_.journal.empty()) {
            record_journal_ = std::make_unique<MigrationJournal>(config_.journal);
            records_.attach_backend(record_journal_.get());
        }
        if (config_.probe_enabled) {
            probe_ = std::make_shared<ProbeServer>(io_, config_.probe_listen,
                [this](std::string_view command) {
                    return command == "liveness" ? std::string("ok alive") : readiness_summary();
                });
            probe_->start();
        }
        // Read-only observability surface: the embedded dashboard page plus
        // /metrics from the same registry the exposer serves.
        if (config_.dashboard_enabled) {
            dashboard_ = std::make_shared<DashboardServer>(io_, config_.dashboard_listen,
                [this] { return diagnostics_->metrics_text(); });
            dashboard_->start();
        }
        // Reserve an admission envelope before allocating protocol buffers. Not a hard RSS quota.
        constexpr std::size_t session_envelope = 6*max_frame + 64*1024;
        config_.max_connections = std::min(config_.max_connections,
            config_.memory_budget_mib*1024*1024/session_envelope);
        if (!config_.max_connections) throw std::invalid_argument("memory budget too small for one session");
        std::cout << "session admission limit " << config_.max_connections << std::endl;
        // What the listener mix demands from the script and from the process:
        // framed game sessions (tcp/ws/wss), the Lua HTTP service (http/https),
        // and whether Drogon owns any listener at all.
        bool game_sessions = false, http_service = false;
        for (const auto& item : config_.listeners) {
            if (item.protocol == "http" || item.protocol == "https") http_service = true;
            else game_sessions = true;
            web_mode_ = web_mode_ || item.protocol != "tcp";
        }
        for (const auto& item : config_.listeners) {
            if (item.protocol != "wss" && item.protocol != "https") continue;
            // Validate every TLS listener's effective pair (listener override,
            // else the global --cert/--key) before any port opens.
            const auto& certificate = item.certificate.empty() ? config_.certificate : item.certificate;
            const auto& private_key = item.private_key.empty() ? config_.private_key : item.private_key;
            asio::ssl::context check(asio::ssl::context::tls_server);
            if (!SSL_CTX_set_min_proto_version(check.native_handle(), TLS1_2_VERSION))
                throw std::runtime_error("cannot configure minimum TLS version");
            check.use_certificate_chain_file(certificate);
            check.use_private_key_file(private_key, asio::ssl::context::pem);
            if (!SSL_CTX_check_private_key(check.native_handle()))
                throw std::runtime_error("TLS private key does not match certificate");
        }
        if (config_.redis_enabled) {
            redis_ = std::make_shared<RedisRuntime>(io_, config_.redis.address, config_.redis.port);
            redis_->start();
        }
        // One source of truth for script-backed roles, so a new script role
        // needs no node-side change.
        const auto* role_definition = find_role(config_.role);
        const bool script_role = role_definition && role_definition->script;
        service_context_ = ServiceContext{config_.role, config_.transport, config_.route_epoch, game_sessions, http_service};
        if (config_.role == "coordinator") registry_ = std::make_unique<Registry>(config_.journal);
        if (config_.coordinator_enabled && config_.role != "login")
            // Members register and heartbeat; login only asks placements.
            member_ = std::make_shared<RegistryMember>(io_, config_,
                [this] { return heartbeat_status(); });
        if (script_role && http_service)
            // Holds a reference to service_, so script reloads swap under it freely.
            websockets_ = std::make_shared<WebsocketService>(service_,
                [this] { return !stopping_ && active_sessions() < config_.max_connections; },
                [this] { publish_active(); });
        if (script_role && game_sessions)
            game_sessions_ = std::make_shared<GameSessions>();
        if (!config_.contract.empty()) {
            // The contract is a build-time descriptor; the node reparses the
            // document exactly like `contract check` so a malformed definition
            // refuses startup rather than booting a half-routed host. Type
            // sources stay inside the document's directory.
            const fs::path path(config_.contract);
            std::ifstream in(path, std::ios::binary);
            if (!in) throw std::runtime_error("cannot read contract: "+path.string());
            std::ostringstream text; text << in.rdbuf();
            const fs::path dir = path.parent_path();
            const auto type_resolver = [&](const std::string& rel) -> std::string {
                if (rel.empty() || rel.find("..") != std::string::npos)
                    throw std::runtime_error("type source path escapes the contract directory: "+rel);
                std::ifstream src(dir/rel, std::ios::binary);
                if (!src) throw std::runtime_error("type source not found: "+rel);
                std::ostringstream buf; buf << src.rdbuf();
                return buf.str();
            };
            entity_contract_ = std::make_unique<EntityContract>(text.str(), type_resolver);
            entity_codec_ = std::make_unique<SprotoCodec>(*entity_contract_);
            entity_endpoint_ = std::make_unique<EntityEndpoint>(*entity_contract_, entity_codec_.get());
            std::cout << "contract " << path.string() << " loaded" << std::endl;
        }
        // The plain gateway is the entity relay: it maps outer transport
        // correlations onto the inner entity ids for every session it fronts.
        // The cross gateway speaks its own join protocol and is out of scope.
        if (config_.role == "gateway" && !config_.is_cross_gateway())
            gateway_correlation_ = std::make_unique<GatewayEntityCorrelation>();
        if (const char* secrets = std::getenv("MISTLANE_TICKET_SECRETS")) {
            // A host never loads a character on a bare claim: with secrets
            // configured the crossServer verifies the join's ticket before
            // admitting it, while the login role signs the ticket for that
            // same home realm. Anything else would silently run a node that
            // looks verified but answers no.
            if (config_.role != "crossServer" && config_.role != "login")
                throw std::invalid_argument(
                    "MISTLANE_TICKET_SECRETS only applies to the crossServer and login roles");
            if (!*secrets) throw std::invalid_argument("MISTLANE_TICKET_SECRETS is empty");
            if (config_.role == "crossServer") {
                if (!record_identifier(config_.service_name))
                    throw std::invalid_argument("MISTLANE_TICKET_SECRETS requires --service-name");
                ticket_verifier_ = std::make_unique<TicketVerifier>(
                    ticket_secrets(secrets), config_.service_name, ticket_claims_);
            } else {
                ticket_issuer_ = std::make_unique<TicketIssuer>(ticket_secrets(secrets));
            }
        }
        if (config_.role == "crossServer" && !config_.data.address.empty())
            return_sender_ = std::make_shared<ReturnSender>(io_, config_.data, *diagnostics_,
                config_.upstream_token.empty() ? "" : service_hello(config_));
        if (script_role)
            service_ = std::make_shared<LuaService>(config_.script, service_context_, redis_,
                websockets_.get(), game_sessions_.get(), this,
                entity_endpoint_ ? &entity_endpoint_->runtime() : nullptr,
                ticket_issuer_.get(), return_sender_.get(), metrics_sink_.get());
        if (http_service) {
            // MLA1 rides the same listener as the Lua routes. Both variables
            // must be injected together; anything else leaves /mla1 as a
            // plain Lua route and half-configured deployments fail startup.
            const char* audience = std::getenv("MISTLANE_MLA1_AUDIENCE");
            const char* credential = std::getenv("MISTLANE_MLA1_CREDENTIAL");
            const bool enabled = audience && *audience && credential && *credential;
            if (!enabled && ((audience && *audience) || (credential && *credential)))
                throw std::invalid_argument(
                    "MISTLANE_MLA1_AUDIENCE and MISTLANE_MLA1_CREDENTIAL must be set together");
            if (enabled) {
                if (!config_.journal.empty())
                    api_store_ = std::make_unique<ApiSqliteAcceptanceStore>(config_.journal);
                else
                    api_store_ = std::make_unique<ApiMemoryAcceptanceStore>();
                api_verifier_ = std::make_unique<ApiVerifier>(audience, api_credentials(credential), api_replay_);
                // Ownership needs the home transaction aggregate; until that
                // source is wired the endpoint answers NotFound, never invents.
                api_surface_ = std::make_unique<ApiSurface>(*api_verifier_, *api_store_,
                    [](const MigrationKey&) { return std::nullopt; });
            }
        }
        for (const auto& e : config_.upstreams) (void)parse_address(e.address);
        for (const auto& item : config_.listeners) {
            if (item.protocol != "tcp") continue;
            auto acceptor = std::make_shared<tcp::acceptor>(io_);
            auto endpoint = tcp::endpoint(parse_address(item.endpoint.address), item.endpoint.port);
            acceptor->open(endpoint.protocol());
            acceptor->set_option(asio::socket_base::reuse_address(true));
            acceptor->bind(endpoint);
            acceptor->listen();
            acceptors_.push_back(std::move(acceptor));
        }
    }
    void run() {
        signal();
        tick();
        const auto announce = [this](const Listener& item) {
            std::cout << "ready " << config_.role << ' ' << item.endpoint.address << ':'
                      << item.endpoint.port << std::endl;
        };
        if (!web_mode_) {
            if (service_) service_->start();
            bind_entities();
            accept();
            diagnostics_->set_ready(true);
            ready_ = true;
            if (member_) member_->start();
            if (return_sender_) return_sender_->start();
            for (const auto& item : config_.listeners) announce(item);
            io_.run();
        } else {
            std::thread worker([this] { io_.run(); });
            try {
                // Mixed listeners: framed TCP beside the Drogon-owned ones.
                accept();
                for (const auto& item : config_.listeners)
                    if (item.protocol == "tcp") announce(item);
                WsAttach lifecycle;
                if (websockets_) lifecycle = [this](std::shared_ptr<Channel> channel, std::string path) {
                    websockets_->attach(std::move(channel), std::move(path));
                };
                run_web(config_, io_.get_executor(), [this](std::shared_ptr<Channel> c) { attach(std::move(c)); },
                    std::move(lifecycle),
                    [this](WebRequest request, HttpCompletion complete) {
                        asio::post(io_, [this, request=std::move(request), complete=std::move(complete)]() mutable {
                            if (stopping_) return complete({503, {}, "Service Unavailable"});
                            // Native MLA1 route, no script in the path. Bodies are
                            // the canonical bytes plus the raw signature; replies
                            // are five framed words, self-describing without a
                            // content type.
                            if (api_surface_ && request.method == "POST" && request.path == "/mla1") {
                                ApiReply reply;
                                ApiRequest parsed;
                                if (!parse_api_body(request.body, parsed)) reply.auth = ApiAuth::Invalid;
                                else {
                                    const auto now = static_cast<std::uint64_t>(
                                        std::chrono::duration_cast<std::chrono::seconds>(
                                            std::chrono::system_clock::now().time_since_epoch()).count());
                                    reply = api_surface_->handle(parsed, now);
                                }
                                return complete({api_http_status(reply), {}, api_reply_body(reply)});
                            }
                            if (!service_) return complete({503, {}, "Service Unavailable"});
                            const auto started = std::chrono::steady_clock::now();
                            auto response = service_->http(request);
                            diagnostics_->observe_lua_callback("http", std::chrono::steady_clock::now() - started);
                            complete(std::move(response));
                        });
                    }, [this, &announce] {
                        try {
                            if (service_) service_->start();
                            bind_entities();
                            // Web mode still runs the registry member and the
                            // cross-return sender: a WS/HTTP/HTTPS crossServer is
                            // not exempt from readiness or home-data draining.
                            if (member_) member_->start();
                            if (return_sender_) return_sender_->start();
                            diagnostics_->set_ready(true);
                            ready_ = true;
                            for (const auto& item : config_.listeners)
                                if (item.protocol != "tcp") announce(item);
                        } catch (...) { startup_error_ = std::current_exception(); stop(); }
                    });
            }
            catch (...) { asio::post(io_, [this] { stop(); }); worker.join(); throw; }
            asio::post(io_, [this] { stop(); });
            worker.join();
            if (startup_error_) std::rethrow_exception(startup_error_);
        }
    }
private:
    // Game sessions, registry control connections and Lua websocket sessions
    // share one admission budget.
    std::size_t active_sessions() const {
        return sessions_.size() + registry_sessions_.size() + (websockets_ ? websockets_->size() : 0);
    }
    void publish_active() { diagnostics_->set_active_sessions(active_sessions()); }
    std::string heartbeat_status() const {
        return std::string(ready_ && admitting_ ? "ready" : "notready")+" "+
            std::to_string(active_sessions())+" "+std::to_string(config_.max_connections);
    }
    void attach(std::shared_ptr<Channel> client) {
        if (stopping_ || active_sessions() >= config_.max_connections) return client->close();
        if (registry_) return attach_registry(std::move(client));
        const Endpoint* upstream = config_.upstreams.empty() || config_.is_cross_gateway() ? nullptr
            : &config_.upstreams[cursor_++ % config_.upstreams.size()];
        // An authenticated peer may upgrade its leg into a shared hub.
        std::function<void(std::shared_ptr<Channel>)> mux_upgrade;
        if (!config_.is_cross_gateway() && config_.role != "crossServer")
            mux_upgrade = [this](std::shared_ptr<Channel> channel) { attach_mux(std::move(channel)); };
        auto session = std::make_shared<Session>(std::move(client), service_, upstream, config_, admission_, *diagnostics_,
            game_sessions_.get(),
            [this](Session* ptr) {
                admission_.release(ptr);
                for (auto i = sessions_.begin(); i != sessions_.end(); ++i)
                    if (i->get() == ptr) { sessions_.erase(i); break; }
                publish_active();
            },
            config_.upstream_multiplex ? &mux_links_ : nullptr, std::move(mux_upgrade),
            false, config_.role == "data" ? &records_ : nullptr, ticket_verifier_.get(),
            entity_endpoint_.get(),
            // The correlation table belongs to the dedicated-socket upstream
            // path only; link sessions are keyed by their logical id instead.
            config_.upstream_multiplex ? nullptr : gateway_correlation_.get(), next_connection_++);
        sessions_.insert(session);
        publish_active();
        session->start();
    }
    // The registry accepts control connections only; gameplay roles never
    // reach the coordinator listener's forwarding machinery.
    void attach_registry(std::shared_ptr<Channel> client) {
        if (stopping_) { client->close(); return; }
        auto session = std::make_shared<RegistrySession>(std::move(client), *registry_, config_,
            *diagnostics_, [this](RegistrySession* ptr) {
                for (auto i = registry_sessions_.begin(); i != registry_sessions_.end(); ++i)
                    if (i->get() == ptr) { registry_sessions_.erase(i); break; }
                diagnostics_->set_registry_members(registry_->size());
                publish_active();
            });
        registry_sessions_.insert(session);
        publish_active();
        session->start();
    }
    // A peer upgraded a connection into a hub: it now owns every logical
    // session until the peer goes away.
    void attach_mux(std::shared_ptr<Channel> channel) {
        if (stopping_) { channel->close(); return; }
        auto hub = std::make_shared<MuxHub>(std::move(channel), *diagnostics_,
            [this](MuxHub* ptr) {
                for (auto i = hubs_.begin(); i != hubs_.end(); ++i)
                    if (i->get() == ptr) { hubs_.erase(i); break; }
            });
        hubs_.insert(hub);
        const auto weak = std::weak_ptr<MuxHub>(hub);
        hub->set_spawner([this, weak](std::uint32_t id) -> std::shared_ptr<MuxHub::Logical> {
            const auto hub_ptr = weak.lock();
            if (!hub_ptr || stopping_ || active_sessions() >= config_.max_connections) return nullptr;
            auto channel = std::make_shared<VirtualChannel>(id, weak, hub_ptr->executor());
            auto logical = std::make_shared<MuxHub::Logical>();
            logical->channel = channel;
            logical->deliver = [channel](std::string payload) { channel->deliver(std::move(payload)); };
            auto session = std::make_shared<Session>(channel, service_, nullptr, config_, admission_, *diagnostics_,
                game_sessions_.get(),
                [this](Session* ptr) {
                    admission_.release(ptr);
                    for (auto i = sessions_.begin(); i != sessions_.end(); ++i)
                        if (i->get() == ptr) { sessions_.erase(i); break; }
                    publish_active();
                }, nullptr, nullptr, true, config_.role == "data" ? &records_ : nullptr,
                ticket_verifier_.get(), entity_endpoint_.get(), nullptr, 0);
            sessions_.insert(session);
            publish_active();
            session->start();
            return logical;
        });
        hub->start();
    }
    void accept() {
        for (const auto& acceptor : acceptors_) accept(acceptor);
    }
    void accept(const std::shared_ptr<tcp::acceptor>& acceptor) {
        acceptor->async_accept([this, acceptor](Error ec, tcp::socket socket) {
            if (stopping_) return;
            if (!ec && active_sessions() < config_.max_connections) {
                attach(std::make_shared<TcpChannel>(std::move(socket)));
            } else if (!ec) { diagnostics_->reject("connection_limit"); Error ignored; socket.close(ignored); }
            // Resource exhaustion should fail visibly rather than spin the event loop.
            if (ec && ec != asio::error::operation_aborted) {
                std::cerr << "accept failed: " << ec.message() << '\n';
                stop();
                return;
            }
            if (!ec) accept(acceptor);
        });
    }
    void signal() {
        signals_.async_wait([this](Error ec, int value) {
            if (ec || stopping_) return;
            if (value == SIGHUP) {
                if (service_) {
                    try {
                        auto replacement = std::make_shared<LuaService>(config_.script, service_context_, redis_,
                            websockets_.get(), game_sessions_.get(), this,
                            entity_endpoint_ ? &entity_endpoint_->runtime() : nullptr);
                        replacement->start();
                        // Rebinding under the new state happens before the swap,
                        // so a bad implementation table keeps the old service.
                        std::string error;
                        if (!bind_entities(*replacement, error))
                            throw std::runtime_error("entity bindings: "+error);
                        service_.swap(replacement);
                        std::cout << "script reloaded; module state reset" << std::endl;
                    } catch (const std::exception& e) {
                        std::cerr << "reload rejected, previous service retained: " << e.what() << '\n';
                    }
                }
                signal();
            } else stop();
        });
    }
    // Binds the script's entity implementation table onto the runtime's
    // dispatcher. A node without a contract has nothing to bind. The throwing
    // form serves startup (a refused table must abort the boot); the error form
    // serves SIGHUP reload (a refused table keeps the previous service).
    void bind_entities() {
        if (!entity_endpoint_) return;
        std::string error;
        if (!bind_entities(*service_, error)) throw std::runtime_error("entity bindings: "+error);
    }
    bool bind_entities(LuaService& service, std::string& error) {
        if (!entity_endpoint_) return true;
        return service.bind_entity_handlers(entity_endpoint_->runtime().dispatcher(), *entity_contract_, error);
    }
    void tick() {
        const auto scheduled = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        tick_.expires_at(scheduled);
        tick_.async_wait([this, scheduled](Error ec) {
            if (ec || stopping_) return;
            // Timestamps only: observing the lag must never block the Lua loop.
            const auto lag = std::chrono::steady_clock::now() - scheduled;
            diagnostics_->observe_event_loop_lag(lag);
            last_lag_seconds_ = std::chrono::duration<double>(lag).count();
            // Readiness flips only after a full hysteresis window on either side,
            // so a single busy tick never starts rejecting new rooms.
            const bool saturated =
                static_cast<double>(active_sessions()) >= config_.overload_ratio*static_cast<double>(config_.max_connections);
            over_ticks_ = saturated ? over_ticks_+1 : 0;
            healthy_ticks_ = saturated ? 0 : healthy_ticks_+1;
            if (over_ticks_ >= config_.ready_hysteresis) admitting_ = false;
            if (healthy_ticks_ >= config_.ready_hysteresis) admitting_ = true;
            // The exported readiness must track the gate the probe answers with,
            // so operators and load balancers see the same state.
            diagnostics_->set_ready(ready_ && admitting_);
            if (service_) {
                try { service_->tick(); }
                catch (const std::exception& e) { std::cerr << "tick failed: " << e.what() << '\n'; }
            }
            diagnostics_->refresh_process_memory();
            tick();
        });
    }
    std::string readiness_summary() const {
        std::ostringstream out;
        out << "ok " << (ready_ && admitting_ ? "ready" : "notready")
            << " active=" << active_sessions() << " capacity=" << config_.max_connections
            << " lag_ms=" << std::fixed << std::setprecision(1) << last_lag_seconds_*1000.0;
        return out.str();
    }
    void stop() {
        if (stopping_) return;
        stopping_ = true;
        ready_ = false;
        diagnostics_->set_ready(false);
        if (probe_) probe_->stop();
        if (dashboard_) dashboard_->stop();
        Error ignored;
        for (const auto& acceptor : acceptors_) acceptor->close(ignored);
        // Close websocket sessions while the VM can still run their callbacks.
        if (websockets_) websockets_->close_all();
        if (service_) {
            try { service_->stop(); }
            catch (const std::exception& e) { std::cerr << "stop failed: " << e.what() << '\n'; }
        }
        if (redis_) redis_->stop();
        if (web_mode_) stop_web();
        signals_.cancel();
        tick_.cancel();
        // Hubs first: their teardown stops the logical sessions through the
        // normal Session::stop path, so the drain below only sees leftovers.
        auto hubs = std::move(hubs_);
        for (auto& hub : hubs) hub->stop();
        // Registry sessions retire their member entries as they close.
        auto registry_sessions = std::move(registry_sessions_);
        for (auto& session : registry_sessions) session->stop();
        if (member_) member_->stop();
        if (return_sender_) return_sender_->stop();
        while (!sessions_.empty()) {
            auto session = *sessions_.begin();
            session->stop();
        }
        if (service_) {
            try { service_->shutdown(); }
            catch (const std::exception& e) { std::cerr << "shutdown failed: " << e.what() << '\n'; }
        }
        // Drain cancellation handlers before destroying Lua/Node state.
    }
    Config config_;
    asio::io_context io_;
    MuxLinks mux_links_{io_};
    std::vector<std::shared_ptr<tcp::acceptor>> acceptors_;
    asio::signal_set signals_;
    asio::steady_timer tick_;
    std::shared_ptr<LuaService> service_;
    std::shared_ptr<WebsocketService> websockets_;
    std::shared_ptr<GameSessions> game_sessions_;
    ServiceContext service_context_;
    std::shared_ptr<RedisRuntime> redis_;
    std::set<std::shared_ptr<Session>> sessions_;
    std::set<std::shared_ptr<MuxHub>> hubs_;
    std::set<std::shared_ptr<RegistrySession>> registry_sessions_;
    std::unique_ptr<Registry> registry_;
    std::shared_ptr<RegistryMember> member_;
    std::shared_ptr<ReturnSender> return_sender_;
    std::size_t cursor_ = 0;
    CrossAdmission admission_;
    // Only the data role routes frames into this; every other role passes
    // nullptr and keeps MLD1 as plain script traffic.
    RecordStore records_;
    std::unique_ptr<MigrationJournal> record_journal_;
    // MLA1 external API: native route beside the Lua HTTP callbacks.
    ApiReplayGuard api_replay_;
    std::unique_ptr<ApiAcceptanceStore> api_store_;
    std::unique_ptr<ApiVerifier> api_verifier_;
    std::unique_ptr<ApiSurface> api_surface_;
    // Entity runtime (only when --contract is configured). Declaration order
    // is lifetime order: the contract outlives the codec that borrows it and
    // the endpoint that owns the runtime/dispatcher.
    std::unique_ptr<EntityContract> entity_contract_;
    std::unique_ptr<SprotoCodec> entity_codec_;
    std::unique_ptr<EntityEndpoint> entity_endpoint_;
    // Outer-to-inner entity correlation table, present only on a plain gateway.
    // Sessions borrow it; the node hands out per-session connection ids.
    std::unique_ptr<GatewayEntityCorrelation> gateway_correlation_;
    std::uint64_t next_connection_ = 1;
    // MLT1 host verification on the crossServer join path. The claims are
    // volatile and per process, like the rest of the verifier's state.
    TicketClaims ticket_claims_;
    std::unique_ptr<TicketVerifier> ticket_verifier_;
    std::unique_ptr<TicketIssuer> ticket_issuer_;
    std::unique_ptr<Diagnostics> diagnostics_;
    std::shared_ptr<ProbeServer> probe_;
    std::shared_ptr<DashboardServer> dashboard_;
    std::unique_ptr<ScriptGaugeSink> metrics_sink_;
    std::exception_ptr startup_error_;
    bool ready_ = false, admitting_ = true;
    unsigned over_ticks_ = 0, healthy_ticks_ = 0;
    double last_lag_seconds_ = 0;
    bool stopping_ = false, web_mode_ = false;
};
}
int run_node(Config config) { Node node(std::move(config)); node.run(); return 0; }
}
