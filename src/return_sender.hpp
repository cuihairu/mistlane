// SPDX-License-Identifier: Apache-2.0
// The sender side of the MLD1 cross return protocol, extracted from the node
// implementation: one ordered retry queue per crossServer, one LOAD/SUBMIT leg
// against the home data endpoint, MLS1 identity when configured.
#pragma once
#include "tcp_channel.hpp"
#include "mistlane/lua_service.hpp"
#include "mistlane/diagnostics.hpp"
#include "mistlane/record.hpp"
#include <chrono>
#include <optional>
#include <set>
#include <sstream>
#include <map>
#include <vector>

namespace mistlane {

// The identity a gateway presents to an upstream: role plus --service-name, else
// the listen endpoint. It only labels the peer's logs; the token decides.
inline std::string service_hello(const Config& config) {
    const std::string instance = !config.service_name.empty() ? config.service_name
        : config.listen.address + ":" + std::to_string(config.listen.port);
    return "MLS1 " + config.role + " " + instance + " " + config.upstream_token;
}

class ReturnSender : public ReturnSink, public std::enable_shared_from_this<ReturnSender> {
public:
    ReturnSender(asio::io_context& io, const Endpoint& endpoint, Diagnostics& diagnostics,
                 std::string identity)
      : endpoint_(tcp::endpoint(parse_address(endpoint.address), endpoint.port)), io_(io),
        diagnostics_(diagnostics), timer_(io), queue_(3, pending_capacity_),
        identity_(std::move(identity)) {}
    void start() { if (!started_) { started_ = true; connect(); } }
    void stop() {
        stopping_ = true;
        timer_.cancel();
        if (channel_) { channel_->close(); channel_ = nullptr; }
    }
    // Whether every change this participant queued has been durably
    // acknowledged: nothing pending in the queue, in flight, or held before the
    // seed LOAD answered. This is the graceful return's condition — a cross
    // participant is a participant of record until its final change-set is
    // acknowledged. A participant never seeded holds nothing and is drained,
    // and one whose LOAD is still in flight is not (its changes wait on it).
    bool drained(std::string_view realm, std::string_view character) const override {
        const Key key{std::string(realm), std::string(character)};
        if (!seeded_.contains(key)) return true;
        if (!loaded_cursor_.contains(key)) return false;
        if (submitted_ && *submitted_ == key) return false;
        if (pending_before_load_.contains(key)) return false;
        return queue_.drained(key.first, key.second);
    }
    // Node wiring: called on the event-loop thread when a participant's last
    // queued change is durably acknowledged, so the source that was keeping
    // this participant as a participant of record can release it.
    void set_on_drain(
        std::function<void(const std::pair<std::string, std::string>&)> on_drain) {
        on_drain_ = std::move(on_drain);
    }
    std::string seed(std::string_view realm, std::string_view character) override {
        try {
            const Key key{std::string(realm), std::string(character)};
            if (seeded_.contains(key)) return "already seeding";
            seeded_.insert(key);
            send_next();
            return {};
        } catch (const std::exception&) { return "invalid"; }
    }
    std::string enqueue(std::string_view realm, std::string_view character,
                        std::uint64_t generation, std::uint64_t sequence, std::string_view payload) override {
        try {
            const Key key{std::string(realm), std::string(character)};
            if (!generation || !sequence) throw std::invalid_argument("invalid generation or sequence");
            Change change{key.first, key.second, generation, sequence, std::string(payload)};
            // The SUBMIT frame prepends "MLD1 SUBMIT <realm> <char> gen N seq N "
            // before the payload; TcpChannel::write refuses anything over
            // max_frame, so a near-limit payload would burn its retries on a
            // frame that can never be sent. Reject it up front instead.
            const auto header = submit_header(change);
            if (header + change.payload.size() > max_frame) throw std::invalid_argument("payload too large for MLD1 frame");
            // The script may enqueue before the seed LOAD has answered: hold the
            // change until the cursor is seeded, then feed it into the ordered
            // queue in submission order (the queue keeps per-participant order).
            if (!loaded_cursor_.contains(key)) {
                if (!seeded_.contains(key)) throw std::invalid_argument("unseeded participant");
                // The queue itself is bounded, so the pre-load pool shares that
                // same occupancy budget; otherwise a burst could later overflow
                // the queue during drain and strand changes silently.
                if (pending_before_load_count_ + queue_.pending() >= pending_capacity_)
                    throw std::invalid_argument("return queue is full");
                // The queue will reject a duplicate sequence once drained, so
                // reject it here too: enqueue is the caller-facing boundary and
                // must not promise a change it cannot deliver.
                if (queue_.contains(key.first, key.second, change.sequence))
                    throw std::invalid_argument("duplicate sequence");
                const auto held = pending_before_load_.find(key);
                if (held != pending_before_load_.end())
                    for (const auto& queued : held->second)
                        if (queued.sequence == change.sequence)
                            throw std::invalid_argument("duplicate sequence");
                pending_before_load_[key].push_back(std::move(change));
                ++pending_before_load_count_;
            } else queue_.enqueue(std::move(change));
            send_next();
            return {};
        } catch (const std::exception&) { return "invalid"; }
    }
    // The graceful return's settlement: one final change the framework itself
    // owns. The sequence is not chosen by the caller — it resolves against the
    // durable cursor (one past every change this participant holds) at the
    // moment the cursor exists, so the settle change always lands after
    // whatever the script queued, whenever the seed LOAD answers. Until then
    // it is held like a pre-load change, and drained() keeps the participant a
    // participant of record for exactly as long as the settle change is not
    // durably acknowledged.
    std::string settle(std::string_view realm, std::string_view character,
                       std::uint64_t generation, std::string_view payload) override {
        try {
            const Key key{std::string(realm), std::string(character)};
            if (!generation) throw std::invalid_argument("invalid generation");
            if (settle_holds_.contains(key)) throw std::invalid_argument("settle already pending");
            // The sequence resolves only once the cursor exists, so budget the
            // frame against a worst-case header (20-digit sequence and
            // generation) now: a near-limit payload must be refused here, not
            // after it waited for a cursor it can never fit.
            const auto header = 12 + key.first.size() + 1 + key.second.size()
                + 5 + 20 + 5 + 20 + 1;
            if (header + payload.size() > max_frame)
                throw std::invalid_argument("payload too large for MLD1 frame");
            if (!seeded_.contains(key) && !seed(realm, character).empty())
                throw std::invalid_argument("unseeded participant");
            if (loaded_cursor_.contains(key)) {
                resolve_settle(key, generation, payload);
            } else settle_holds_[key] = {generation, std::string(payload)};
            send_next();
            return {};
        } catch (const std::exception&) { return "invalid"; }
    }
private:
    using Key = std::pair<std::string, std::string>;
    using Change = ReturnSendQueue::Change;
    static constexpr std::size_t pending_capacity_ = 1024;
    static std::size_t submit_header(const Change& change) {
        // Exact prefix length without the payload: the frame is rebuilt in
        // send_next, so keep this in sync with it.
        return 12 + change.realm.size() + 1 + change.character.size() + 5
            + std::to_string(change.generation).size() + 5
            + std::to_string(change.sequence).size() + 1;
    }
    void connect() {
        phase_ = Phase::connecting;
        channel_ = std::make_shared<TcpChannel>(tcp::socket(io_));
        channel_->start([self=shared_from_this()](Error) { self->pump(); });
    }
    void pump() {
        if (stopping_ || !channel_) return;
        if (phase_ == Phase::connecting) {
            channel_->socket().async_connect(endpoint_, [self=shared_from_this()](Error ec) {
                if (self->stopping_) return;
                if (ec) return self->retry_later();
                // A tokened data endpoint demands the MLS1 identity before any
                // record frame; present it first and wait for the verdict.
                if (!self->identity_.empty()) {
                    self->phase_ = Phase::identifying;
                    self->writing_ = true;
                    self->channel_->write(self->identity_, [self](Error e) {
                        if (self->stopping_) return;
                        if (e) return self->retry_later();
                        // The identity write finished: release the transport
                        // slot so the ready phase can send LOAD/SUBMIT. Without
                        // this the write guard stays set forever and a tokened
                        // data endpoint never drains its return queue.
                        self->writing_ = false;
                        self->send_next();
                    });
                } else {
                    self->phase_ = Phase::ready;
                }
                self->pump();
                self->send_next();
            });
            return;
        }
        if (phase_ == Phase::identifying) {
            if (reading_) return;
            reading_ = true;
            channel_->read([self=shared_from_this()](Error ec, std::string payload) {
                self->reading_ = false;
                if (self->stopping_) return;
                if (ec) return self->retry_later();
                if (payload != "MLS1 OK") { self->diagnostics_.upstream_error(); return self->retry_later(); }
                self->phase_ = Phase::ready;
                self->send_next();
                // The identity read consumed the only armed read; re-arm the
                // ready-phase read so the LOAD/SUBMIT verdicts are observed.
                self->pump();
            });
            return;
        }
        if (reading_) return;
        reading_ = true;
        channel_->read([self=shared_from_this()](Error ec, std::string payload) {
            self->reading_ = false;
            if (self->stopping_) return;
            if (ec) return self->retry_later();
            self->receive(std::move(payload));
            self->pump();
        });
    }
    void receive(const std::string& payload) {
        try {
            if (phase_ == Phase::identifying) throw std::invalid_argument("invalid"); // guarded in pump
            if (payload.starts_with("MLD1 ACK durable ")) {
                const auto ack = parse_durable_ack(payload);
                if (!submitted_ || ack.realm != submitted_->first || ack.character != submitted_->second)
                    throw std::invalid_argument("invalid");
                (void)queue_.received(ack.realm, ack.character, ack.sequence);
                if (!queue_.ack_durable(ack)) throw std::invalid_argument("invalid");
                submitted_.reset(); submitted_sequence_ = 0;
                // The durable half of the return completes here: the last
                // change for this participant is acknowledged, so it is no
                // longer a participant of record and a source waiting on the
                // return can release its avatar. A participant that never
                // queued a change drains at the session boundary instead
                // (drained() answers true there without a callback).
                if (drained(ack.realm, ack.character) && on_drain_) on_drain_({ack.realm, ack.character});
                return send_next();
            }
            if (payload.starts_with("MLD1 RECEIVED ")) {
                std::istringstream in(payload);
                std::string magic, state, gen_tag, gen, seq_tag, seq, extra;
                std::uint64_t generation = 0, sequence = 0;
                if (!(in >> magic >> state >> gen_tag >> gen >> seq_tag >> seq) || (in >> extra)
                        || magic != "MLD1" || state != "RECEIVED"
                        || gen_tag != "gen" || seq_tag != "seq"
                        || !record_decimal(gen, generation) || !record_decimal(seq, sequence))
                    throw std::invalid_argument("invalid");
                if (!submitted_ || sequence != submitted_sequence_) throw std::invalid_argument("invalid");
                (void)queue_.received(submitted_->first, submitted_->second, sequence);
                submitted_.reset(); submitted_sequence_ = 0;
                return send_next();
            }
            if (payload.starts_with("MLD1 ERR ")) {
                // The home side refused (SEQ/STALE/FULL/STORAGE): the change
                // stays queued; release the transport slot so retry budget
                // decides whether it is reattempted. A hard STALE eventually
                // exhausts and surfaces a failed return rather than a silent drop.
                if (submitted_) (void)queue_.retry(submitted_->first, submitted_->second);
                submitted_.reset(); submitted_sequence_ = 0;
                return send_next();
            }
            if (payload.starts_with("MLD1 STATE ")) {
                std::uint64_t generation = 0, sequence = 0;
                if (!parse_load(payload, generation, sequence)) throw std::invalid_argument("invalid");
                if (!load_in_flight_) throw std::invalid_argument("invalid");
                queue_.seed(load_in_flight_->first, load_in_flight_->second, sequence);
                queue_.owner_generation(load_in_flight_->first, load_in_flight_->second, generation);
                loaded_cursor_.insert(*load_in_flight_);
                load_in_flight_.reset();
                drain_pending();
                return send_next();
            }
            if (payload == "MLD1 EMPTY") {
                if (!load_in_flight_) throw std::invalid_argument("invalid");
                queue_.seed(load_in_flight_->first, load_in_flight_->second, 0);
                // EMPTY answers LOAD for a participant the home has never
                // heard of, so the first durable acceptance is necessarily
                // that participant's first generation. Recording zero keeps
                // ack_durable's owner-generation check satisfiable instead of
                // rejecting every first ACK durable as unregistered.
                queue_.owner_generation(load_in_flight_->first, load_in_flight_->second, 0);
                loaded_cursor_.insert(*load_in_flight_);
                load_in_flight_.reset();
                drain_pending();
                return send_next();
            }
            throw std::invalid_argument("invalid");
        } catch (const std::exception&) {
            diagnostics_.upstream_error();
            retry_later();
        }
    }
    void send_next() {
        if (stopping_ || phase_ != Phase::ready || writing_) return;
        resolve_settles();
        if (!load_in_flight_ && !submitted_) {
            for (const auto& key : seeded_)
                if (!loaded_cursor_.contains(key)) { load_in_flight_ = key; break; }
        }
        if (load_in_flight_) {
            writing_ = true;
            return write("MLD1 LOAD "+load_in_flight_->first+" "+load_in_flight_->second);
        }
        if (!submitted_) {
            for (const auto& key : loaded_cursor_) {
                auto change = queue_.next(key.first, key.second);
                if (change) {
                    submitted_ = key;
                    submitted_sequence_ = change->sequence;
                    break;
                }
            }
            if (!submitted_) return;
        }
        writing_ = true;
        const auto& key = *submitted_;
        const auto change = *queue_.peek(key.first, key.second);
        // The submit claims this generation as the participant's current one,
        // so the durable ACK for exactly this generation is judgeable — for an
        // EMPTY-seeded participant (baseline zero) as much as a STATE-loaded
        // one advancing to a newer generation. A stale-owner ACK from an older
        // generation still mismatches and retries.
        queue_.owner_generation(key.first, key.second, change.generation);
        return write("MLD1 SUBMIT "+change.realm+" "+change.character
            + " gen "+std::to_string(change.generation)
            + " seq "+std::to_string(change.sequence)
            + (change.payload.empty() ? "" : " "+change.payload));
    }
    // The cursor for this participant just became known: a held settle change
    // can now take its place at the tail of the ordered queue. The frame budget
    // was pre-checked in settle() against the worst-case header, so the only
    // throw here is a full queue.
    void resolve_settle(const Key& key, std::uint64_t generation, std::string_view payload) {
        Change change{key.first, key.second, generation,
            queue_.next_sequence(key.first, key.second), std::string(payload)};
        const auto header = submit_header(change);
        if (header + change.payload.size() > max_frame)
            throw std::invalid_argument("payload too large for MLD1 frame");
        queue_.enqueue(std::move(change));
    }
    // Resolve every held settle whose cursor has since answered, at the head
    // of each pump. A full queue keeps the hold: ACKs free capacity and the
    // next pump retries, so the account is never stranded — it lands once it
    // fits, still one past everything the participant holds.
    void resolve_settles() {
        for (auto held = settle_holds_.begin(); held != settle_holds_.end();) {
            if (!loaded_cursor_.contains(held->first)) { ++held; continue; }
            try {
                resolve_settle(held->first, held->second.first, held->second.second);
            } catch (const std::exception&) { return; }
            held = settle_holds_.erase(held);
        }
    }
    // After a seed LOAD answers, feed any changes the script enqueued before
    // the cursor existed into the ordered queue (per-participant order kept).
    void drain_pending() {
        // The queue is the ordering boundary; every change that reached the
        // pre-load pool must either land in it or fail loudly. A throw here is
        // caught by receive() and turns into a retry/reconnect, but because the
        // load cursor is already recorded the key is never re-loaded, so a
        // partial drain would strand the remaining changes. Pre-validate the
        // whole batch against the queue's invariants (capacity, per-key
        // duplicates and positive generation/sequence) before moving any
        // change, so a refusal is atomic.
        std::size_t incoming = 0;
        for (const auto& key : loaded_cursor_) {
            const auto it = pending_before_load_.find(key);
            if (it == pending_before_load_.end()) continue;
            incoming += it->second.size();
            if (queue_.pending() + incoming > pending_capacity_)
                throw std::invalid_argument("return queue is full");
            for (const auto& change : it->second)
                if (!change.generation || !change.sequence)
                    throw std::invalid_argument("invalid generation or sequence");
            for (const auto& change : it->second)
                if (queue_.contains(key.first, key.second, change.sequence))
                    throw std::invalid_argument("duplicate sequence");
        }
        for (const auto& key : loaded_cursor_) {
            auto it = pending_before_load_.find(key);
            if (it == pending_before_load_.end()) continue;
            for (auto& change : it->second)
                queue_.enqueue(std::move(change));
            pending_before_load_count_ -= it->second.size();
            pending_before_load_.erase(it);
        }
    }
    void write(std::string payload) {
        channel_->write(std::move(payload), [self=shared_from_this()](Error ec) {
            self->writing_ = false;
            if (self->stopping_) return;
            if (ec) return self->retry_later();
        });
    }
    void retry_later() {
        diagnostics_.upstream_error();
        if (submitted_) (void)queue_.retry(submitted_->first, submitted_->second);
        submitted_.reset(); submitted_sequence_ = 0;
        load_in_flight_.reset();
        teardown();
        timer_.expires_after(std::chrono::milliseconds(50));
        timer_.async_wait([self=shared_from_this()](Error ec) {
            if (!ec && !self->stopping_) self->connect();
        });
    }
    void teardown() {
        reading_ = false;
        writing_ = false;
        phase_ = Phase::idle;
        if (channel_) { channel_->close(); channel_ = nullptr; }
    }
    static bool parse_load(const std::string& payload, std::uint64_t& generation,
                           std::uint64_t& sequence) {
        std::istringstream in(payload);
        std::string magic, state, gen_tag, gen, seq_tag, seq, extra;
        if (!(in >> magic >> state >> gen_tag >> gen >> seq_tag >> seq) || (in >> extra)
                || magic != "MLD1" || state != "STATE"
                || gen_tag != "gen" || seq_tag != "seq"
                || !record_decimal(gen, generation) || !record_decimal(seq, sequence)) return false;
        return true;
    }
    enum class Phase { idle, connecting, identifying, ready };
    tcp::endpoint endpoint_;
    asio::io_context& io_;
    Diagnostics& diagnostics_;
    asio::steady_timer timer_;
    ReturnSendQueue queue_;
    std::string identity_;
    std::set<Key> seeded_, loaded_cursor_;
    std::map<Key, std::vector<Change>> pending_before_load_;
    std::map<Key, std::pair<std::uint64_t, std::string>> settle_holds_;
    std::size_t pending_before_load_count_ = 0;
    std::optional<Key> load_in_flight_, submitted_;
    std::uint64_t submitted_sequence_ = 0;
    std::shared_ptr<TcpChannel> channel_;
    std::function<void(const Key&)> on_drain_;
    Phase phase_ = Phase::idle;
    bool started_ = false, stopping_ = false;
    bool reading_ = false, writing_ = false;
};

}
