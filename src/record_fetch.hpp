// SPDX-License-Identifier: Apache-2.0
// The host side of the MLD1 record endpoint: a crossServer's home join loads
// the participant's record from the home data endpoint before it can host the
// character. One connection per fetch — a login is one event, not a long-lived
// leg — so the shape is RegistryClient::lookup's connect / write / read one
// verdict / answer, with a deadline that cancels a silent endpoint. The
// sender side of the same protocol (one ordered return queue, one persistent
// leg) is return_sender.hpp, and the two are deliberately different: nothing
// here is retried, because the join has its own refusal.
#pragma once
#include "tcp_channel.hpp"
#include "node_internal.hpp"
#include "mistlane/record.hpp"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

namespace mistlane {

class RecordFetcher : public std::enable_shared_from_this<RecordFetcher> {
public:
    // One loaded record: the opaque change-set bytes plus the generation and
    // sequence the home store holds. present=false means the data node answered
    // MLD1 EMPTY (no record committed for this participant), not a failure.
    struct Record {
        std::uint64_t generation = 0, sequence = 0;
        std::string payload;
        bool present = false;
    };
    using Answer = std::function<void(Record record, std::string reason)>;
    // Answers exactly once, on the io thread: present=true with the STATE
    // bytes on success; present=false with reason "EMPTY" when the data node
    // holds no record; otherwise present=false with a short reason —
    // "unreachable" (no leg), "refused" (MLS1 refused or no OK),
    // "INVALID"/"STALE"/... (the home side answered MLD1 ERR <word>) or
    // "timeout" (the deadline passed). `identity` is the MLS1 hello presented
    // when the data endpoint is tokened (the crossServer's service_hello);
    // empty for a plain one.
    static void load(asio::io_context& io, Endpoint data, std::string identity,
                     std::string_view realm, std::string_view character, Answer answer) {
        std::shared_ptr<RecordFetcher> self(new RecordFetcher(io, data, std::move(identity)));
        self->question_ = "MLD1 LOAD " + std::string(realm) + " " + std::string(character);
        self->answer_ = std::move(answer);
        self->start();
    }
private:
    RecordFetcher(asio::io_context& io, Endpoint data, std::string identity)
      : io_(io), endpoint_(tcp::endpoint(parse_address(data.address), data.port)),
        identity_(std::move(identity)),
        channel_(std::make_shared<TcpChannel>(tcp::socket(io))), timer_(io) {}
    void start() {
        deadline();
        channel_->socket().async_connect(endpoint_, [self=shared_from_this()](Error ec) {
            if (self->done_) return;
            if (ec) return self->answer({}, "unreachable");
            if (self->identity_.empty()) return self->ask();
            // A tokened data endpoint refuses every record frame until the
            // peer identifies (session plane: refuse_service MLS1 TOKEN).
            self->channel_->write(self->identity_, [self](Error ec) {
                if (self->done_) return;
                if (ec) return self->answer({}, "unreachable");
                self->channel_->read([self](Error ec, std::string payload) {
                    if (self->done_) return;
                    if (ec) return self->answer({}, "unreachable");
                    if (payload != "MLS1 OK") return self->answer({}, "refused");
                    self->ask();
                });
            });
        });
    }
    void ask() {
        channel_->write(question_, [self=shared_from_this()](Error ec) {
            if (self->done_) return;
            if (ec) return self->answer({}, "unreachable");
            self->channel_->read([self](Error ec, std::string payload) {
                if (self->done_) return;
                if (ec) return self->answer({}, "unreachable");
                self->verdict(std::move(payload));
            });
        });
    }
    void verdict(std::string payload) {
        if (payload == "MLD1 EMPTY") return answer({}, "EMPTY");
        if (payload.starts_with("MLD1 STATE ")) {
            Record record;
            if (!parse_state(payload, record)) return answer({}, "unexpected");
            return answer(std::move(record), {});
        }
        if (payload.starts_with("MLD1 ERR ")) return answer({}, payload.substr(9));
        // An MLS1 verdict means the handshake was refused (TOKEN/INVALID/...),
        // not the record frame — a tokened data endpoint answered it.
        if (payload.starts_with("MLS1")) return answer({}, "refused");
        return answer({}, "unexpected");
    }
    // "MLD1 STATE gen <n> seq <m> [<payload>]" — the change-set is everything
    // past one separating space, opaque and space-bearing, so the header is
    // scanned before the payload is sliced (a LOAD reply never carries a tag
    // like SUBMIT does, and an empty payload is a valid record).
    static bool parse_state(const std::string& payload, Record& record) {
        std::istringstream in(payload);
        std::string magic, state, gen_tag, gen, seq_tag, seq;
        if (!(in >> magic >> state >> gen_tag >> gen >> seq_tag >> seq)
                || magic != "MLD1" || state != "STATE"
                || gen_tag != "gen" || seq_tag != "seq"
                || !record_decimal(gen, record.generation) || !record_decimal(seq, record.sequence))
            return false;
        record.present = true;
        if (const auto at = in.tellg(); at >= 0) {
            auto offset = static_cast<std::size_t>(at);
            if (offset < payload.size() && payload[offset] == ' ') ++offset;
            record.payload.assign(payload.data()+offset, payload.size()-offset);
        }
        return true;
    }
    void deadline() {
        timer_.expires_after(std::chrono::seconds(10));
        timer_.async_wait([self=shared_from_this()](Error ec) {
            if (!ec && !self->done_) self->answer({}, "timeout");
        });
    }
    void answer(Record record, std::string reason) {
        if (done_) return;
        done_ = true;
        timer_.cancel();
        channel_->close();
        answer_(std::move(record), std::move(reason));
    }
    asio::io_context& io_;
    tcp::endpoint endpoint_;
    std::string identity_, question_;
    std::shared_ptr<TcpChannel> channel_;
    asio::steady_timer timer_;
    Answer answer_;
    bool done_ = false;
};
}