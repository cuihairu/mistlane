// SPDX-License-Identifier: Apache-2.0
#include "test_support.hpp"
#include "test_common.hpp"
#include "mistlane/api.hpp"
#include "mistlane/api_store.hpp"
#include "mistlane/entry_ticket.hpp"
#include "mistlane/migration.hpp"

namespace mistlane::test {
namespace {
// Deterministic test-only key material: a fixed derivation pattern, never a
// real credential. Real secrets come from the embedding app's private config.
ApiSecret test_secret(unsigned char tag) {
    ApiSecret secret{};
    for (std::size_t i = 0; i < secret.size(); ++i) secret[i] = static_cast<unsigned char>(tag*16 + i);
    return secret;
}
ApiRequest base_request(ApiOperation operation = ApiOperation::Accept) {
    ApiRequest request;
    request.operation = operation;
    request.audience = "platform";
    request.principal = "ops-callback";
    request.key_id = "key-2026";
    request.timestamp = 1'000'000;
    request.nonce = "0123456789abcdef0123456789abcdef";
    request.character = {"realm1", "char42"};
    if (operation == ApiOperation::Accept) {
        request.idempotency_key = "grant-7";
        request.payload = "grant";
    }
    return request;
}
ApiVerifier make_verifier(ApiReplayGuard& guard) {
    ApiCredential credential;
    credential.principal = "ops-callback";
    credential.key_id = "key-2026";
    credential.secret = test_secret(1);
    credential.realms = {"realm1"};
    return ApiVerifier("platform", {credential}, guard);
}
}

void api_signature() {
    ApiReplayGuard guard;
    ApiVerifier verifier = make_verifier(guard);
    auto request = base_request();
    request.signature = api_sign(request, test_secret(1));
    require(verifier.verify(request, request.timestamp) == ApiAuth::Ok, "a correctly signed request was rejected");
    require(api_sign(request, test_secret(1)) == request.signature, "signing was not deterministic");
    require(api_canonical(request).compare(0, 4, "MLA1") == 0, "canonical bytes lost the MLA1 magic");
    // The operation tag is inside the canonical bytes, so ACCEPT and OWNER can
    // never share a signature even over otherwise equal fields.
    auto owner = base_request(ApiOperation::Owner);
    owner.nonce = "fedcba9876544321fedcba9876544321"; // distinct claim per delivery
    owner.signature = api_sign(owner, test_secret(1));
    require(verifier.verify(owner, owner.timestamp) == ApiAuth::Ok, "a signed owner query was rejected");
    require(owner.signature != request.signature, "the operation tag did not change the signature");
    std::cout << "api.signature hmac_sha256: valid request verified [PASS]\n";
    std::cout << "api.signature canonical: MLA1 framing, operation-bound [PASS]\n";
}

void api_tamper() {
    ApiReplayGuard guard;
    ApiVerifier verifier = make_verifier(guard);
    auto request = base_request();
    request.signature = api_sign(request, test_secret(1));
    auto forged = request;
    ++forged.payload.front(); // "grant" -> "hrant": one flipped body byte
    require(verifier.verify(forged, forged.timestamp) == ApiAuth::BadSignature, "a tampered payload was accepted");
    auto flipped = request;
    flipped.signature.front() ^= 0x80; // one flipped signature bit
    require(verifier.verify(flipped, flipped.timestamp) == ApiAuth::BadSignature, "a tampered signature was accepted");
    auto foreign = request; // valid HMAC shape, but under a different secret
    foreign.signature = api_sign(request, test_secret(2));
    require(verifier.verify(foreign, foreign.timestamp) == ApiAuth::BadSignature, "a foreign-secret signature was accepted");
    // Verification claims the nonce only after the HMAC passes, so attacker
    // garbage cannot evict an honest in-flight claim from the bounded guard.
    require(verifier.verify(request, request.timestamp) == ApiAuth::Ok, "a tampered request consumed the honest nonce");
    std::cout << "api.tamper payload/signature/secret: BadSignature [PASS]\n";
    std::cout << "api.tamper nonce_reserved: failures claim nothing [PASS]\n";
}

void api_rotation() {
    // Rotation overlaps: the old key stays valid through its bound inclusive
    // while the new one already verifies, so no coordinated cutover exists.
    ApiCredential old_row;
    old_row.principal = "ops-callback";
    old_row.key_id = "key-2025";
    old_row.secret = test_secret(1);
    old_row.realms = {"realm1"};
    old_row.valid_until = 2'000;
    ApiCredential new_row = old_row;
    new_row.key_id = "key-2026";
    new_row.valid_until = 0; // unbounded
    ApiReplayGuard guard;
    ApiVerifier verifier("platform", {old_row, new_row}, guard);
    const auto signed_request = [](const char* key_id, const char* nonce, std::uint64_t timestamp) {
        ApiRequest request = base_request();
        request.key_id = key_id;
        request.nonce = nonce;
        request.timestamp = timestamp;
        request.signature = api_sign(request, test_secret(1));
        return request;
    };
    const auto overlap_old = signed_request("key-2025", "0123456789abcdef0123456789abcdef", 1'999);
    const auto overlap_new = signed_request("key-2026", "fedcba9876544321fedcba9876544321", 1'999);
    require(verifier.verify(overlap_old, 1'999) == ApiAuth::Ok, "the old key was refused inside the overlap");
    require(verifier.verify(overlap_new, 1'999) == ApiAuth::Ok, "the new key was refused during rotation");
    const auto boundary = signed_request("key-2025", "11112222333344445555666677778888", 2'000);
    require(verifier.verify(boundary, 2'000) == ApiAuth::Ok, "the old key did not stay valid through its bound");
    const auto stale = signed_request("key-2025", "aaaabbbbccccddddeeeeffff00001111", 2'001);
    require(verifier.verify(stale, 2'001) == ApiAuth::UnknownCredential, "an expired key was not retired");
    // A failed expiry check claims nothing: the same nonce under the live key
    // still lands, because the claim key ignores key_id.
    const auto reuse = signed_request("key-2026", "aaaabbbbccccddddeeeeffff00001111", 2'001);
    require(verifier.verify(reuse, 2'001) == ApiAuth::Ok, "an expired check consumed the shared nonce slot");
    require(verifier.verify(overlap_new, 2'001) == ApiAuth::Replay, "a live nonce stopped replaying after rotation");

    // Revocation is removing the row: the same key that verified stops at once.
    ApiReplayGuard revoked_guard;
    ApiVerifier revocable("platform", {old_row, new_row}, revoked_guard);
    const auto live = signed_request("key-2025", "22223333444455556666777788889999", 1'999);
    require(revocable.verify(live, 1'999) == ApiAuth::Ok, "the row was not live before revocation");
    ApiVerifier cut("platform", {new_row}, revoked_guard);
    require(cut.verify(live, 1'999) == ApiAuth::UnknownCredential, "a revoked key still verified");
    std::cout << "api.rotation overlap: old key live through its bound inclusive [PASS]\n";
    std::cout << "api.rotation revoke: row removal stops the key at once [PASS]\n";
}

void api_replay() {
    ApiReplayGuard guard;
    ApiVerifier verifier = make_verifier(guard);
    auto request = base_request();
    request.signature = api_sign(request, test_secret(1));
    require(verifier.verify(request, request.timestamp) == ApiAuth::Ok, "first delivery was rejected");
    require(verifier.verify(request, request.timestamp) == ApiAuth::Replay, "a byte-identical replay was accepted");
    // A fresh timestamp and signature with a reused nonce is still a replay:
    // the claim key is (audience, principal, nonce), inside the time window.
    auto shifted = request;
    shifted.timestamp += 1;
    shifted.signature = api_sign(shifted, test_secret(1));
    require(verifier.verify(shifted, shifted.timestamp) == ApiAuth::Replay, "a reused nonce inside the window was accepted");
    // Outside the absolute window the request is refused before replay matters.
    auto stale = base_request();
    stale.nonce = "abcdef0123456789abcdef0123456789";
    stale.signature = api_sign(stale, test_secret(1));
    require(verifier.verify(stale, stale.timestamp + api_window_seconds + 1) == ApiAuth::Window,
            "a stale request was not refused by its window");
    // Wall-clock rollback fails closed instead of re-opening the window.
    require(verifier.verify(stale, stale.timestamp) == ApiAuth::Clock, "clock rollback did not fail closed");
    std::cout << "api.replay identical_and_nonce_reuse: Replay [PASS]\n";
    std::cout << "api.replay window_and_clock: Window/Clock fail closed [PASS]\n";
}

void api_acceptance() {
    // Store level: create-or-read over the full key, body-verified.
    ApiMemoryAcceptanceStore store;
    const ApiAcceptanceKey key{"platform", "ops-callback", {"realm1", "char42"}, "grant-7"};
    const ApiCommand command{key, "grant"};
    const auto first = store.create(command);
    require(first.result == ApiCreate::Inserted && first.receipt && first.receipt->command == command,
            "first acceptance was not Inserted with its receipt");
    const auto second = store.create(command);
    require(second.result == ApiCreate::Existing && second.receipt && second.receipt->command == command,
            "duplicate acceptance did not return the same command");
    require(*second.receipt == *first.receipt && store.size() == 1, "duplicate acceptance created a second row");
    const auto conflict = store.create(ApiCommand{key, "grant-v2"});
    require(conflict.result == ApiCreate::Conflict && !conflict.receipt && store.size() == 1,
            "the same key with a changed body was not refused");
    const auto fresh = store.create(ApiCommand{{"platform", "ops-callback", {"realm1", "char42"}, "grant-8"}, "grant"});
    require(fresh.result == ApiCreate::Inserted && store.size() == 2, "a new idempotency key did not insert");

    // Surface level: a signed ACCEPT is idempotent across retries even when
    // the retry is a different wire request (fresh nonce/signature).
    ApiReplayGuard guard;
    ApiVerifier verifier = make_verifier(guard);
    ApiMemoryAcceptanceStore surface_store;
    const ApiSurface surface(verifier, surface_store, {});
    auto request = base_request();
    request.signature = api_sign(request, test_secret(1));
    const auto accepted = surface.handle(request, request.timestamp);
    require(accepted.auth == ApiAuth::Ok && accepted.acceptance
            && accepted.acceptance->result == ApiCreate::Inserted, "signed acceptance was not Inserted");
    auto retry = base_request();
    retry.nonce = "fedcba9876544321fedcba9876544321";
    retry.signature = api_sign(retry, test_secret(1));
    const auto again = surface.handle(retry, retry.timestamp);
    require(again.auth == ApiAuth::Ok && again.acceptance && again.acceptance->result == ApiCreate::Existing
            && again.acceptance->receipt == accepted.acceptance->receipt,
            "a retry was not deduplicated to the original receipt");
    require(surface_store.size() == 1, "a retry created a second pending command");
    std::cout << "api.acceptance store: Inserted/Existing/Conflict exactly once [PASS]\n";
    std::cout << "api.acceptance surface: retry dedup to the same receipt [PASS]\n";
}

void api_sqlite_acceptance() {
    // The durable backend mirrors the memory store's create-or-read semantics
    // but commits each row to SQLite before reporting it, so a restart sees
    // the same pending command (lost-reply replay) and a changed body stays
    // Conflict. Storage failures must propagate, never be read as Conflict.
    Temp temp;
    const auto path = temp.path / "api-acceptance.sqlite";
    {
        ApiSqliteAcceptanceStore store(path.string());
        const ApiAcceptanceKey key{"platform", "ops-callback", {"realm1", "char42"}, "grant-7"};
        const ApiCommand command{key, "grant"};
        const auto first = store.create(command);
        require(first.result == ApiCreate::Inserted && first.durability == ApiDurability::Durable
                && first.receipt && first.receipt->command == command,
                "a durable acceptance was not Inserted with a Durable receipt");
        const auto second = store.create(command);
        require(second.result == ApiCreate::Existing && second.durability == ApiDurability::Durable
                && second.receipt && second.receipt->command == command
                && *second.receipt == *first.receipt && store.size() == 1,
                "a durable retry did not deduplicate to the original receipt");
        const auto conflict = store.create(ApiCommand{key, "grant-v2"});
        require(conflict.result == ApiCreate::Conflict && conflict.durability == ApiDurability::Durable
                && !conflict.receipt && store.size() == 1,
                "a durable key with a changed body was not refused");
        const auto fresh = store.create(ApiCommand{
            {"platform", "ops-callback", {"realm1", "char42"}, "grant-8"}, "grant"});
        require(fresh.result == ApiCreate::Inserted && store.size() == 2,
                "a new durable idempotency key did not insert");
        // Invalid identifiers and oversize payloads are refused before storage.
        require(store.create(ApiCommand{{"bad key", "ops-callback", {"realm1", "char42"}, "k"}, "p"}).result
                == ApiCreate::Invalid, "an invalid audience was not refused");
        require(store.create(ApiCommand{{"platform", "ops-callback", {"realm1", "char42"}, "k"},
                std::string(1024*1024+1, 'x')}).result == ApiCreate::Invalid,
                "an oversize payload was not refused");
    }
    // A fresh store on the same file restores the rows: a lost reply after a
    // restart deduplicates instead of inserting a second pending command.
    {
        ApiSqliteAcceptanceStore store(path.string());
        require(store.size() == 2, "restart lost a durable acceptance row");
        const ApiCommand command{{"platform", "ops-callback", {"realm1", "char42"}, "grant-7"}, "grant"};
        const auto again = store.create(command);
        require(again.result == ApiCreate::Existing && again.durability == ApiDurability::Durable
                && again.receipt && again.receipt->command == command,
                "a lost-reply replay after restart was not deduplicated");
        const auto conflict = store.create(ApiCommand{command.key, "grant-v2"});
        require(conflict.result == ApiCreate::Conflict && store.size() == 2,
                "a changed body after restart was not refused");
    }
    // A bounded store refuses a fresh key once full, without growing.
    {
        ApiSqliteAcceptanceStore store((temp.path / "api-acceptance-full.sqlite").string(), 1);
        require(store.create(ApiCommand{{"platform", "ops-callback", {"realm1", "char42"}, "k1"}, "p"}).result
                == ApiCreate::Inserted, "the first bounded row did not insert");
        require(store.create(ApiCommand{{"platform", "ops-callback", {"realm1", "char42"}, "k2"}, "p"}).result
                == ApiCreate::Full, "a full bounded store did not refuse a fresh key");
        // A duplicate of the stored key still answers Existing, not Full.
        require(store.create(ApiCommand{{"platform", "ops-callback", {"realm1", "char42"}, "k1"}, "p"}).result
                == ApiCreate::Existing, "a full store refused its own stored key");
    }
    // Storage failures surface as exceptions: a path inside a missing
    // directory cannot open, and a non-database file fails at schema setup.
    {
        bool opened = false;
        try {
            ApiSqliteAcceptanceStore store((temp.path / "missing-dir" / "x.sqlite").string());
            opened = true;
        } catch (const std::exception& e) {
            require(std::string(e.what()).starts_with("cannot open api acceptance store"),
                "unexpected open failure: "+std::string(e.what()));
        }
        require(!opened, "a store inside a missing directory did not fail");
        write(temp.path / "garbage.sqlite", "this is not a sqlite database at all");
        bool schema_ok = false;
        try {
            ApiSqliteAcceptanceStore store((temp.path / "garbage.sqlite").string());
            schema_ok = true;
        } catch (const std::exception& e) {
            require(std::string(e.what()).starts_with("api acceptance store schema failed"),
                "unexpected schema failure: "+std::string(e.what()));
        }
        require(!schema_ok, "a garbage store file did not fail");
    }
    std::cout << "api.acceptance.sqlite durable: Inserted/Existing/Conflict exactly once, "
        "restart dedup and Full bound [PASS]\n";
}

void api_wire() {
    const auto field = [](std::string_view value) {
        std::string bytes(4, '\0');
        const auto size = static_cast<std::uint32_t>(value.size());
        bytes[0] = static_cast<char>(size >> 24);
        bytes[1] = static_cast<char>(size >> 16);
        bytes[2] = static_cast<char>(size >> 8);
        bytes[3] = static_cast<char>(size);
        bytes.append(value);
        return bytes;
    };
    // Round-trip: the wire body parses back to the exact request, both operations.
    for (const auto operation : {ApiOperation::Accept, ApiOperation::Owner}) {
        auto request = base_request(operation);
        if (operation == ApiOperation::Owner) request.nonce = "fedcba9876544321fedcba9876544321";
        request.signature = api_sign(request, test_secret(1));
        ApiRequest parsed;
        require(parse_api_body(api_canonical(request)
                + std::string(request.signature.begin(), request.signature.end()), parsed),
                "a well-formed MLA1 body was refused");
        require(parsed == request, "the parsed body did not round-trip the request");
    }
    auto request = base_request();
    request.signature = api_sign(request, test_secret(1));
    const std::string signature(reinterpret_cast<const char*>(request.signature.data()), request.signature.size());
    // Nine framed fields; the payload is appended by body_of so a case can
    // replace it with a lying length or with nothing at all.
    const auto nine = [&](std::string_view operation, std::string_view timestamp) {
        return "MLA1" + field(operation) + field(request.audience) + field(request.principal)
            + field(request.key_id) + field(timestamp) + field(request.nonce)
            + field(request.character.first) + field(request.character.second)
            + field(request.idempotency_key);
    };
    const auto body_of = [&](std::string_view operation, std::string_view timestamp, std::string_view payload) {
        return nine(operation, timestamp) + field(payload);
    };
    const auto lie = [](std::uint32_t size) {
        std::string header(4, '\0');
        header[0] = static_cast<char>(size >> 24);
        header[1] = static_cast<char>(size >> 16);
        header[2] = static_cast<char>(size >> 8);
        header[3] = static_cast<char>(size);
        return header;
    };
    const std::string good = body_of("ACCEPT", std::to_string(request.timestamp), request.payload);
    require(good + signature == api_canonical(request) + signature,
            "hand-framed bytes diverged from api_canonical");
    // Structural refusals: anything but ten framed fields then 32 bytes fails.
    ApiRequest sink;
    const auto ts = std::to_string(request.timestamp);
    const std::vector<std::pair<std::string, const char*>> malformed = {
        {{}, "empty"},
        {"MLA1", "magic only"},
        {"MLA2" + api_canonical(request).substr(4) + signature, "wrong magic"},
        {nine("ACCEPT", ts) + signature, "nine fields"},
        {good + field("extra") + signature, "eleven fields"},
        {nine("ACCEPT", ts) + lie(100) + "grant" + signature, "length beyond body"},
        {good + signature + "x", "leftover bytes"},
        {good + signature.substr(0, 31), "short signature"},
        {body_of("POLL", ts, request.payload) + signature, "unknown operation"},
        {body_of("ACCEPT", "1e6", request.payload) + signature, "non-numeric timestamp"},
        {body_of("ACCEPT", "", request.payload) + signature, "empty timestamp"},
        {body_of("ACCEPT", "99999999999999999999999", request.payload) + signature, "overflowing timestamp"},
    };
    for (const auto& [body, name] : malformed)
        require(!parse_api_body(body, sink), std::string("malformed body accepted: ") + name);
    // Shape-valid framing with a request-invalid field (nonce charset) still
    // fails, because validation runs after the framing passes.
    std::string bad_nonce = good;
    require(!parse_api_body(bad_nonce.replace(bad_nonce.find(request.nonce), request.nonce.size(),
        std::string(32, 'z')) + signature, sink), "a non-hex nonce was accepted");

    // Reply framing: five words, receipt payload verbatim, empty tails.
    const auto reply_bytes = [&](const std::vector<std::string_view>& values) {
        std::string bytes = "MLA1";
        for (const auto& value : values) bytes += field(value);
        return bytes;
    };
    ApiReply accepted;
    accepted.auth = ApiAuth::Ok;
    accepted.acceptance = ApiAcceptance{ApiCreate::Inserted, ApiDurability::Volatile,
        ApiReceipt{ApiCommand{{"platform", "ops-callback", {"realm1", "char42"}, "grant-7"}, "grant"}}};
    require(api_reply_body(accepted) == reply_bytes({"OK", "INSERTED", "VOLATILE", "grant", ""}),
            "the inserted reply framing changed");
    auto existing = accepted;
    existing.acceptance->result = ApiCreate::Existing;
    existing.acceptance->durability = ApiDurability::Durable;
    require(api_reply_body(existing) == reply_bytes({"OK", "EXISTING", "DURABLE", "grant", ""}),
            "the idempotent-reply framing changed");
    auto conflicted = accepted;
    conflicted.acceptance->result = ApiCreate::Conflict;
    conflicted.acceptance->receipt.reset();
    require(api_reply_body(conflicted) == reply_bytes({"OK", "CONFLICT", "VOLATILE", "", ""}),
            "a conflict reply lost its empty receipt tail");
    ApiReply found;
    found.auth = ApiAuth::Ok;
    found.ownership = ApiOwnershipReply{ApiOwnershipReply::Status::Found, std::nullopt};
    require(api_reply_body(found) == reply_bytes({"OK", "", "", "", "FOUND"}),
            "a found-ownership reply framing changed");
    ApiReply missing = found;
    missing.ownership->status = ApiOwnershipReply::Status::NotFound;
    require(api_reply_body(missing) == reply_bytes({"OK", "", "", "", "NOT_FOUND"}),
            "a not-found ownership reply framing changed");
    ApiReply replay;
    replay.auth = ApiAuth::Replay;
    require(api_reply_body(replay) == reply_bytes({"REPLAY", "", "", "", ""}),
            "an auth-only reply left a non-empty tail");

    // HTTP status mapping: auth verdicts first, then the operation outcome.
    require(api_http_status(accepted) == 200 && api_http_status(existing) == 200
            && api_http_status(found) == 200, "successful outcomes lost 200");
    require(api_http_status(missing) == 404, "an unknown character did not map to 404");
    require(api_http_status(conflicted) == 409, "a conflicting body did not map to 409");
    auto full = accepted;
    full.acceptance->result = ApiCreate::Full;
    full.acceptance->receipt.reset();
    require(api_http_status(full) == 503, "a full store did not map to 503");
    for (const auto& [auth, status] : {std::pair{ApiAuth::Invalid, 400u},
            {ApiAuth::UnknownCredential, 401u}, {ApiAuth::BadSignature, 401u},
            {ApiAuth::Window, 401u}, {ApiAuth::Replay, 401u}, {ApiAuth::Forbidden, 403u},
            {ApiAuth::Clock, 503u}, {ApiAuth::Full, 503u}}) {
        ApiReply reply;
        reply.auth = auth;
        require(api_http_status(reply) == status, "an auth verdict mapped to the wrong HTTP status");
    }
    std::cout << "api.wire body: ten framed fields plus signature, byte-exact [PASS]\n";
    std::cout << "api.wire reply and status: five framed words, verdict mapping [PASS]\n";
}

void api_ownership() {
    // Home facts live on the home transaction aggregate, never the route.
    HomeGenerationAuthority home;
    const MigrationKey key{"realm1", "char42"};
    require(home.seed(key, 1), "home generation seed failed");
    const MigrationOwner source{{MigrationScope::Kind::Realm, "realm1", {}}, "gameServer", "game-1", "scene-1"};
    const MigrationOwner destination{{MigrationScope::Kind::Realm, "realm1", {}}, "gameServer", "game-2", "scene-9"};
    const MigrationRoute route{source, 1};
    MigrationTransaction txn(home, key, route);
    const auto before = txn.lookup_ownership(key);
    require(before && before->state == HomeOwnershipState::Active && before->owner == source
            && before->generation == 1, "baseline ownership was not Active with the source");
    require(home.issued(key) == 1, "an ownership query reserved a generation");
    require(!txn.lookup_ownership({"realm1", "other"}), "an unknown character returned ownership");
    require(txn.prepare({key, "mig-1", route, destination}) == MigrationResult::Applied, "prepare failed");
    const auto draining = txn.lookup_ownership(key);
    require(draining && draining->state == HomeOwnershipState::Draining && draining->owner == source
            && draining->generation == 1, "drain phase ownership was not Draining with the source");
    const MigrationToken token{key, "mig-1", 2};
    require(txn.apply(token, MigrationEvent::SourceDrained) == MigrationResult::Applied, "drain failed");
    require(txn.apply(token, MigrationEvent::HomeFence) == MigrationResult::Applied, "fence failed");
    const auto fenced = txn.lookup_ownership(key);
    require(fenced && fenced->state == HomeOwnershipState::Fenced && !fenced->owner && fenced->generation == 2,
            "the fence gap was not Fenced without an owner");
    require(txn.apply(token, MigrationEvent::DestinationReady) == MigrationResult::Applied, "destination ACK failed");
    const auto taken = txn.lookup_ownership(key);
    require(taken && taken->state == HomeOwnershipState::Active && taken->owner == destination
            && taken->generation == 2, "the destination ACK did not become the Active owner");
    require(txn.apply(token, MigrationEvent::Commit) == MigrationResult::Applied, "commit failed");
    const auto committed = txn.lookup_ownership(key);
    require(committed && committed->state == HomeOwnershipState::Active && committed->owner == destination
            && committed->generation == 2, "committed ownership changed after publication");

    // Deterministic read-only reply through the composed surface.
    ApiReplayGuard guard;
    ApiVerifier verifier = make_verifier(guard);
    ApiMemoryAcceptanceStore store;
    const ApiSurface surface(verifier, store,
        [&txn](const MigrationKey& k) { return txn.lookup_ownership(k); });
    auto request = base_request(ApiOperation::Owner);
    request.signature = api_sign(request, test_secret(1));
    const auto found = surface.handle(request, request.timestamp);
    require(found.auth == ApiAuth::Ok && found.ownership
            && found.ownership->status == ApiOwnershipReply::Status::Found
            && found.ownership->current && *found.ownership->current == *committed,
            "the ownership reply did not match the home facts");
    auto unknown = base_request(ApiOperation::Owner);
    unknown.character = {"realm1", "other"};
    unknown.nonce = "fedcba9876544321fedcba9876544321";
    unknown.signature = api_sign(unknown, test_secret(1));
    const auto missing = surface.handle(unknown, request.timestamp);
    require(missing.auth == ApiAuth::Ok && missing.ownership
            && missing.ownership->status == ApiOwnershipReply::Status::NotFound && !missing.ownership->current,
            "an unknown character did not answer NotFound");
    std::cout << "api.ownership phases: Active->Draining->Fenced->Active [PASS]\n";
    std::cout << "api.ownership surface: deterministic Found/NotFound [PASS]\n";
}

void login_ticket() {
    const RecordStore::RecordKey hero{"home", "hero"};
    const auto field = [](std::string& bytes, const std::string& value) {
        const auto size = static_cast<std::uint32_t>(value.size());
        for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<char>((size >> shift) & 255));
        bytes += value;
    };
    const auto base = [] {
        EntryTicket ticket;
        ticket.character = {"home", "hero"};
        ticket.instance = "gs-a";
        ticket.generation = 9;
        ticket.expires = 1'000;
        ticket.nonce = "0123456789abcdef0123456789abcdef";
        return ticket;
    };

    // Issue and verify: the roundtrip admits exactly the issued arrival, and
    // the canonical bytes bind every field, so each one moves the signature.
    TicketClaims roundtrip;
    const TicketVerifier verifier(std::map<std::string, ApiSecret>{{"home", test_secret(3)}, {"foreign", test_secret(4)}},
        "gs-a", roundtrip);
    auto ticket = base();
    ticket.signature = entry_ticket_sign(ticket, test_secret(3));
    require(valid_entry_ticket(ticket), "a well-formed ticket was marked invalid");
    require(verifier.verify(ticket, hero, 9, 999) == TicketVerdict::Ok, "a fresh ticket was refused");
    require(entry_ticket_sign(ticket, test_secret(3)) == ticket.signature, "ticket signing was not deterministic");
    std::string hand = "MLT1";
    for (const auto& value : {std::string("home"), std::string("hero"), std::string("gs-a"),
            std::string("9"), std::string("1000"), ticket.nonce}) field(hand, value);
    require(entry_ticket_canonical(ticket) == hand, "ticket canonical bytes diverged from the framing");
    const auto diverges = [&](EntryTicket changed) {
        changed.signature = {};
        return entry_ticket_canonical(changed) != entry_ticket_canonical(ticket);
    };
    auto changed = base();
    changed.character.first = "foreign";
    require(diverges(changed), "the realm did not change the canonical bytes");
    changed = base();
    changed.character.second = "sidekick";
    require(diverges(changed), "the character did not change the canonical bytes");
    changed = base();
    changed.instance = "gs-b";
    require(diverges(changed), "the instance did not change the canonical bytes");
    changed = base();
    changed.generation = 10;
    require(diverges(changed), "the generation did not change the canonical bytes");
    changed = base();
    changed.expires = 2'000;
    require(diverges(changed), "the expiry did not change the canonical bytes");
    changed = base();
    changed.nonce = "fedcba9876544321fedcba9876544321";
    require(diverges(changed), "the nonce did not change the canonical bytes");

    // Issuing refuses a bad shape outright; the verifier refuses bad inputs too.
    const auto throws = [](const EntryTicket& bad) {
        try {
            entry_ticket_canonical(bad);
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };
    auto broken = base();
    broken.instance.clear();
    require(throws(broken) && !valid_entry_ticket(broken), "an empty instance was accepted");
    broken = base();
    broken.generation = 0;
    require(throws(broken) && !valid_entry_ticket(broken), "a zero generation was accepted");
    broken = base();
    broken.nonce.pop_back();
    require(throws(broken) && !valid_entry_ticket(broken), "a short nonce was accepted");
    broken = base();
    broken.nonce = std::string(32, 'A');
    require(throws(broken) && !valid_entry_ticket(broken), "an uppercase nonce was accepted");
    const auto bad_verifier = [&](std::map<std::string, ApiSecret> secrets, const std::string& instance) {
        try {
            TicketClaims sink;
            TicketVerifier draft(std::move(secrets), instance, sink);
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };
    require(bad_verifier({}, "gs-a"), "a verifier without secrets was accepted");
    require(bad_verifier({{"home", test_secret(3)}}, "gs a!"), "a non-identifier instance was accepted");

    // The verdict ladder: a failed check claims nothing, so the honest retry
    // of the same nonce still lands Ok and only the second use is a Replay.
    TicketClaims ladder;
    const TicketVerifier host(std::map<std::string, ApiSecret>{{"home", test_secret(3)}, {"foreign", test_secret(4)}},
        "gs-a", ladder);
    auto claim = base();
    claim.signature = entry_ticket_sign(claim, test_secret(3));
    auto flipped = claim;
    flipped.signature.front() ^= 0x80; // one flipped signature bit
    require(host.verify(flipped, hero, 9, 999) == TicketVerdict::BadSignature, "a flipped signature was accepted");
    require(host.verify(claim, hero, 9, 999) == TicketVerdict::Ok, "a failed verify consumed the honest nonce");
    require(host.verify(claim, hero, 9, 999) == TicketVerdict::Replay, "a byte-identical replay was accepted");
    auto fresh = base();
    fresh.nonce = "fedcba9876544321fedcba9876544321";
    fresh.signature = entry_ticket_sign(fresh, test_secret(3));
    require(host.verify(fresh, hero, 9, 999) == TicketVerdict::Ok, "a new nonce was refused");
    const RecordStore::RecordKey other{"home", "sidekick"};
    require(host.verify(fresh, other, 9, 999) == TicketVerdict::WrongCharacter, "a foreign character was admitted");
    auto elsewhere = base();
    elsewhere.instance = "gs-b";
    elsewhere.signature = entry_ticket_sign(elsewhere, test_secret(3));
    require(host.verify(elsewhere, hero, 9, 999) == TicketVerdict::WrongInstance, "a wrong instance was admitted");
    require(host.verify(fresh, hero, 8, 999) == TicketVerdict::WrongGeneration, "a stale generation was admitted");
    auto guest = base();
    guest.character = {"foreign", "guest"};
    guest.nonce = "11112222333344445555666677778888";
    guest.signature = entry_ticket_sign(guest, test_secret(3)); // home's secret, not foreign's
    require(host.verify(guest, guest.character, 9, 999) == TicketVerdict::BadSignature,
            "a cross-realm secret was accepted");
    guest.signature = entry_ticket_sign(guest, test_secret(4));
    require(host.verify(guest, guest.character, 9, 999) == TicketVerdict::Ok,
            "a foreign realm under its own secret was refused");
    auto ghost = base();
    ghost.character = {"elsewhere", "ghost"};
    ghost.signature = entry_ticket_sign(ghost, test_secret(5));
    require(host.verify(ghost, ghost.character, 9, 999) == TicketVerdict::UnknownRealm,
            "an unknown realm was not refused before its signature mattered");

    // Time: valid through expiry inclusively, expired strictly after, clock
    // rollback fails closed without consuming anything.
    TicketClaims time_claims;
    const TicketVerifier timed(std::map<std::string, ApiSecret>{{"home", test_secret(3)}}, "gs-a", time_claims);
    auto early = base();
    early.signature = entry_ticket_sign(early, test_secret(3));
    require(timed.verify(early, hero, 9, 999) == TicketVerdict::Ok, "a ticket before expiry was refused");
    auto stale = base();
    stale.nonce = "abcdef0123456789abcdef0123456789";
    stale.signature = entry_ticket_sign(stale, test_secret(3));
    require(timed.verify(stale, hero, 9, 1'001) == TicketVerdict::Expired, "an expired ticket was accepted");
    require(timed.verify(stale, hero, 9, 1'001) == TicketVerdict::Expired, "expiry was not deterministic");
    auto edge = base();
    edge.nonce = "abcdef0123456789abcdef0123456789";
    edge.signature = entry_ticket_sign(edge, test_secret(3));
    require(timed.verify(edge, hero, 9, 1'000) == TicketVerdict::Ok,
            "the expiry verdict consumed the nonce it refused");
    auto rolled_back = base();
    rolled_back.nonce = "99998888777766665555444433332222";
    rolled_back.signature = entry_ticket_sign(rolled_back, test_secret(3));
    require(timed.verify(rolled_back, hero, 9, 999) == TicketVerdict::Clock, "time moved backwards and was not refused");
    TicketClaims full(1);
    const TicketVerifier tight(std::map<std::string, ApiSecret>{{"home", test_secret(3)}}, "gs-a", full);
    auto first = base();
    first.signature = entry_ticket_sign(first, test_secret(3));
    require(tight.verify(first, hero, 9, 999) == TicketVerdict::Ok, "the first claim did not fit");
    auto second = base();
    second.nonce = "fedcba9876544321fedcba9876544321";
    second.signature = entry_ticket_sign(second, test_secret(3));
    require(tight.verify(second, hero, 9, 999) == TicketVerdict::Full, "a full claim table admitted another nonce");

    // The login-side issuer mints a ticket with a fresh CSPRNG nonce and the
    // same instance it was constructed for; the host verifier admits exactly
    // that arrival. A bad shape or an unknown realm refuses to mint, and the
    // issuer itself rejects an empty secret map or a non-identifier instance.
    const TicketIssuer issuer(std::map<std::string, ApiSecret>{{"home", test_secret(3)},
        {"foreign", test_secret(4)}});
    const auto minted = issuer.issue(hero, "gs-a", 9, 1'000);
    require(minted && minted->size() > 0, "a valid ticket was not minted");
    const auto parsed = parse_entry_ticket(*minted);
    require(parsed && parsed->character == hero && parsed->instance == "gs-a"
            && parsed->generation == 9 && parsed->expires == 1'000
            && parsed->nonce.size() == 32, "the minted ticket did not round-trip");
    TicketClaims issued_claims;
    const TicketVerifier issued_host(std::map<std::string, ApiSecret>{{"home", test_secret(3)},
        {"foreign", test_secret(4)}}, "gs-a", issued_claims);
    require(issued_host.verify(*parsed, hero, 9, 999) == TicketVerdict::Ok,
            "the issuer's ticket was refused by the host");
    const auto minted_again = issuer.issue(hero, "gs-a", 9, 1'000);
    require(minted_again && *minted_again != *minted, "two mints reused a nonce");
    require(!issuer.issue({"elsewhere", "ghost"}, "gs-a", 9, 1'000), "an unknown realm minted a ticket");
    require(!issuer.issue({"home", "x y"}, "gs-a", 9, 1'000), "a non-identifier character minted a ticket");
    require(!issuer.issue(hero, "gs a!", 9, 1'000), "a non-identifier instance minted a ticket");
    require(!issuer.issue(hero, "gs-a", 0, 1'000), "a zero generation minted a ticket");
    require(!issuer.issue(hero, "gs-a", 9, 0), "a zero expiry minted a ticket");
    const auto bad_issuer = [](std::map<std::string, ApiSecret> secrets) {
        try {
            TicketIssuer draft(std::move(secrets));
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };
    require(bad_issuer({}), "an issuer without secrets was accepted");

    std::cout << "login.ticket issuer: mint, round-trip, fresh nonce, bad shapes [PASS]\n";
    std::cout << "login.ticket roundtrip: issue then verify admits the arrival [PASS]\n";
    std::cout << "login.ticket ladder: tamper/identity/expiry/replay/capacity all refused [PASS]\n";
}

void api_http() {
    Context c;
    write(c.path("api.lua"), http_lua);
    ApiSecret secret{};
    for (auto& byte : secret) byte = 0x37;
    std::string hex;
    for (std::size_t i = 0; i < secret.size(); ++i) hex += "37";
    const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    unsigned counter = 0;
    const auto nonce = [&counter] {
        char digits[33];
        std::snprintf(digits, sizeof digits, "%032x", ++counter);
        return std::string(digits);
    };
    const auto signed_body = [&](ApiRequest& request) {
        request.signature = api_sign(request, secret);
        return api_canonical(request) + std::string(request.signature.begin(), request.signature.end());
    };
    const auto accept_body = [&](const std::string& idempotency, const std::string& payload = "grant") {
        ApiRequest request;
        request.operation = ApiOperation::Accept;
        request.audience = "platform";
        request.principal = "ops-callback";
        request.key_id = "key-2026";
        request.timestamp = now;
        request.nonce = nonce();
        request.character = {"realm1", "char42"};
        request.idempotency_key = idempotency;
        request.payload = payload;
        return signed_body(request);
    };
    const auto owner_body = [&] {
        ApiRequest request;
        request.operation = ApiOperation::Owner;
        request.audience = "platform";
        request.principal = "ops-callback";
        request.key_id = "key-2026";
        request.timestamp = now;
        request.nonce = nonce();
        request.character = {"realm1", "char42"};
        return signed_body(request);
    };
    const auto reply_body = [](std::initializer_list<std::string_view> values) {
        std::string bytes = "MLA1";
        for (const auto& value : values) {
            const auto size = static_cast<std::uint32_t>(value.size());
            for (int shift : {24, 16, 8, 0}) bytes.push_back(static_cast<char>((size >> shift) & 255));
            bytes.append(value);
        }
        return bytes;
    };
    {
        ScopedEnv audience("MISTLANE_MLA1_AUDIENCE", "platform");
        ScopedEnv credentials("MISTLANE_MLA1_CREDENTIAL", "ops-callback:key-2026:"+hex+":realm1");
        const auto listen = port();
        c.node("apiServer", listen, {"--script", c.path("api.lua").string()});
        const auto base = "http://"+endpoint(listen);
        // A signed acceptance inserts once, then deduplicates on retry.
        auto sent = http(base+"/mla1", "POST", accept_body("grant-7"));
        require(sent.status == 200, "a signed acceptance was not 200");
        equal(sent.body, reply_body({"OK", "INSERTED", "VOLATILE", "grant", ""}));
        sent = http(base+"/mla1", "POST", accept_body("grant-7"));
        require(sent.status == 200, "a retried acceptance was not 200");
        equal(sent.body, reply_body({"OK", "EXISTING", "VOLATILE", "grant", ""}));
        // One idempotency key with a changed body conflicts, nothing stored.
        sent = http(base+"/mla1", "POST", accept_body("grant-7", "grant-v2"));
        require(sent.status == 409, "a changed body under one key was not 409");
        equal(sent.body, reply_body({"OK", "CONFLICT", "VOLATILE", "", ""}));
        // One flipped signature bit: 401 verdict word, never stored.
        auto tampered = accept_body("grant-8");
        tampered.back() = static_cast<char>(tampered.back() ^ 0x80);
        sent = http(base+"/mla1", "POST", tampered);
        require(sent.status == 401, "a tampered signature was not 401");
        equal(sent.body, reply_body({"BAD_SIGNATURE", "", "", "", ""}));
        // OWNER asks home facts; with no facts wired the answer is NOT_FOUND.
        sent = http(base+"/mla1", "POST", owner_body());
        require(sent.status == 404, "an unknown character was not 404");
        equal(sent.body, reply_body({"OK", "", "", "", "NOT_FOUND"}));
        // Structural garbage is a 400 verdict body, not a Lua trip.
        sent = http(base+"/mla1", "POST", "garbage");
        require(sent.status == 400, "a malformed body was not 400");
        equal(sent.body, reply_body({"INVALID", "", "", "", ""}));
        // Every other route (and GET /mla1) stays plain Lua routing.
        equal(http(base+"/nowhere", "POST", "ping").body, "missing");
        equal(http(base+"/mla1", "GET").body, "missing");
    }
    // Without credential variables /mla1 is ordinary Lua routing.
    {
        const auto listen = port();
        c.node("apiServer", listen, {"--script", c.path("api.lua").string()});
        equal(http("http://"+endpoint(listen)+"/mla1", "POST", accept_body("grant-9")).body, "missing");
    }
    // Half-configured or malformed credentials refuse startup.
    {
        ScopedEnv audience("MISTLANE_MLA1_AUDIENCE", "platform");
        auto& lonely = c.spawn({self().string(), "--role", "apiServer", "--listen", endpoint(port()),
            "--script", c.path("api.lua").string()});
        require(lonely.wait() != 0 && lonely.output().find("MISTLANE_MLA1") != std::string::npos,
                "an audience without credentials booted");
        ScopedEnv bad("MISTLANE_MLA1_CREDENTIAL", "ops-callback:key-2026:not-hex:realm1");
        auto& corrupt = c.spawn({self().string(), "--role", "apiServer", "--listen", endpoint(port()),
            "--script", c.path("api.lua").string()});
        require(corrupt.wait() != 0 && corrupt.output().find("MLA1 credential") != std::string::npos,
                "a malformed credential booted");
    }
    // A rotation bound on the row: expired retires the key, live verifies.
    {
        ScopedEnv audience("MISTLANE_MLA1_AUDIENCE", "platform");
        {
            ScopedEnv expired("MISTLANE_MLA1_CREDENTIAL",
                "ops-callback:key-2026:"+hex+":realm1:"+std::to_string(now-10));
            const auto listen = port();
            c.node("apiServer", listen, {"--script", c.path("api.lua").string()});
            const auto sent = http("http://"+endpoint(listen)+"/mla1", "POST", accept_body("grant-old"));
            require(sent.status == 401, "an expired rotation bound was not 401");
            equal(sent.body, reply_body({"UNKNOWN_CREDENTIAL", "", "", "", ""}));
        }
        {
            ScopedEnv bounded("MISTLANE_MLA1_CREDENTIAL",
                "ops-callback:key-2026:"+hex+":realm1:"+std::to_string(now+3600));
            const auto listen = port();
            c.node("apiServer", listen, {"--script", c.path("api.lua").string()});
            const auto sent = http("http://"+endpoint(listen)+"/mla1", "POST", accept_body("grant-live"));
            require(sent.status == 200, "a live rotation bound refused a signed request");
            equal(sent.body, reply_body({"OK", "INSERTED", "VOLATILE", "grant", ""}));
        }
        ScopedEnv bad("MISTLANE_MLA1_CREDENTIAL", "ops-callback:key-2026:"+hex+":realm1:soon");
        auto& refused = c.spawn({self().string(), "--role", "apiServer", "--listen", endpoint(port()),
            "--script", c.path("api.lua").string()});
        require(refused.wait() != 0 && refused.output().find("rotation bound") != std::string::npos,
                "a malformed rotation bound booted");
    }
    // --journal FILE makes the apiServer acceptance backend durable: the same
    // MLA1 reply gains DURABLE only after the SQLite row commits, and a
    // restarted server answers the lost-reply replay as EXISTING DURABLE.
    {
        ScopedEnv audience("MISTLANE_MLA1_AUDIENCE", "platform");
        ScopedEnv credentials("MISTLANE_MLA1_CREDENTIAL", "ops-callback:key-2026:"+hex+":realm1");
        const auto journal = c.path("api-acceptance.sqlite");
        const auto listen = port();
        auto& durable = c.node("apiServer", listen,
            {"--script", c.path("api.lua").string(), "--journal", journal.string()});
        const auto base = "http://"+endpoint(listen);
        auto sent = http(base+"/mla1", "POST", accept_body("grant-durable"));
        require(sent.status == 200, "a durable acceptance was not 200");
        equal(sent.body, reply_body({"OK", "INSERTED", "DURABLE", "grant", ""}));
        sent = http(base+"/mla1", "POST", accept_body("grant-durable"));
        equal(sent.body, reply_body({"OK", "EXISTING", "DURABLE", "grant", ""}));
        sent = http(base+"/mla1", "POST", accept_body("grant-durable", "grant-v2"));
        require(sent.status == 409, "a durable key with a changed body was not 409");
        equal(sent.body, reply_body({"OK", "CONFLICT", "DURABLE", "", ""}));
        durable.signal(SIGTERM);
        require(durable.wait() == 0, "a durable apiServer did not stop cleanly");
        auto& restored = c.node("apiServer", listen,
            {"--script", c.path("api.lua").string(), "--journal", journal.string()});
        (void)restored;
        sent = http(base+"/mla1", "POST", accept_body("grant-durable"));
        require(sent.status == 200, "a restored durable acceptance was not 200");
        equal(sent.body, reply_body({"OK", "EXISTING", "DURABLE", "grant", ""}));
    }
    std::cout << "api.http surface: signed POST /mla1 inserts, dedupes, refuses [PASS]\n";
    std::cout << "api.http config: env-only enablement, malformed credentials refuse startup [PASS]\n";
}
// The home-facts source end to end: a signed MLA1 OWNER ask answers from the
// home data endpoint's read-only MLD1 OWNERSHIP leg — a record alone is an
// unnamed active lease at its own generation, a named grant is owned, a fence
// freezes the top into an unnamed FENCED gap, and an unknown participant is a
// bare NOT_FOUND. The apiServer without --data, and one with an unreachable
// data address, fail closed: the owner callback never invents a fact.
void api_ownership_source() {
    Context c;
    write(c.path("echo.lua"), echo);
    write(c.path("api.lua"), http_lua);
    ApiSecret secret{};
    for (auto& byte : secret) byte = 0x37;
    std::string hex;
    for (std::size_t i = 0; i < secret.size(); ++i) hex += "37";
    const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    unsigned counter = 0;
    const auto nonce = [&counter] {
        char digits[33];
        std::snprintf(digits, sizeof digits, "%032x", ++counter);
        return std::string(digits);
    };
    const auto owner_body = [&](const std::string& realm, const std::string& character) {
        ApiRequest request;
        request.operation = ApiOperation::Owner;
        request.audience = "platform";
        request.principal = "ops-callback";
        request.key_id = "key-2026";
        request.timestamp = now;
        request.nonce = nonce();
        request.character = {realm, character};
        request.signature = api_sign(request, secret);
        return api_canonical(request) + std::string(request.signature.begin(), request.signature.end());
    };
    const auto reply_body = [](std::initializer_list<std::string_view> values) {
        std::string bytes = "MLA1";
        for (const auto& value : values) {
            const auto size = static_cast<std::uint32_t>(value.size());
            for (int shift : {24, 16, 8, 0}) bytes.push_back(static_cast<char>((size >> shift) & 255));
            bytes.append(value);
        }
        return bytes;
    };
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
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
    const auto data_port = port();
    c.node("data", data_port, {"--script", c.path("echo.lua").string(),
        "--journal", c.path("ownership.sqlite").string()});
    {
        ScopedEnv audience("MISTLANE_MLA1_AUDIENCE", "platform");
        ScopedEnv credentials("MISTLANE_MLA1_CREDENTIAL", "ops-callback:key-2026:"+hex+":home,other");
        const auto listen = port();
        c.node("apiServer", listen, {"--script", c.path("api.lua").string(),
            "--data", endpoint(data_port)});
        const auto base = "http://"+endpoint(listen);
        // An unknown participant has no record and no lease: the one honest
        // NotFound, never an invented owner.
        auto sent = http(base+"/mla1", "POST", owner_body("home", "novice"));
        require(sent.status == 404, "an unknown participant was not 404");
        equal(sent.body, reply_body({"OK", "", "", "", "NOT_FOUND"}));
        // A record without any grant owns itself but names no one: the implicit
        // lease of a migration-era or never-granted record.
        equal(ask(data_port, "MLD1 SUBMIT home leased gen 2 seq 1 hp=1"),
            "MLD1 ACK durable home leased gen 2 seq 1");
        sent = http(base+"/mla1", "POST", owner_body("home", "leased"));
        require(sent.status == 200, "a record-backed owner ask was not 200");
        equal(sent.body, reply_body({"OK", "", "", "", "FOUND", "-", "2", "ACTIVE"}));
        // A named grant is owned: the entry face names its placed instance.
        equal(ask(data_port, "MLD1 GRANT home leased xs-a"), "MLD1 GRANTED gen 3");
        sent = http(base+"/mla1", "POST", owner_body("home", "leased"));
        equal(sent.body, reply_body({"OK", "", "", "", "FOUND", "xs-a", "3", "ACTIVE"}));
        // The write consumes the granted generation; a fence freezes the top
        // into an unnamed FENCED gap even while the lease sits at it.
        equal(ask(data_port, "MLD1 SUBMIT home leased gen 3 seq 2 hp=2"),
            "MLD1 ACK durable home leased gen 3 seq 2");
        equal(ask(data_port, "MLD1 FENCE home leased gen 3"), "MLD1 FENCED gen 3 seq 2");
        sent = http(base+"/mla1", "POST", owner_body("home", "leased"));
        equal(sent.body, reply_body({"OK", "", "", "", "FOUND", "-", "3", "FENCED"}));
        // The same character name in another realm stays scoped to its own key.
        sent = http(base+"/mla1", "POST", owner_body("other", "leased"));
        require(sent.status == 404, "a foreign realm ask was not 404");
        equal(sent.body, reply_body({"OK", "", "", "", "NOT_FOUND"}));
    }
    // Without --data every ask fails closed; so does an unreachable data leg.
    ScopedEnv audience("MISTLANE_MLA1_AUDIENCE", "platform");
    ScopedEnv credentials("MISTLANE_MLA1_CREDENTIAL", "ops-callback:key-2026:"+hex+":home,other");
    {
        const auto listen = port();
        c.node("apiServer", listen, {"--script", c.path("api.lua").string()});
        const auto sent = http("http://"+endpoint(listen)+"/mla1", "POST", owner_body("home", "leased"));
        require(sent.status == 404, "a sourceless owner ask was not 404");
        equal(sent.body, reply_body({"OK", "", "", "", "NOT_FOUND"}));
    }
    {
        const auto dead = port();
        const auto listen = port();
        c.node("apiServer", listen, {"--script", c.path("api.lua").string(),
            "--data", endpoint(dead)});
        const auto sent = http("http://"+endpoint(listen)+"/mla1", "POST", owner_body("home", "leased"));
        require(sent.status == 404, "an unreachable source did not fail closed");
        equal(sent.body, reply_body({"OK", "", "", "", "NOT_FOUND"}));
    }
    std::cout << "api.ownership.source: signed OWNER asks read the real lease/fence "
        "facts from --data, absent or unreachable sources fail closed [PASS]\n";
}
// The apiServer's callback-class and multi-listener acceptance: one apiServer
// serves the same script face on two http listeners, and an HTTP handler asks
// the home data leg through mistlane.data.load -- the answer cannot ride the
// HTTP response (the leg is a fresh connection and the callback fires after
// the handler returned), so the script stashes it where a later request reads
// it. Pins the callback-after-return shape with the error-first answers (an
// absent participant is EMPTY, a node without --data answers disabled), and
// that the signed /mla1 intercept and Lua routing answer on EVERY listener,
// with one acceptance dedup table shared across them.
void api_callback() {
    Context c;
    write(c.path("echo.lua"), echo);
    write(c.path("cb.lua"), R"cb(local state = {}
return {
  http = function(r)
    local realm, character = r.path:match('^/load/(%w+)/(%w+)$')
    if realm then
      mistlane.data.load(realm, character, function(err, record)
        state[realm..':'..character] = err and ('ERR '..err)
          or (record and ('REC '..record.generation..' '..record.sequence..' '..record.payload) or 'EMPTY')
      end)
      return {status=202, body='asked'}
    end
    local asked, name = r.path:match('^/result/(%w+)/(%w+)$')
    if asked then return {status=200, body=state[asked..':'..name] or 'PENDING'} end
    return {status=404, body='missing'}
  end,
}
)cb");
    ApiSecret secret{};
    for (auto& byte : secret) byte = 0x37;
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    for (unsigned char byte : secret) { hex += digits[byte/16]; hex += digits[byte%16]; }
    ScopedEnv audience("MISTLANE_MLA1_AUDIENCE", "platform");
    ScopedEnv credentials("MISTLANE_MLA1_CREDENTIAL", "ops-callback:key-2026:"+hex+":realm1");
    const auto data = port(), primary = port(), second = port(), bare = port();
    c.node("data", data, {"--script", c.path("echo.lua").string(),
        "--journal", c.path("cb.sqlite").string()});
    equal(tcp(data)->request("MLD1 SUBMIT alpha hero gen 1 seq 1 hp=10"),
        "MLD1 ACK durable alpha hero gen 1 seq 1");
    auto& api = c.node("apiServer", primary, {"--script", c.path("cb.lua").string(),
        "--data", endpoint(data),
        "--listener", "http://127.0.0.1:"+std::to_string(second)});
    api.ready("ready apiServer 127.0.0.1:"+std::to_string(second));
    const auto one = "http://"+endpoint(primary), two = "http://"+endpoint(second);
    // The handler answers 202 before the callback can have answered: the ask
    // rides a fresh connection the script cannot synchronously await.
    auto sent = http(one+"/load/alpha/hero");
    require(sent.status == 202, "the load ask did not answer before the callback");
    equal(sent.body, "asked");
    // The callback fires after the handler returned; the answer becomes
    // visible to a later request -- read from the OTHER listener.
    eventually([&] { return http(two+"/result/alpha/hero").body == "REC 1 1 hp=10"; },
        5s, "the post-return callback never recorded the load answer");
    // An absent participant is the normal first login, not a refusal.
    sent = http(two+"/load/alpha/novice");
    require(sent.status == 202, "the absent-record ask did not answer 202");
    eventually([&] { return http(one+"/result/alpha/novice").body == "EMPTY"; },
        5s, "the absent record did not answer EMPTY");
    // Lua routing answers on every listener.
    equal(http(one+"/nowhere", "POST", "ping").body, "missing");
    equal(http(two+"/nowhere", "POST", "ping").body, "missing");
    // The signed intercept rides both listeners and the acceptance dedup
    // table is shared across them: INSERTED on one, EXISTING on the other.
    const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    unsigned counter = 0;
    const auto nonce = [&counter] {
        char digits[33];
        std::snprintf(digits, sizeof digits, "%032x", ++counter);
        return std::string(digits);
    };
    const auto accept_body = [&](const char* idempotency) {
        ApiRequest request;
        request.operation = ApiOperation::Accept;
        request.audience = "platform";
        request.principal = "ops-callback";
        request.key_id = "key-2026";
        request.timestamp = now;
        request.nonce = nonce();
        request.character = {"realm1", "char42"};
        request.idempotency_key = idempotency;
        request.payload = "grant";
        request.signature = api_sign(request, secret);
        return api_canonical(request) + std::string(request.signature.begin(), request.signature.end());
    };
    const auto reply_body = [](std::initializer_list<std::string_view> values) {
        std::string bytes = "MLA1";
        for (const auto& value : values) {
            const auto size = static_cast<std::uint32_t>(value.size());
            for (int shift : {24, 16, 8, 0}) bytes.push_back(static_cast<char>((size >> shift) & 255));
            bytes.append(value);
        }
        return bytes;
    };
    sent = http(one+"/mla1", "POST", accept_body("cb-1"));
    require(sent.status == 200, "a signed acceptance was not 200 on the primary listener");
    equal(sent.body, reply_body({"OK", "INSERTED", "VOLATILE", "grant", ""}));
    sent = http(two+"/mla1", "POST", accept_body("cb-1"));
    require(sent.status == 200, "a retried acceptance was not 200 on the second listener");
    equal(sent.body, reply_body({"OK", "EXISTING", "VOLATILE", "grant", ""}));
    // A node without --data answers the disabled word through the same
    // callback shape instead of failing the script.
    auto& lonely = c.node("apiServer", bare, {"--script", c.path("cb.lua").string()});
    lonely.ready();
    require(http("http://"+endpoint(bare)+"/load/alpha/hero").status == 202,
        "the bare ask did not answer 202");
    eventually([&] { return http("http://"+endpoint(bare)+"/result/alpha/hero").body == "ERR disabled"; },
        5s, "a bare apiServer did not answer the disabled word");
    std::cout << "api.callback: the post-return callback records the home-data answer, "
        "both listeners share the script and dedup faces [PASS]\n";
}
void login_ticket_wire() {
    Context c;
    write(c.path("login.lua"),
        "return {message=function(p,c)\n"
        " local realm, character, instance, generation, expires = p:match('issue (%S+) (%S+) (%S+) (%S+) (%S+)')\n"
        " if not realm then return 'usage: issue realm character instance generation expires' end\n"
        " local ticket, err = mistlane.ticket.issue(realm, character, instance, tonumber(generation), tonumber(expires))\n"
        " if not ticket then return 'ERR '..err end\n"
        " return ticket\n"
        "end}");
    write(c.path("host.lua"),
        "return {message=function(p,c)"
        " return (c.character or 'anon')..' verified='..tostring(c.verified==true) end}");
    ApiSecret secret{};
    for (std::size_t i = 0; i < secret.size(); ++i) secret[i] = static_cast<unsigned char>(i*13+5);
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    for (unsigned char byte : secret) { hex += digits[byte/16]; hex += digits[byte%16]; }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto login = port(), host = port();
    {
        ScopedEnv secrets("MISTLANE_TICKET_SECRETS", "alpha:"+hex);
        c.node("login", login, {"--script", c.path("login.lua").string()});
        c.node("crossServer", host, {"--script", c.path("host.lua").string(), "--service-name", "xs-a"});
    }
    const auto ask = [&]() {
        return tcp(login)->request("issue alpha hero xs-a 7 "+std::to_string(now+120));
    };
    const auto ticket = ask();
    require(ticket.find("ERR ") != 0 && !ticket.empty(), "the login script failed to issue a ticket: "+ticket);
    require(parse_entry_ticket(ticket).has_value(), "the Lua API did not return a valid MLT1 hex ticket");
    const auto join = "MLC1 1 alpha arena r1 ch hero gen 7 ret alpha arena r1 tk "+ticket;
    {
        auto peer = tcp(host);
        equal(peer->request(join), "MLC1 OK 1");
        equal(peer->request("who"), "hero verified=true");
    }
    {
        auto replay = tcp(host);
        equal(replay->request(join), "MLC1 ERR TICKET");
    }
    const auto fresh = ask();
    require(fresh != ticket, "the Lua issuer reused a nonce");
    // An unconfigured login has no ticket API at all, so a script error is
    // surfaced rather than minting an unsigned claim.
    const auto unconfigured = port();
    c.node("login", unconfigured, {"--script", c.path("login.lua").string()});
    must_fail([&] { (void)tcp(unconfigured)->request("issue alpha hero xs-a 7 "+std::to_string(now+120)); });
    // Bad ticket arguments must be Lua errors from lua_pcall, not C++
    // exceptions crossing the boundary. The same login remains usable for the
    // next well-formed request, and neither negative value can wrap to UINT64_MAX.
    must_fail([&] { (void)tcp(login)->request("issue al!pha hero xs-a 7 99"); });
    must_fail([&] { (void)tcp(login)->request("issue alpha hero xs-a -1 99"); });
    must_fail([&] { (void)tcp(login)->request("issue alpha hero xs-a 7 -1"); });
    // A well-formed realm the issuer holds no secret for answers nil+reason
    // through the API instead of an error.
    equal(tcp(login)->request("issue beta hero xs-a 7 "+std::to_string(now+120)),
        "ERR no ticket secret for this realm or invalid arguments");
    require(!ask().empty(), "login did not keep serving after a rejected ticket argument");
    std::cout << "login.ticket.wire: Lua issuer -> crossServer verifier [PASS]\n";
}
// The login entry flow asks the home data side for the hosting generation
// instead of choosing one: mistlane.grant answers through the callback with a
// strictly greater generation every time, the allocation survives a data
// restart, a tokened data leg is one ask like any other, and the ticket the
// same login issues is bound to exactly the granted number — so a host admits
// that arrival and refuses a join that claims a different generation. A login
// without --data exposes no grant at all.
void login_grant() {
    Context c;
    write(c.path("echo.lua"), echo);
    write(c.path("grant.lua"),
        "return {message=function(p,c)\n"
        " local realm, character = p:match('grant (%S+) (%S+)')\n"
        " if not realm then return 'usage: grant realm character' end\n"
        " mistlane.grant(realm, character, function(err, gen)\n"
        "   mistlane.session.send(c.session, err and ('ERR '..err) or ('OK '..gen))\n"
        " end)\n"
        " return nil end}");
    write(c.path("bind.lua"),
        "return {message=function(p,c)\n"
        " local realm, character, instance, expires = p:match('bind (%S+) (%S+) (%S+) (%S+)')\n"
        " if not realm then return 'usage: bind realm character instance expires' end\n"
        " mistlane.grant(realm, character, function(err, gen)\n"
        "   if err then return mistlane.session.send(c.session, 'ERR '..err) end\n"
        "   local ticket, issue = mistlane.ticket.issue(realm, character, instance, gen, tonumber(expires))\n"
        "   if not ticket then return mistlane.session.send(c.session, 'ERR '..issue) end\n"
        "   mistlane.session.send(c.session, 'GEN '..gen..' TICKET '..ticket)\n"
        " end)\n"
        " return nil end}");
    write(c.path("host.lua"),
        "return {message=function(p,c)"
        " return (c.character or 'anon')..' verified='..tostring(c.verified==true) end}");
    ApiSecret secret{};
    for (std::size_t i = 0; i < secret.size(); ++i) secret[i] = static_cast<unsigned char>(i*13+5);
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    for (unsigned char byte : secret) { hex += digits[byte/16]; hex += digits[byte%16]; }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto data = port(), login = port(), bind_login = port(), host = port(),
        token_login = port(), wrong_login = port(), bare = port(), tokened = port();
    const auto journal = c.path("grant.sqlite");
    const auto grant = [&](unsigned listen, const std::string& line) {
        return tcp(listen)->request(line);
    };
    // The durable data node hands out strictly greater generations to the same
    // participant, and the login surfaces each answer through its callback.
    {
        auto& initial = c.node("data", data, {"--script", c.path("echo.lua").string(),
            "--journal", journal.string()});
        c.node("login", login, {"--script", c.path("grant.lua").string(),
            "--data", endpoint(data)});
        equal(grant(login, "grant alpha hero"), "OK 1");
        equal(grant(login, "grant alpha hero"), "OK 2");
        equal(grant(login, "grant alpha hero"), "OK 3");
        // A different participant starts its own cursor, independently.
        equal(grant(login, "grant alpha sidekick"), "OK 1");
        initial.signal(SIGTERM);
        require(initial.wait() == 0, "grant data node did not stop cleanly");
    }
    c.node("data", data, {"--script", c.path("echo.lua").string(),
        "--journal", journal.string()});
    c.node("login", login, {"--script", c.path("grant.lua").string(),
        "--data", endpoint(data)});
    // The restart kept the high-water mark, so the same participant resumes
    // strictly above the last grant rather than reusing a freed number.
    equal(grant(login, "grant alpha hero"), "OK 4");
    // A tokened data node still answers one identified ask; a login with the
    // wrong token gets the refusal word, not a hang or a wrong generation.
    c.node("data", tokened, {"--script", c.path("echo.lua").string(),
        "--service-token", "s3cret"});
    c.node("login", token_login, {"--script", c.path("grant.lua").string(),
        "--data", endpoint(tokened), "--upstream-token", "s3cret"});
    c.node("login", wrong_login, {"--script", c.path("grant.lua").string(),
        "--data", endpoint(tokened), "--upstream-token", "nope"});
    equal(grant(token_login, "grant alpha hero"), "OK 1");
    equal(grant(wrong_login, "grant alpha hero"), "ERR refused");
    // A login with no data endpoint still exposes mistlane.grant (like
    // mistlane.place without --coordinator): the callback answers a graceful
    // "disabled" reason word instead of failing the script or hanging.
    c.node("login", bare, {"--script", c.path("grant.lua").string()});
    equal(grant(bare, "grant alpha hero"), "ERR disabled");
    // The ticket the same login issues is bound to the granted generation: a
    // host admits that exact arrival and refuses a join that lies about it.
    {
        ScopedEnv secrets("MISTLANE_TICKET_SECRETS", "alpha:"+hex);
        c.node("login", bind_login, {"--script", c.path("bind.lua").string(),
            "--data", endpoint(data)});
        c.node("crossServer", host, {"--script", c.path("host.lua").string(),
            "--service-name", "xs-a"});
    }
    const auto reply = grant(bind_login, "bind alpha hero xs-a "+std::to_string(now+120));
    require(reply.rfind("GEN ", 0) == 0, "the bind script did not answer with a granted ticket: "+reply);
    const auto gen_at = reply.find(' ', 4);
    const auto gen = std::stoull(reply.substr(4, gen_at-4));
    const auto ticket = reply.substr(gen_at+8); // "TICKET <hex>"
    require(parse_entry_ticket(ticket).has_value(), "the granted ticket was not a valid MLT1 hex token");
    const auto join = "MLC1 1 alpha arena r1 ch hero gen "+std::to_string(gen)+" ret alpha arena r1 tk "+ticket;
    {
        auto peer = tcp(host);
        equal(peer->request(join), "MLC1 OK 1");
        equal(peer->request("who"), "hero verified=true");
    }
    {
        auto replay = tcp(host);
        equal(replay->request(join), "MLC1 ERR TICKET");
    }
    {
        // A ticket bound to the granted generation cannot vouch for another
        // one: the join claims gen+1 and is refused as malformed, not admitted.
        auto lying = tcp(host);
        const auto mismatch = "MLC1 1 alpha arena r1 ch hero gen "+std::to_string(gen+1)
            +" ret alpha arena r1 tk "+ticket;
        equal(lying->request(mismatch), "MLC1 ERR INVALID");
    }
    std::cout << "login.grant: the data-issued generation answers the entry flow "
        "and is the one the ticket binds [PASS]\n";
}

// The login charlist face: mistlane.data.load asks the home data endpoint for
// one participant's record over one MLD1 LOAD -- the leg the crossServer's
// home join drives natively -- and the callback answers (error, record) with
// the record nil both for an absent participant (the first login, error nil)
// and for a leg refusal (which carries the reason word). A login without
// --data answers the disabled reason instead of failing the script.
void login_data() {
    Context c;
    write(c.path("echo.lua"), echo);
    write(c.path("load.lua"),
        "return {message=function(p,c)\n"
        " local realm, character = p:match('load (%S+) (%S+)')\n"
        " if not realm then return 'usage: load realm character' end\n"
        " mistlane.data.load(realm, character, function(err, record)\n"
        "   if err then return mistlane.session.send(c.session, 'ERR '..err) end\n"
        "   if not record then return mistlane.session.send(c.session, 'EMPTY') end\n"
        "   mistlane.session.send(c.session, 'REC '..record.generation..' '..record.sequence..' '..record.payload)\n"
        " end)\n"
        " return nil end}");
    const auto data = port(), login = port(), tokened = port(), token_login = port(),
        wrong = port(), bare = port();
    c.node("data", data, {"--script", c.path("echo.lua").string(),
        "--journal", c.path("load.sqlite").string()});
    c.node("login", login, {"--script", c.path("load.lua").string(), "--data", endpoint(data)});
    equal(tcp(data)->request("MLD1 SUBMIT alpha hero gen 1 seq 1 hp=10"),
        "MLD1 ACK durable alpha hero gen 1 seq 1");
    // A seeded record answers with its full identity; an unknown participant
    // answers (nil, nil) -- the first login, not a refusal.
    equal(tcp(login)->request("load alpha hero"), "REC 1 1 hp=10");
    equal(tcp(login)->request("load alpha novice"), "EMPTY");
    // A tokened data leg is one identified ask; the wrong token is refused.
    c.node("data", tokened, {"--script", c.path("echo.lua").string(), "--service-token", "s3cret"});
    c.node("login", token_login, {"--script", c.path("load.lua").string(),
        "--data", endpoint(tokened), "--upstream-token", "s3cret"});
    c.node("login", wrong, {"--script", c.path("load.lua").string(),
        "--data", endpoint(tokened), "--upstream-token", "nope"});
    equal(tcp(token_login)->request("load alpha hero"), "EMPTY");
    equal(tcp(wrong)->request("load alpha hero"), "ERR refused");
    // No --data: the node answers the disabled word, same as grant -- a bare
    // login carries the sink but its data address is empty.
    c.node("login", bare, {"--script", c.path("load.lua").string()});
    equal(tcp(bare)->request("load alpha hero"), "ERR disabled");
    // Malformed arguments are Lua errors, never crashes.
    must_fail([&] { (void)tcp(login)->request("load al!pha hero"); });
    std::cout << "login.data: the charlist face reads the home record, EMPTY is a first login [PASS]\n";
}

// The composed normal-login acceptance: one client walks the whole entry flow
// over bounded two-way messages -- auth (the script's credential check), the
// charlist (the record read from home data), then entry (place -> grant ->
// ticket) -- and the placed crossServer admits the arrival at exactly the
// granted generation with the login's ticket, hosting the avatar from the
// record the charlist previewed. This is the runtime acceptance's first item
// (docs/avatar-runtime.md): every hop is a real node.
void login_entry() {
    Context c;
    write(c.path("echo.lua"), echo);
    write(c.path("login.lua"), R"login(return {message=function(p,c)
  local user, pass = p:match('auth (%S+) (%S+)')
  if user then return pass == 'secret' and 'OK' or 'ERR DENIED' end
  local realm, character = p:match('charlist (%S+) (%S+)')
  if realm then
    mistlane.data.load(realm, character, function(err, record)
      if err then return mistlane.session.send(c.session, 'ERR '..err) end
      if not record then return mistlane.session.send(c.session, 'EMPTY') end
      mistlane.session.send(c.session, 'CHAR '..record.generation..' '..record.sequence..' '..record.payload)
    end)
    return nil
  end
  local eRealm, eChar, instance, expires = p:match('entry (%S+) (%S+) (%S+) (%S+)')
  if eRealm then
    mistlane.place('crossServer', nil, 'arena', function(err, address)
      if err then return mistlane.session.send(c.session, 'ERR place '..err) end
      mistlane.grant(eRealm, eChar, instance, function(err, gen)
        if err then return mistlane.session.send(c.session, 'ERR grant '..err) end
        local ticket, terr = mistlane.ticket.issue(eRealm, eChar, instance, gen, tonumber(expires))
        if not ticket then return mistlane.session.send(c.session, 'ERR ticket '..terr) end
        mistlane.session.send(c.session, 'ENTRY '..address..' '..gen..' '..ticket)
      end)
    end)
    return nil
  end
  return p
end})login");
    write(c.path("host.lua"), R"host(return {
  message = function(payload, ctx)
    if payload == 'who' then return (ctx.character or 'anon')..' verified='..tostring(ctx.verified==true) end
    return payload
  end,
  avatar_load = function(context, state)
    print('load:' .. context.character .. ':' .. tostring(state))
    return { name = context.character }
  end,
  avatar_enter = function(context, avatar) return true end,
  avatar_save = function(context, avatar) return true end,
}
)host");
    // The same per-realm secret mints on the login and verifies on the host.
    const auto secret = test_secret(5);
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    for (unsigned char byte : secret) { hex += digits[byte/16]; hex += digits[byte%16]; }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto registry = port(), data_port = port(), login = port(), host = port();
    c.node("coordinator", registry, {"--journal", c.path("entry.sqlite").string()});
    c.node("data", data_port, {"--script", c.path("echo.lua").string(),
        "--journal", c.path("entry-records.sqlite").string()});
    {
        ScopedEnv secrets("MISTLANE_TICKET_SECRETS", "alpha:"+hex);
        c.node("login", login, {"--script", c.path("login.lua").string(),
            "--coordinator", endpoint(registry), "--data", endpoint(data_port)});
        c.node("crossServer", host, {"--script", c.path("host.lua").string(),
            "--coordinator", endpoint(registry), "--data", endpoint(data_port),
            "--scope", "arena", "--service-name", "xs-a"});
    }
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
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
    // The placed host must be a live member before an entry names it.
    eventually([&] {
        return ask(registry, "MLR1 QUERY crossServer - arena").find("xs-a") != std::string::npos;
    }, 10s, "the crossServer never reached the registry");
    // The seed: hero has a record at home; novice is a first login.
    equal(ask(data_port, "MLD1 SUBMIT alpha hero gen 1 seq 1 hp=10"),
        "MLD1 ACK durable alpha hero gen 1 seq 1");
    // The client's flow, over bounded two-way messages: auth is the script's
    // decision; the charlist previews the record; entry composes place,
    // grant and ticket.
    equal(tcp(login)->request("auth hero secret"), "OK");
    equal(tcp(login)->request("auth hero wrong"), "ERR DENIED");
    equal(tcp(login)->request("charlist alpha hero"), "CHAR 1 1 hp=10");
    equal(tcp(login)->request("charlist alpha novice"), "EMPTY");
    // The placement answers EMPTY until the placed host passes its readiness
    // hysteresis, so the entry composition is retried until it answers; each
    // retry consumes a grant, and the final reply is the one the join uses.
    std::string reply;
    eventually([&] {
        reply = tcp(login)->request("entry alpha hero xs-a "+std::to_string(now+120));
        return reply.starts_with("ENTRY ");
    }, 15s, "the entry flow never answered: "+reply);
    std::istringstream parts(reply.substr(6));
    std::string address, ticket;
    std::uint64_t generation = 0;
    parts >> address >> generation >> ticket;
    require(!address.empty() && address != "nil", "the entry response carried no placed address");
    require(generation > 1, "the granted generation did not floor above the record: "+reply);
    require(parse_entry_ticket(ticket).has_value(), "the entry response carried no valid ticket");
    // The placed host admits the arrival at the granted generation with the
    // login's ticket and hosts the avatar from the record the charlist
    // previewed (the home join's deferred record fetch). The join answers
    // after the record fetch, and the address the entry named is the host's.
    require(address == endpoint(host), "the placed address is not the crossServer's: "+reply);
    {
        RawSocket raw(host);
        send_frame(raw, "MLC1 1 alpha arena r1 ch hero gen "+std::to_string(generation)
            +" ret alpha arena r1 tk "+ticket);
        equal(read_frame(raw), "MLC1 OK 1");
        send_frame(raw, "who");
        equal(read_frame(raw), "hero verified=true");
    }
    eventually([&] {
        return c.children.back()->output().find("load:hero:hp=10") != std::string::npos;
    }, 10s, "the hosted avatar never loaded the record the charlist previewed");
    // The first login composes the same flow with no record: grant 1, EMPTY
    // charlist behind it, nil state in avatar_load.
    const auto first = tcp(login)->request("entry alpha novice xs-a "+std::to_string(now+120));
    require(first.starts_with("ENTRY "), "the first-login entry flow never answered: "+first);
    std::istringstream novice_parts(first.substr(6));
    std::string novice_addr, novice_ticket;
    std::uint64_t novice_generation = 0;
    novice_parts >> novice_addr >> novice_generation >> novice_ticket;
    require(novice_generation == 1, "a fresh participant did not start its own cursor at 1: "+first);
    require(parse_entry_ticket(novice_ticket).has_value(), "the novice entry carried no valid ticket");
    {
        RawSocket raw(host);
        send_frame(raw, "MLC1 1 alpha arena r1 ch novice gen "+std::to_string(novice_generation)
            +" ret alpha arena r1 tk "+novice_ticket);
        equal(read_frame(raw), "MLC1 OK 1");
    }
    eventually([&] {
        return c.children.back()->output().find("load:novice:nil") != std::string::npos;
    }, 10s, "the first-login avatar never loaded without a record");
    std::cout << "login.entry: auth -> charlist -> place/grant/ticket -> placed host admits and "
        "hosts the avatar, the whole normal-login chain [PASS]\n";
}
// Character route records: a route is only written against a registered
// member, GET answers from the live member state, and a retiring member drops
// every route that pointed at it.
}
