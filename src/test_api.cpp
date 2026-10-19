// SPDX-License-Identifier: Apache-2.0
#include "test_support.hpp"
#include "mistlane/api.hpp"
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
}
