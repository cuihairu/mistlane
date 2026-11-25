// SPDX-License-Identifier: Apache-2.0
// The host side of the MLD1 record endpoint, as a one-shot client. A
// crossServer's home join loads the participant's record from the home data
// endpoint before it can host the character; a login's entry flow asks the
// same endpoint for the data-issued hosting generation. One connection per
// question — a login is one event, not a long-lived leg — so the shape is
// RegistryClient::lookup's connect / write / read one verdict / answer, with
// a deadline that cancels a silent endpoint. The sender side of the same
// protocol (one ordered return queue, one persistent leg) is return_sender.hpp,
// and the two are deliberately different: nothing here is retried, because the
// join and the entry flow have their own refusal.
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
    using GrantAnswer = std::function<void(std::uint64_t generation, std::string reason)>;
    // The write side of the record protocol (SUBMIT / FENCE): ok=true on the
    // success verdict, ok=false with the short reason word otherwise.
    using WriteAnswer = std::function<void(bool ok, std::string reason)>;
    // One read-only home-facts answer (the MLA1 OWNER callback's source).
    // present=false means MLD1 OWNER NONE (no record and no lease exist),
    // not a failure.
    struct Ownership {
        std::string owner; // empty = the lease is unnamed
        std::uint64_t generation = 0;
        bool fenced = false;
        bool present = false;
    };
    using OwnershipAnswer = std::function<void(Ownership facts, std::string reason)>;
    // Answers exactly once, on the executor's thread that started it: present=true with
    // the STATE bytes on success; present=false with reason "EMPTY" when the
    // data node holds no record; otherwise present=false with a short reason —
    // "unreachable" (no leg), "refused" (MLS1 refused or no OK),
    // "INVALID"/"STALE"/... (the home side answered MLD1 ERR <word>) or
    // "timeout" (the deadline passed). `identity` is the MLS1 hello presented
    // when the data endpoint is tokened (the crossServer's service_hello);
    // empty for a plain one. The executor is the caller's event loop, so the
    // answer re-enters the same serial thread that started the fetch.
    static void load(asio::any_io_executor executor, Endpoint data, std::string identity,
                     std::string_view realm, std::string_view character, Answer answer) {
        std::shared_ptr<RecordFetcher> self(
            new RecordFetcher(std::move(executor), data, std::move(identity), Kind::Load));
        self->question_ = "MLD1 LOAD " + std::string(realm) + " " + std::string(character);
        self->answer_ = std::move(answer);
        self->start();
    }
    // The login entry flow's grant: asks the data endpoint for the data-issued
    // hosting generation of a participant. Answers exactly once with the
    // granted generation and an empty reason, or generation 0 and a short
    // reason (the same words load() uses, plus "EMPTY" never applies — a grant
    // allocates, so it always produces a generation or a refusal).
    static void grant(asio::any_io_executor executor, Endpoint data, std::string identity,
                      std::string_view realm, std::string_view character,
                      std::string_view instance, GrantAnswer answer) {
        std::shared_ptr<RecordFetcher> self(
            new RecordFetcher(std::move(executor), data, std::move(identity), Kind::Grant));
        self->question_ = "MLD1 GRANT " + std::string(realm) + " " + std::string(character)
            + (instance.empty() ? "" : " " + std::string(instance));
        self->grant_answer_ = std::move(answer);
        self->start();
    }
    // The apiServer's read-only home-facts lookup (MLA1 OWNER's source): asks
    // the data endpoint what it knows about one participant's ownership and
    // never changes it. Answers exactly once with the facts and an empty
    // reason, present=false with reason "NONE" when the participant is unknown
    // to the home store, or present=false with a short failure reason ("the
    // same words load() uses"). The facts' owner is empty while the lease is
    // unnamed, and fenced=true while the top generation is frozen mid-lease.
    static void ownership(asio::any_io_executor executor, Endpoint data, std::string identity,
                          std::string_view realm, std::string_view character,
                          OwnershipAnswer answer) {
        std::shared_ptr<RecordFetcher> self(
            new RecordFetcher(std::move(executor), data, std::move(identity), Kind::Ownership));
        self->question_ = "MLD1 OWNERSHIP " + std::string(realm) + " " + std::string(character);
        self->ownership_answer_ = std::move(answer);
        self->start();
    }
    // The source transfer's final change: submit the state snapshot as the
    // next ordered change at the current generation. Answers exactly once with
    // ok=true on MLD1 ACK durable (journaled) or MLD1 RECEIVED (volatile), or
    // ok=false with a short reason — "unreachable"/"refused"/"timeout"/
    // "INVALID"/"STALE"/"SEQ"/"UNGRANTED"/"FULL"/"STORAGE"/"unexpected".
    // The payload is opaque and space-bearing, appended after one separating
    // space (an empty payload is a valid record, matching return_sender).
    static void submit(asio::any_io_executor executor, Endpoint data, std::string identity,
                       std::string_view realm, std::string_view character,
                       std::uint64_t generation, std::uint64_t sequence,
                       std::string payload, WriteAnswer answer) {
        std::shared_ptr<RecordFetcher> self(
            new RecordFetcher(std::move(executor), data, std::move(identity), Kind::Submit));
        self->question_ = "MLD1 SUBMIT " + std::string(realm) + " " + std::string(character)
            + " gen " + std::to_string(generation) + " seq " + std::to_string(sequence)
            + (payload.empty() ? "" : " " + payload);
        self->write_answer_ = std::move(answer);
        self->start();
    }
    // The transfer tombstone: fence the current generation so no older writer
    // resumes. Answers exactly once with ok=true on MLD1 FENCED, or ok=false
    // with the short reason ("unreachable"/"refused"/"timeout"/"NOFENCE"/
    // "STALE"/"STORAGE"/"unexpected").
    static void fence(asio::any_io_executor executor, Endpoint data, std::string identity,
                      std::string_view realm, std::string_view character,
                      std::uint64_t generation, WriteAnswer answer) {
        std::shared_ptr<RecordFetcher> self(
            new RecordFetcher(std::move(executor), data, std::move(identity), Kind::Fence));
        self->question_ = "MLD1 FENCE " + std::string(realm) + " " + std::string(character)
            + " gen " + std::to_string(generation);
        self->write_answer_ = std::move(answer);
        self->start();
    }
private:
    enum class Kind { Load, Grant, Submit, Fence, Ownership };
    RecordFetcher(asio::any_io_executor executor, Endpoint data, std::string identity, Kind kind)
      : endpoint_(tcp::endpoint(parse_address(data.address), data.port)),
        identity_(std::move(identity)), kind_(kind),
        channel_(std::make_shared<TcpChannel>(tcp::socket(executor))), timer_(executor) {}
    void start() {
        deadline();
        channel_->socket().async_connect(endpoint_, [self=shared_from_this()](Error ec) {
            if (self->done_) return;
            if (ec) return self->fail("unreachable");
            if (self->identity_.empty()) return self->ask();
            // A tokened data endpoint refuses every record frame until the
            // peer identifies (session plane: refuse_service MLS1 TOKEN).
            self->channel_->write(self->identity_, [self](Error ec) {
                if (self->done_) return;
                if (ec) return self->fail("unreachable");
                self->channel_->read([self](Error ec, std::string payload) {
                    if (self->done_) return;
                    if (ec) return self->fail("unreachable");
                    if (payload != "MLS1 OK") return self->fail("refused");
                    self->ask();
                });
            });
        });
    }
    void ask() {
        channel_->write(question_, [self=shared_from_this()](Error ec) {
            if (self->done_) return;
            if (ec) return self->fail("unreachable");
            self->channel_->read([self](Error ec, std::string payload) {
                if (self->done_) return;
                if (ec) return self->fail("unreachable");
                self->verdict(std::move(payload));
            });
        });
    }
    void verdict(std::string payload) {
        if (kind_ == Kind::Load) {
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
        // MLD1 OWNER NONE / MLD1 OWNER AT <instance|-> gen <n> <ACTIVE|FENCED>.
        if (kind_ == Kind::Ownership) {
            if (payload == "MLD1 OWNER NONE") return ownership({}, "NONE");
            if (payload.starts_with("MLD1 OWNER AT ")) {
                std::istringstream in(payload);
                std::string magic, owner_word, owner_tag, owner, gen_tag, gen, state;
                std::uint64_t generation = 0;
                if (!(in >> magic >> owner_word >> owner_tag >> owner >> gen_tag >> gen >> state)
                        || magic != "MLD1" || owner_word != "OWNER" || owner_tag != "AT"
                        || gen_tag != "gen"
                        || !record_decimal(gen, generation)
                        || (state != "ACTIVE" && state != "FENCED")
                        || (owner != "-" && !record_identifier(owner)))
                    return ownership({}, "unexpected");
                return ownership({owner == "-" ? "" : owner, generation,
                    state == "FENCED", true}, {});
            }
            if (payload.starts_with("MLD1 ERR ")) return ownership({}, payload.substr(9));
            if (payload.starts_with("MLS1")) return ownership({}, "refused");
            return ownership({}, "unexpected");
        }
        // MLD1 SUBMIT / MLD1 FENCE: the success verdict is all the caller
        // needs (the echoed generation and sequence are its own frame's), the
        // refusal is the ERR word — including ERR SEQ seq <n> for a submit
        // past the committed sequence.
        if (kind_ == Kind::Submit || kind_ == Kind::Fence) {
            const bool ok = kind_ == Kind::Submit
                ? (payload.starts_with("MLD1 ACK durable ") || payload.starts_with("MLD1 RECEIVED "))
                : payload.starts_with("MLD1 FENCED ");
            if (ok) return write_answer(true, {});
            if (payload.starts_with("MLD1 ERR ")) return write_answer(false, payload.substr(9));
            if (payload.starts_with("MLS1")) return write_answer(false, "refused");
            return write_answer(false, "unexpected");
        }
        // MLD1 GRANTED gen <n> — the granted generation, nothing else.
        if (payload.starts_with("MLD1 GRANTED ")) {
            std::istringstream in(payload);
            std::string magic, granted, gen_tag, gen;
            std::uint64_t generation = 0;
            if (!(in >> magic >> granted >> gen_tag >> gen) || magic != "MLD1"
                    || granted != "GRANTED" || gen_tag != "gen"
                    || !record_decimal(gen, generation)) return grant_answer(0, "unexpected");
            return grant_answer(generation, {});
        }
        if (payload.starts_with("MLD1 ERR ")) return grant_answer(0, payload.substr(9));
        if (payload.starts_with("MLS1")) return grant_answer(0, "refused");
        return grant_answer(0, "unexpected");
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
            if (!ec && !self->done_) self->fail("timeout");
        });
    }
    void answer(Record record, std::string reason) {
        if (done_) return;
        done_ = true;
        timer_.cancel();
        channel_->close();
        answer_(std::move(record), std::move(reason));
    }
    void grant_answer(std::uint64_t generation, std::string reason) {
        if (done_) return;
        done_ = true;
        timer_.cancel();
        channel_->close();
        grant_answer_(generation, std::move(reason));
    }
    void write_answer(bool ok, std::string reason) {
        if (done_) return;
        done_ = true;
        timer_.cancel();
        channel_->close();
        write_answer_(ok, std::move(reason));
    }
    void ownership(Ownership facts, std::string reason) {
        if (done_) return;
        done_ = true;
        timer_.cancel();
        channel_->close();
        ownership_answer_(std::move(facts), std::move(reason));
    }
    // Every refusal that is not a protocol verdict — no leg, a refused
    // handshake, a deadline — is delivered to whichever question was asked.
    void fail(std::string reason) {
        if (kind_ == Kind::Load) return answer({}, std::move(reason));
        if (kind_ == Kind::Grant) return grant_answer(0, std::move(reason));
        if (kind_ == Kind::Ownership) return ownership({}, std::move(reason));
        return write_answer(false, std::move(reason));
    }
    tcp::endpoint endpoint_;
    std::string identity_, question_;
    Kind kind_ = Kind::Load;
    std::shared_ptr<TcpChannel> channel_;
    asio::steady_timer timer_;
    Answer answer_;
    GrantAnswer grant_answer_;
    WriteAnswer write_answer_;
    OwnershipAnswer ownership_answer_;
    bool done_ = false;
};
}