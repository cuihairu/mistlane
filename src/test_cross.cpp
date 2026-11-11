// SPDX-License-Identifier: Apache-2.0
#include "test_common.hpp"
namespace mistlane::test {
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
// Host-side entry ticket verification: a crossServer configured with per-realm
// secrets verifies the join's MLT1 ticket against its own instance and the
// declared identity, a replayed or forged ticket refuses the join with one
// word, an unconfigured host admits the same join with no eligibility, and
// broken secret configuration refuses startup.
void cross_ticket() {
    Context c; write(c.path("host.lua"),
        "return {message=function(p,c)"
        " return (c.character or 'anon')..' verified='..tostring(c.verified==true) end}");
    ApiSecret secret{};
    for (std::size_t i = 0; i < secret.size(); ++i) secret[i] = static_cast<unsigned char>(i*11+3);
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    for (unsigned char byte : secret) { hex += digits[byte/16]; hex += digits[byte%16]; }
    ApiSecret forged{}; forged.fill(7);
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto nonce = [](std::uint64_t serial) {
        std::string out(32, '0');
        for (int i = 31; i >= 0 && serial; --i, serial >>= 4)
            out[static_cast<std::size_t>(i)] = "0123456789abcdef"[serial & 15];
        return out;
    };
    // Serial keeps every minted nonce distinct; the replay probe reuses one.
    const auto mint = [&](std::uint64_t serial, const std::string& instance, std::uint64_t expires,
                          const ApiSecret& key, const std::string& character = "hero") {
        EntryTicket ticket;
        ticket.character = {"alpha", character};
        ticket.instance = instance;
        ticket.generation = 7;
        ticket.expires = expires;
        ticket.nonce = nonce(serial);
        ticket.signature = entry_ticket_sign(ticket, key);
        return ticket;
    };
    const auto join_with = [](cli::Client& peer, const std::string& tail) {
        return peer.request("MLC1 1 alpha arena r1 ch hero gen 7 ret alpha arena r1"+tail);
    };
    const auto plain = port(), checking = port();
    // Without secrets a carried ticket authorizes nothing: the join is valid
    // wire shape, so an unconfigured host still admits it — eligibility only
    // comes from verification, never from the token's presence.
    c.node("crossServer", plain, {"--script", c.path("host.lua").string(), "--service-name", "xs-a"});
    {
        auto peer = tcp(plain);
        equal(join_with(*peer, " tk "+entry_ticket_hex(mint(1, "xs-a", now+120, secret))), "MLC1 OK 1");
        equal(peer->request("who"), "hero verified=false");
    }
    std::string carried;
    {
        ScopedEnv secrets("MISTLANE_TICKET_SECRETS", "alpha:"+hex);
        c.node("crossServer", checking, {"--script", c.path("host.lua").string(), "--service-name", "xs-a"});
    }
    {   // An anonymous join stays admitted, and stays read-only.
        auto anon = tcp(checking);
        equal(anon->request("MLC1 1 alpha arena r1"), "MLC1 OK 1");
        equal(anon->request("who"), "anon verified=false");
    }
    {   // The honest ticket verifies under the home realm secret, this
        // instance and the declared generation.
        auto peer = tcp(checking);
        carried = entry_ticket_hex(mint(2, "xs-a", now+120, secret));
        equal(join_with(*peer, " tk "+carried), "MLC1 OK 1");
        equal(peer->request("who"), "hero verified=true");
    }
    {   // The same nonce never admits twice inside one process.
        auto replay = tcp(checking);
        equal(join_with(*replay, " tk "+carried), "MLC1 ERR TICKET");
    }
    // Every failed verdict answers the same one word; the specific reason
    // stays on the operator's side of the log.
    for (const auto& ticket : {
            mint(3, "xs-b", now+120, secret),   // another host's ticket
            mint(4, "xs-a", now-10, secret),    // expired before this join
            mint(5, "xs-a", now+120, forged),   // signed by the wrong secret
        })
        { auto peer = tcp(checking); equal(join_with(*peer, " tk "+entry_ticket_hex(ticket)), "MLC1 ERR TICKET"); }
    {   // A ticket naming a different character than the tail claims is an
        // INVALID join — the wire binding refuses it before verification runs.
        auto peer = tcp(checking);
        equal(join_with(*peer, " tk "+entry_ticket_hex(mint(6, "xs-a", now+120, secret, "other"))),
              "MLC1 ERR INVALID");
    }
    // Broken or misdirected secret configuration refuses startup, named.
    const auto refuses = [&](const std::string& value, const char* role, const char* instance) {
        ScopedEnv secrets("MISTLANE_TICKET_SECRETS", value);
        std::vector<std::string> args{self().string(), "--role", role, "--listen", endpoint(port()),
            "--script", c.path("host.lua").string()};
        if (instance) { args.push_back("--service-name"); args.push_back(instance); }
        auto& refused = c.spawn(args);
        require(refused.wait() != 0 && refused.output().find("MISTLANE_TICKET_SECRETS") != std::string::npos,
                "broken ticket secrets must refuse startup");
    };
    refuses("alpha:"+hex+",beta:short", "crossServer", "xs-c"); // bad hex row
    refuses("alpha", "crossServer", "xs-c");                    // no secret at all
    refuses("al!pha:"+hex, "crossServer", "xs-c");              // realm is not an identifier
    refuses("alpha:"+hex, "crossServer", nullptr);              // host without a stable name
    refuses("alpha:"+hex, "gameServer", "xs-c");                // secrets on a non-host role
    std::cout << "cross.ticket: host verifies MLT1 on the join, replay/forgery/expiry refuse TICKET, "
                 "unconfigured host admits without eligibility, bad secrets refuse startup [PASS]\n";
}
// Login-side issuance plus host-side verification: a login node configured
// with MISTLANE_TICKET_SECRETS signs a fresh ticket from Lua, and a
// crossServer with the same secret verifies it on the join. The same ticket
// is refused on a second use, and a secret on a non-login/non-cross role
// refuses startup.
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
    equal(ask(leg, "MLD1 SUBMIT away hero gen 1 seq 1 hp=3"), "MLD1 RECEIVED gen 1 seq 1");
    equal(ask(leg, "MLD1 LOAD home hero"), "MLD1 STATE gen 6 seq 2 taken");
    equal(ask(leg, "MLD1 LOAD away hero"), "MLD1 STATE gen 1 seq 1 hp=3");
    // Inject sequence 2 before sequence 1. The sender queue holds the gap,
    // then emits both frames in order to the real framed-TCP endpoint.
    ReturnSendQueue ordered(3);
    ordered.seed("order", "hero", 0);
    ordered.enqueue({"order", "hero", 2, 2, "second"});
    require(!ordered.next("order", "hero"), "sender emitted across a sequence gap");
    ordered.enqueue({"order", "hero", 2, 1, "first"});
    auto first = ordered.next("order", "hero");
    require(first && first->sequence == 1, "sender did not select missing sequence first");
    const auto submit = [](const ReturnSendQueue::Change& change) {
        return "MLD1 SUBMIT " + change.realm + " " + change.character + " gen "
            + std::to_string(change.generation) + " seq " + std::to_string(change.sequence)
            + " " + change.payload;
    };
    equal(ask(leg, submit(*first)), "MLD1 RECEIVED gen 2 seq 1");
    require(ordered.received("order", "hero", 1), "sender did not release first receipt");
    auto second_change = ordered.next("order", "hero");
    require(second_change && second_change->sequence == 2, "sender did not continue in order");
    equal(ask(leg, submit(*second_change)), "MLD1 RECEIVED gen 2 seq 2");
    require(ordered.received("order", "hero", 2), "sender did not release second receipt");
    require(!ordered.ack_durable("order", "hero", 2), "queue accepted an out-of-order durable ACK");
    require(ordered.ack_durable("order", "hero", 1), "queue rejected first durable ACK");
    require(ordered.ack_durable("order", "hero", 2), "queue rejected second durable ACK");
    equal(ask(leg, "MLD1 LOAD order hero"), "MLD1 STATE gen 2 seq 2 second");
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
    require(results("accepted") == 5, "accepted counter mismatch");
    require(results("loaded") == 5, "loaded counter mismatch");
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
void cross_record_journal() {
    Context c;
    write(c.path("echo.lua"), echo);
    const auto first = port(), second = port(), isolated = port();
    const auto journal = c.path("records.sqlite");
    auto& initial = c.node("data", first, {"--script", c.path("echo.lua").string(),
        "--journal", journal.string()});
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
    const auto ask = [&send_frame, &read_frame](unsigned listen, const std::string& line) {
        RawSocket raw(listen);
        send_frame(raw, line);
        return read_frame(raw);
    };
    // A durable data node answers ACK durable only after the write is stored.
    equal(ask(first, "MLD1 LOAD home hero"), "MLD1 EMPTY");
    equal(ask(first, "MLD1 SUBMIT home hero gen 5 seq 1 hp=10 mp=5"),
        "MLD1 ACK durable home hero gen 5 seq 1");
    equal(ask(first, "MLD1 LOAD home hero"), "MLD1 STATE gen 5 seq 1 hp=10 mp=5");
    equal(ask(first, "MLD1 SUBMIT home hero gen 5 seq 1 hp=99"),
        "MLD1 ACK durable home hero gen 5 seq 1");
    equal(ask(first, "MLD1 LOAD home hero"), "MLD1 STATE gen 5 seq 1 hp=10 mp=5");
    equal(ask(first, "MLD1 SUBMIT home hero gen 6 seq 2 hp=12"),
        "MLD1 ACK durable home hero gen 6 seq 2");
    equal(ask(first, "MLD1 SUBMIT home hero gen 5 seq 3 late"), "MLD1 ERR STALE gen 6");
    equal(ask(first, "MLD1 SUBMIT home hero gen 7 seq 9 jump"), "MLD1 ERR SEQ seq 3");
    equal(ask(first, "MLD1 SUBMIT away hero gen 1 seq 1 hp=3"),
        "MLD1 ACK durable away hero gen 1 seq 1");
    initial.signal(SIGTERM);
    require(initial.wait() == 0, "durable data node did not stop cleanly");

    // Crash reload: LOAD, fence, sequence and the committed ACK all survive.
    c.node("data", second, {"--script", c.path("echo.lua").string(),
        "--journal", journal.string()});
    equal(ask(second, "MLD1 LOAD home hero"), "MLD1 STATE gen 6 seq 2 hp=12");
    equal(ask(second, "MLD1 LOAD away hero"), "MLD1 STATE gen 1 seq 1 hp=3");
    // A lost durable reply replays the committed ACK without altering state.
    equal(ask(second, "MLD1 SUBMIT home hero gen 6 seq 2 hp=99"),
        "MLD1 ACK durable home hero gen 6 seq 2");
    equal(ask(second, "MLD1 LOAD home hero"), "MLD1 STATE gen 6 seq 2 hp=12");
    equal(ask(second, "MLD1 SUBMIT home hero gen 5 seq 3 late"), "MLD1 ERR STALE gen 6");
    equal(ask(second, "MLD1 SUBMIT home hero gen 7 seq 9 jump"), "MLD1 ERR SEQ seq 3");
    equal(ask(second, "MLD1 SUBMIT home hero gen 7 seq 3 recovered"), "MLD1 ACK durable home hero gen 7 seq 3");
    equal(ask(second, "MLD1 SUBMIT home hero gen 8 seq 5 after"), "MLD1 ERR SEQ seq 4");
    equal(ask(second, "MLD1 SUBMIT home hero gen 8 seq 4 after"), "MLD1 ACK durable home hero gen 8 seq 4");
    // A data node without a store stays volatile: no ACK, no fabricated state.
    c.node("data", isolated, {"--script", c.path("echo.lua").string()});
    equal(ask(isolated, "MLD1 LOAD home hero"), "MLD1 EMPTY");
    equal(ask(isolated, "MLD1 SUBMIT home hero gen 1 seq 1 x"), "MLD1 RECEIVED gen 1 seq 1");
    std::cout << "record.journal: durable MLD1 write survives restart, ACK durable is the "
        "only commitment and replay is idempotent [PASS]\n";
}
void cross_home_sequence() {
    // The home final-sequence authority is the data-side tombstone: once a
    // migration fences a generation at its current cursor, that generation
    // cannot advance the cursor. A resend of the frozen cursor is answered as a
    // duplicate, and only a strictly newer generation may resume from it.
    RecordStore store;
    std::string_view result;
    equal(store.handle("MLD1 SUBMIT home hero gen 1 seq 1 v=1", result), "MLD1 RECEIVED gen 1 seq 1");
    equal(store.handle("MLD1 SUBMIT home hero gen 1 seq 2 v=2", result), "MLD1 RECEIVED gen 1 seq 2");
    // Freezing at generation 1 pins the final cursor at sequence 2.
    const auto pin = store.fence({"home", "hero"}, 1);
    require(pin && pin->generation == 1 && pin->sequence == 2, "home fence did not pin the final cursor");
    require(store.final_sequence({"home", "hero"}) == pin, "final sequence query lost the pin");
    // The same fence is idempotent; a different generation is refused.
    require(store.fence({"home", "hero"}, 1) == pin, "home fence was not idempotent");
    require(!store.fence({"home", "hero"}, 2), "a changed-generation fence was accepted");
    // The old generation can only resend the frozen cursor, never advance.
    equal(store.handle("MLD1 SUBMIT home hero gen 1 seq 2 v=2", result), "MLD1 RECEIVED gen 1 seq 2");
    equal(store.handle("MLD1 SUBMIT home hero gen 1 seq 3 late", result), "MLD1 ERR STALE gen 1");
    // A newer generation resumes from the frozen cursor, not from one.
    equal(store.handle("MLD1 SUBMIT home hero gen 2 seq 3 v=3", result), "MLD1 RECEIVED gen 2 seq 3");
    equal(store.handle("MLD1 SUBMIT home hero gen 2 seq 4 v=4", result), "MLD1 RECEIVED gen 2 seq 4");
    // An unrelated key is independent and unfenced.
    require(!store.final_sequence({"home", "other"}), "an unfenced key reported a final sequence");
    equal(store.handle("MLD1 SUBMIT home other gen 1 seq 1 v=1", result), "MLD1 RECEIVED gen 1 seq 1");
    // A fenced store refuses a fence for a key that has no committed cursor.
    require(!store.fence({"away", "hero"}, 1), "a fence without a record was accepted");
    std::cout << "record.home.sequence: final cursor pins at the fence, old generation "
        "cannot advance, newer generation resumes and keys are independent [PASS]\n";
}
void cross_home_sequence_journal() {
    // The durable final-sequence pin survives a store restart and keeps the
    // old generation tombstoned; the newer generation resumes from the pin.
    Context c;
    write(c.path("echo.lua"), echo);
    const auto first = port(), second = port();
    const auto journal = c.path("home-sequence.sqlite");
    auto& initial = c.node("data", first, {"--script", c.path("echo.lua").string(),
        "--journal", journal.string()});
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
    const auto ask = [&](unsigned listen, const std::string& line) {
        RawSocket raw(listen);
        send_frame(raw, line);
        return read_frame(raw);
    };
    // A fenced generation is not writable through MLD1 in this slice; the
    // endpoint pins only when the store is asked to fence. This uses the
    // journal boundary directly to prove the pin persists.
    {
        MigrationJournal store(journal.string());
        store.save_record({"home", "hero", 1, 2, "v=2"});
        store.save_fence({{"home", "hero"}, 1, 2});
    }
    initial.signal(SIGTERM);
    require(initial.wait() == 0, "home sequence node did not stop cleanly");
    c.node("data", second, {"--script", c.path("echo.lua").string(),
        "--journal", journal.string()});
    equal(ask(second, "MLD1 LOAD home hero"), "MLD1 STATE gen 1 seq 2 v=2");
    // A durable resend of the frozen cursor dedups; an advance is stale.
    equal(ask(second, "MLD1 SUBMIT home hero gen 1 seq 2 v=2"),
        "MLD1 ACK durable home hero gen 1 seq 2");
    equal(ask(second, "MLD1 SUBMIT home hero gen 1 seq 3 late"), "MLD1 ERR STALE gen 1");
    equal(ask(second, "MLD1 SUBMIT home hero gen 2 seq 3 v=3"),
        "MLD1 ACK durable home hero gen 2 seq 3");
    std::cout << "record.home.sequence.journal: the durable pin fences the old "
        "generation after restart and the newer generation resumes [PASS]\n";
}
// The sender-side slice of the return protocol: the crossServer owns an
// ordered retry queue against a real framed data endpoint. The script only
// seeds and enqueues; the sender performs LOAD, submits in sequence and
// removes a change only when home data answers ACK durable.
void cross_return_sender() {
    Context c;
    write(c.path("return.lua"),
        "local M = {}\n"
        "function M.message(payload, context)\n"
        "  if payload == 'seed' then\n"
        "    local ok, reason = mistlane.returning.seed('home', 'hero')\n"
        "    if not ok then return 'SEED '..tostring(reason) end\n"
        "    return 'SEEDED'\n"
        "  end\n"
        "  if payload == 'send' then\n"
        "    local ok, reason = mistlane.returning.enqueue('home','hero',5,1,'hp=10')\n"
        "    if not ok then return 'ENQ '..tostring(reason) end\n"
        "    return 'ENQUEUED'\n"
        "  end\n"
        "  if payload == 'send2' then\n"
        "    local ok, reason = mistlane.returning.enqueue('home','hero',5,2,'hp=20')\n"
        "    if not ok then return 'ENQ2 '..tostring(reason) end\n"
        "    return 'ENQUEUED2'\n"
        "  end\n"
        "  if payload == 'neg' then\n"
        "    local ok, reason = pcall(mistlane.returning.enqueue,'home','hero',-1,1,'hp=10')\n"
        "    if ok then return 'NEG ACCEPTED' end\n"
        "    return 'NEG '..tostring(reason)\n"
        "  end\n"
        "  if payload == 'zero' then\n"
        "    local ok, reason = pcall(mistlane.returning.enqueue,'home','hero',5,0,'hp=10')\n"
        "    if ok then return 'ZERO ACCEPTED' end\n"
        "    return 'ZERO '..tostring(reason)\n"
        "  end\n"
        "  if payload == 'dup' then\n"
        "    local a, ra = mistlane.returning.enqueue('home','hero',5,999,'hp=10')\n"
        "    if not a then return 'SEEDFAIL '..tostring(ra) end\n"
        "    local b, rb = mistlane.returning.enqueue('home','hero',5,999,'hp=10')\n"
        "    if b then return 'DUP ACCEPTED' end\n"
        "    return 'DUP '..tostring(rb)\n"
        "  end\n"
        "  if payload == 'unseed' then\n"
        "    -- enqueue before seed: the pre-load pool must refuse a participant\n"
        "    -- it cannot ever LOAD, instead of holding the change forever.\n"
        "    -- ReturnSender::enqueue reports a refusal as false,reason rather\n"
        "    -- than throwing, so the probe must read the return values instead\n"
        "    -- of pcall+ok (which stays true on a graceful refusal).\n"
        "    local ok, reason = mistlane.returning.enqueue('away','ghost',5,1,'hp=10')\n"
        "    if ok then return 'UNSEED ACCEPTED' end\n"
        "    return 'UNSEED '..tostring(reason)\n"
        "  end\n"
        "  return 'noop'\n"
        "end\n"
        "return M");
    write(c.path("echo.lua"), echo);
    const auto data_port=port(), cross=port();
    const auto journal=c.path("return-records.sqlite");
    // A tokened data endpoint demands MLS1 on every leg; the crossServer
    // presents it through --upstream-token. This is the path the return
    // sender's identify handshake must drain without wedging the write guard.
    c.node("data", data_port, {"--script", c.path("echo.lua").string(),
        "--journal", journal.string(), "--service-token", "s3cret"});
    c.node("crossServer", cross, {"--script", c.path("return.lua").string(),
        "--data", endpoint(data_port), "--upstream-token", "s3cret",
        "--service-name", "xs:1"});
    const auto send_frame=[](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length=static_cast<std::uint32_t>(payload.size());
        for (int i=3; i>=0; --i) { frame[i]=static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame=[](RawSocket& raw) {
        const auto header=raw.receive(4);
        std::size_t length=0;
        for (unsigned char ch : header) length=(length << 8) | ch;
        return raw.receive(length);
    };
    const auto ask=[&](unsigned listen, const std::string& line) {
        RawSocket raw(listen);
        send_frame(raw, line);
        return read_frame(raw);
    };
    // The data node demands MLS1 first; every fresh probe leg presents the
    // identity before its record frame, like the crossServer sender itself.
    const auto ask_data=[&](const std::string& line) {
        RawSocket raw(data_port);
        // The default listen "addr:port" identity carries a colon, which is
        // not a record identifier: the MLS1 frame accepts any single word, so
        // the colon-bearing service name must work end to end (regression for
        // the over-strict --service-name validation).
        send_frame(raw, "MLS1 crossServer xs:1 s3cret");
        equal(read_frame(raw), "MLS1 OK");
        send_frame(raw, line);
        return read_frame(raw);
    };
    // The crossServer leg joins, then drives the return sender from Lua.
    {
        RawSocket raw(cross);
        send_frame(raw, "MLC1 1 home g r1 ch hero gen 5 ret home g r1");
        equal(read_frame(raw), "MLC1 OK 1");
        send_frame(raw, "seed");
        equal(read_frame(raw), "SEEDED");
        send_frame(raw, "send");
        equal(read_frame(raw), "ENQUEUED");
    }
    // A negative generation and a zero sequence are refused by the Lua
    // boundary before they wrap to UINT64_MAX and burn the retry queue. The
    // join declares epoch 1 (the node's route epoch), and the second token of
    // an MLC1 join is the epoch, not a request id.
    {
        RawSocket raw(cross);
        send_frame(raw, "MLC1 1 home g r1 ch hero gen 5 ret home g r1");
        equal(read_frame(raw), "MLC1 OK 1");
        // The pre-load pool refuses unseeded participants before holding
        // anything, so an unseedable change is reported to the caller instead
        // of stranding forever. The non-positive invariants are covered at the
        // ordered queue boundary (cross_return_queue_bounded).
        send_frame(raw, "unseed");
        const auto unseed_reply = read_frame(raw);
        require(unseed_reply.find("UNSEED ") == 0 && unseed_reply.find("ACCEPTED") == std::string::npos,
                "unseeded participant was accepted: "+unseed_reply);
        send_frame(raw, "neg");
        const auto neg_reply = read_frame(raw);
        require(neg_reply.find("NEG ") == 0 && neg_reply.find("ACCEPTED") == std::string::npos,
                "negative generation was accepted: "+neg_reply);
        send_frame(raw, "zero");
        const auto zero_reply = read_frame(raw);
        require(zero_reply.find("ZERO ") == 0 && zero_reply.find("ACCEPTED") == std::string::npos,
                "zero sequence was accepted: "+zero_reply);
        send_frame(raw, "dup");
        const auto dup_reply = read_frame(raw);
        // Sequence 999 is not contiguous with the seed cursor, so the first
        // enqueue holds it in the queue and the second is refused; a first
        // enqueue failure is reported as SEEDFAIL, not a duplicate verdict.
        require(dup_reply.find("DUP ") == 0 && dup_reply.find("ACCEPTED") == std::string::npos,
                "duplicate sequence was accepted");
    }
    // The sender performs LOAD, submits in sequence and home data ACKs durably.
    eventually([&] {
        return ask_data("MLD1 LOAD home hero") == "MLD1 STATE gen 5 seq 1 hp=10";
    }, 5s, "the return sender never drained the durable change");
    equal(ask_data("MLD1 LOAD home hero"), "MLD1 STATE gen 5 seq 1 hp=10");
    // The first ACK durable must be judgeable for a participant the home had
    // never heard of (an EMPTY load answers the seed): the sender consumes it
    // instead of resubmitting sequence one forever, so the next change drains.
    {
        RawSocket raw(cross);
        send_frame(raw, "MLC1 1 home g r1 ch hero gen 5 ret home g r1");
        equal(read_frame(raw), "MLC1 OK 1");
        send_frame(raw, "send2");
        equal(read_frame(raw), "ENQUEUED2");
    }
    eventually([&] {
        return ask_data("MLD1 LOAD home hero") == "MLD1 STATE gen 5 seq 2 hp=20";
    }, 5s, "the first durable ACK was never accepted for an EMPTY-seeded participant");
    std::cout << "cross.return.sender: crossServer ordered queue drains only on "
        "ACK durable against a tokened data endpoint over MLS1 [PASS]\n";
}
void cross_home_fence() {
    // MLD1 FENCE is the data endpoint's explicit migration fence: it pins the
    // final cursor for a generation and, once pinned, makes that generation a
    // tombstone (old-generation advances are stale, newer ones resume).
    RecordStore store;
    std::string_view result;
    equal(store.handle("MLD1 SUBMIT home hero gen 1 seq 1 v=1", result), "MLD1 RECEIVED gen 1 seq 1");
    equal(store.handle("MLD1 SUBMIT home hero gen 1 seq 2 v=2", result), "MLD1 RECEIVED gen 1 seq 2");
    // The wire verb pins the final cursor at the committed sequence.
    equal(store.handle("MLD1 FENCE home hero gen 1", result), "MLD1 FENCED gen 1 seq 2");
    require(result == "fenced", "MLD1 FENCE result label was not fenced");
    // Re-fencing the same generation is idempotent; a different one is stale.
    equal(store.handle("MLD1 FENCE home hero gen 1", result), "MLD1 FENCED gen 1 seq 2");
    equal(store.handle("MLD1 FENCE home hero gen 2", result), "MLD1 ERR STALE gen 1");
    // Fencing a generation the store does not hold is refused before it can
    // pin the wrong cursor; the current generation stays fencable.
    RecordStore fresh;
    equal(fresh.handle("MLD1 SUBMIT home hero gen 1 seq 1 v=1", result), "MLD1 RECEIVED gen 1 seq 1");
    equal(fresh.handle("MLD1 SUBMIT home hero gen 1 seq 2 v=2", result), "MLD1 RECEIVED gen 1 seq 2");
    equal(fresh.handle("MLD1 FENCE home hero gen 2", result), "MLD1 ERR STALE gen 1");
    require(result == "stale", "fence on the wrong generation was not stale");
    equal(fresh.handle("MLD1 FENCE home hero gen 1", result), "MLD1 FENCED gen 1 seq 2");
    equal(fresh.handle("MLD1 SUBMIT home hero gen 2 seq 3 v=3", result), "MLD1 RECEIVED gen 2 seq 3");
    // The old generation is a tombstone: only the frozen cursor resends.
    equal(store.handle("MLD1 SUBMIT home hero gen 1 seq 2 v=2", result), "MLD1 RECEIVED gen 1 seq 2");
    equal(store.handle("MLD1 SUBMIT home hero gen 1 seq 3 late", result), "MLD1 ERR STALE gen 1");
    // A strictly newer generation resumes from the frozen cursor.
    equal(store.handle("MLD1 SUBMIT home hero gen 2 seq 3 v=3", result), "MLD1 RECEIVED gen 2 seq 3");
    // A key with no committed cursor cannot be fenced.
    equal(store.handle("MLD1 FENCE away hero gen 1", result), "MLD1 ERR NOFENCE");
    require(result == "nofence", "MLD1 FENCE without a record was not NOFENCE");
    // Malformed fence frames are invalid.
    equal(store.handle("MLD1 FENCE home hero", result), "MLD1 ERR INVALID");
    equal(store.handle("MLD1 FENCE home hero gen", result), "MLD1 ERR INVALID");
    std::cout << "record.home.fence: MLD1 FENCE pins the final cursor, is idempotent, "
        "tombstones the old generation and refuses keys without a cursor [PASS]\n";
}
void cross_home_fence_journal() {
    // A fence issued through the MLD1 wire on a store-backed data node is
    // durable: it survives a restart and keeps the old generation tombstoned.
    Context c;
    write(c.path("echo.lua"), echo);
    const auto first = port(), second = port();
    const auto journal = c.path("home-fence.sqlite");
    auto& initial = c.node("data", first, {"--script", c.path("echo.lua").string(),
        "--journal", journal.string()});
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
    const auto ask = [&](unsigned listen, const std::string& line) {
        RawSocket raw(listen);
        send_frame(raw, line);
        return read_frame(raw);
    };
    equal(ask(first, "MLD1 SUBMIT home hero gen 1 seq 1 v=1"),
        "MLD1 ACK durable home hero gen 1 seq 1");
    equal(ask(first, "MLD1 FENCE home hero gen 1"), "MLD1 FENCED gen 1 seq 1");
    initial.signal(SIGTERM);
    require(initial.wait() == 0, "home fence node did not stop cleanly");
    // A restart restores the durable pin; the old generation stays tombstoned
    // and the newer generation resumes from the frozen cursor.
    c.node("data", second, {"--script", c.path("echo.lua").string(),
        "--journal", journal.string()});
    equal(ask(second, "MLD1 LOAD home hero"), "MLD1 STATE gen 1 seq 1 v=1");
    equal(ask(second, "MLD1 FENCE home hero gen 1"), "MLD1 FENCED gen 1 seq 1");
    equal(ask(second, "MLD1 SUBMIT home hero gen 1 seq 2 late"), "MLD1 ERR STALE gen 1");
    equal(ask(second, "MLD1 SUBMIT home hero gen 2 seq 2 v=2"),
        "MLD1 ACK durable home hero gen 2 seq 2");
    std::cout << "record.home.fence.journal: a wire fence is durable, survives restart "
        "and tombstones the old generation [PASS]\n";
}
void cross_return_queue() {
    const DurableAck ack{"realm-a", "same-id", 3, 5};
    const auto wire = encode_durable_ack(ack);
    equal(wire, "MLD1 ACK durable realm-a same-id gen 3 seq 5");
    const auto decoded = parse_durable_ack(wire);
    require(decoded.realm == ack.realm && decoded.character == ack.character && decoded.generation == 3 && decoded.sequence == 5,
        "durable ack roundtrip changed fence/key/cursor");
    for (const auto& bad : {"MLD1 ACK realm-a same-id gen 3 seq 5", "MLD1 ACK durable realm-a same-id gen 0 seq 5",
                            "MLD1 ACK durable realm-a same-id gen 3 seq 0", "MLD1 ACK durable realm-a same-id gen 3 seq 5 extra"})
        must_fail([&] { (void)parse_durable_ack(bad); });
    WriterLease source, destination;
    require(source.start(8), "lease initial grant failed");
    require(source.write_allowed(8) && !source.write_allowed(9), "active lease fence failed");
    require(source.begin_request(), "active in-flight request not admitted");
    require(source.request(9) && !source.timer_running(), "transfer did not stop timers/drain in-flight work");
    require(!source.request(10), "duplicate handoff request accepted");
    require(!source.relinquish() && source.finish_request(), "handoff did not wait for in-flight request completion");
    require(source.relinquish() && !source.write_allowed(8), "source was not fenced at tombstone");
    require(!destination.accept(9), "destination accepted before transfer grant");
    require(destination.start(1) && destination.request(9) && destination.relinquish() && destination.accept(9), "destination lease activation failed");
    require(!source.write_allowed(8) && destination.write_allowed(9), "late source write was not rejected after handoff");
    require(!destination.accept(9), "duplicate lease grant accepted");
    {
        // A retired member cannot finish its own in-flight handler, so the
        // coordinator force-completes it instead of waiting forever.
        WriterLease retired;
        require(retired.start(8) && retired.begin_request() && retired.request(9), "retired lease fixture failed");
        require(retired.drain_requests(), "a retired member could not force-drain");
        require(retired.relinquish() && !retired.write_allowed(8), "a force-drained handoff was not fenced");
        require(!retired.drain_requests(), "drain ran after the tombstone");
    }
    ReturnSendQueue queue(2);
    queue.seed("realm-a", "same-id", 4);
    queue.seed("realm-b", "same-id", 0);
    queue.enqueue({"realm-a", "same-id", 3, 6, "six"});
    queue.enqueue({"realm-b", "same-id", 1, 1, "other realm"});
    require(!queue.next("realm-a", "same-id"), "gap was not held for retry/fill");
    queue.enqueue({"realm-a", "same-id", 3, 5, "five"});
    must_fail([&] { queue.enqueue({"realm-a", "same-id", 3, 5, "duplicate"}); });
    auto first = queue.next("realm-a", "same-id");
    require(first && first->sequence == 5, "sender did not release filled gap");
    require(!queue.next("realm-a", "same-id"), "same participant had concurrent sends");
    require(queue.retry("realm-a", "same-id"), "first retry was not allowed");
    first = queue.next("realm-a", "same-id");
    require(first && first->sequence == 5, "retry changed sequence order");
    require(!queue.retry("realm-a", "same-id"), "retry limit was not enforced");
    require(!queue.next("realm-a", "same-id"), "retry exhaustion resent an item");
    auto other = queue.next("realm-b", "same-id");
    require(other && other->sequence == 1, "realm key interfered with same character id");
    ReturnSendQueue durable_queue(2);
    durable_queue.seed("realm-a", "same-id", 4);
    durable_queue.enqueue({"realm-a", "same-id", 3, 5, "five"});
    durable_queue.owner_generation("realm-a", "same-id", 3);
    require(durable_queue.next("realm-a", "same-id").has_value(), "durable test send missing");
    require(durable_queue.received("realm-a", "same-id", 5), "volatile receipt did not release transport slot");
    require(!durable_queue.ack_durable(DurableAck{"realm-a", "same-id", 2, 5}), "stale generation durable ack drained queue");
    require(durable_queue.ack_durable(ack), "durable acknowledgement failed to drain matching change");
}
void cross_return_queue_bounded() {
    // The bounded queue refuses enqueue at capacity instead of growing
    // without bound: the retry budget is per-item and the total occupancy is
    // capped, matching the "bounded queue" contract.
    ReturnSendQueue bounded(3, 2);
    bounded.seed("realm", "hero", 0);
    must_fail([&] { bounded.enqueue({"realm", "hero", 0, 1, "zero generation"}); });
    must_fail([&] { bounded.enqueue({"realm", "hero", 1, 0, "zero sequence"}); });
    bounded.enqueue({"realm", "hero", 1, 1, "one"});
    bounded.enqueue({"realm", "hero", 1, 2, "two"});
    require(bounded.pending() == 2, "bounded queue occupancy miscounted");
    must_fail([&] { bounded.enqueue({"realm", "hero", 1, 3, "three"}); });
    // A durable ACK drains one slot, so the next enqueue succeeds. Only one
    // frame per participant may be in flight, so each send needs a RECEIVED
    // before the next can be taken (the queue's ordering invariant).
    require(bounded.next("realm", "hero").has_value(), "first change not sendable");
    require(bounded.received("realm", "hero", 1), "first change not acknowledged");
    require(bounded.next("realm", "hero").has_value(), "second change not sendable");
    require(bounded.received("realm", "hero", 2), "second change not acknowledged");
    require(bounded.ack_durable("realm", "hero", 1)
            && bounded.ack_durable("realm", "hero", 2), "bounded queue durable ack failed");
    require(bounded.pending() == 0, "bounded queue did not release a durable slot");
    bounded.enqueue({"realm", "hero", 1, 3, "three"});
    require(bounded.contains("realm", "hero", 3), "bounded queue lost the re-enqueued change");
    std::cout << "cross.return.queue.bounded: capacity refuses overflow and a "
        "durable ACK frees a slot [PASS]\n";
}
}
