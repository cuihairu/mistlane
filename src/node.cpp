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
#include "session_plane.hpp"
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
                            entity_endpoint_ ? &entity_endpoint_->runtime() : nullptr,
                            ticket_issuer_.get(), return_sender_.get(), metrics_sink_.get());
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
