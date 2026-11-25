// SPDX-License-Identifier: Apache-2.0
// The serving-path wiring of docs/scene-lifecycle.md: map definitions
// registered at startup from configuration, resident instances created at
// startup and prepared from their definition's real source file, and the
// avatar-membership reconciliation that keeps instances actually hosting the
// avatars whose scene reference names them. The state machine and its
// discipline live in SceneRuntime (include/mistlane/scene_runtime.hpp); this
// host supplies what the model deliberately leaves open: the loader's
// completion (resolve/fail from the registered source), the tick that pumps
// it, and one reconciliation mechanism covering every avatar exit path. No
// map bytes travel between processes — each host loads from its own
// registered source, and a changed definition is a new digest, never an
// in-place rewrite.
#pragma once
#include "mistlane/avatar_runtime.hpp"
#include "mistlane/scene_runtime.hpp"
#include <openssl/evp.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mistlane {

// The definition's registered source: the path the loader re-reads. The file
// format and schema are excluded from the design (docs/scene-lifecycle.md
// "Excluded") — the host treats the bytes as opaque, pins their digest at
// registration, and never rewrites a ready cache entry.
struct SceneDef {
    std::string name;   // the definition id scripts and instances name
    std::string digest; // SHA-256 of the file's bytes, pinned at startup
    std::string path;
};

// A stable participant handle for one avatar inside one instance: a digest of
// its oracle key. The reconciliation derives the same handle on both sides
// (enter and leave), so no reverse map is kept anywhere.
inline std::uint64_t scene_participant_id(const AvatarKey& key) {
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](std::string_view text) {
        for (const unsigned char ch : text) { h ^= ch; h *= 1099511628211ull; }
        h ^= '\0'; h *= 1099511628211ull;
    };
    mix(key.realm);
    mix(key.character);
    mix(std::to_string(key.generation));
    return h;
}

// SHA-256 hex — the definition digest pinned at startup. The digest
// algorithm is the implementation's choice (docs/scene-lifecycle.md excludes
// it); this one pins the registered bytes so a changed file is a changed
// definition, never a silent mutation of a ready entry.
inline std::string scene_sha256_hex(std::string_view bytes) {
    unsigned char md[EVP_MAX_MD_SIZE];
    size_t len = 0;
    if (EVP_Q_digest(nullptr, "SHA256", nullptr, bytes.data(), bytes.size(), md, &len) != 1 || len != 32)
        throw std::runtime_error("scene digest failed");
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned int i = 0; i < len; ++i) { out += hex[md[i] >> 4]; out += hex[md[i] & 15]; }
    return out;
}

// A registered source file, read once: its bytes and their digest. The bytes
// are opaque static content; only the digest is compared.
struct SceneSource { std::string digest, bytes; };
inline bool scene_read(const std::filesystem::path& path, SceneSource& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out.bytes = buffer.str();
    out.digest = scene_sha256_hex(out.bytes);
    return true;
}

class SceneHost {
public:
    explicit SceneHost() : scenes_([] { return std::chrono::steady_clock::now(); }) {}

    // Startup registration (docs: "Registered at startup from configuration
    // or a repository"). The file is read once here to pin its digest;
    // register_definition is the idempotent face, so a repeated registration
    // of the same name+digest is a no-op and a startup that names one
    // definition twice with different files is a configuration error thrown
    // here (the caller validates that too).
    void define(const std::string& name, const std::filesystem::path& path) {
        if (name.empty()) throw std::runtime_error("empty scene definition name");
        SceneSource source;
        if (!scene_read(path, source))
            throw std::runtime_error("cannot read scene definition: "+path.string());
        SceneDef def{name, source.digest, path.string()};
        if (!scenes_.register_definition(def.name, def.digest))
            throw std::runtime_error("invalid scene definition identity: "+name);
        const auto existing = sources_.find(name);
        if (existing != sources_.end() && existing->second.digest != def.digest)
            throw std::runtime_error("scene definition "+name+" registered twice with different files");
        sources_[name] = std::move(def);
    }

    // Startup creation of a resident instance (docs: "Resident instances are
    // created at startup and are never closed by idleness"). begin_prepare
    // takes the definition reference — a cache miss queues the load, which a
    // later tick resolves — so a resident instance sits in `preparing` right
    // after startup and lands `ready` only once its source resolved. A
    // configuration error (unknown definition, duplicate instance name)
    // throws; a load failure is per-request and lands the instance `failed`
    // at the tick, never a startup failure.
    void resident(const std::string& name, const std::string& definition, std::uint64_t budget_ms) {
        const auto created = scenes_.create(name, definition, sources_.at(definition).digest, budget_ms);
        if (!created) throw std::runtime_error("scene instance "+name+": "+created.reason);
        const auto prepared = scenes_.begin_prepare(name);
        if (!prepared) throw std::runtime_error("scene instance "+name+": "+prepared.reason);
        queue_load(definition);
    }

    bool has(const std::string& name) const { return scenes_.has(name); }
    SceneInstanceState state(const std::string& name) const { return scenes_.state(name); }
    std::size_t participants(const std::string& name) const { return scenes_.participants(name); }

    // The switch face's entry gate (docs: "only ready accepts entry ... a
    // not-ready destination refuses entry with an explicit failure; it never
    // reports success and never accepts a participant and waits"): "" on
    // entry, otherwise the explicit refusal word.
    std::string enter(const std::string& name, std::uint64_t participant) {
        const auto entered = scenes_.enter(name, participant);
        if (entered) return {};
        if (!scenes_.has(name)) return "no such scene";
        return "scene is " + std::string(to_string(scenes_.state(name)));
    }
    void leave(const std::string& name, std::uint64_t participant) { scenes_.leave(name, participant); }

    // The node tick: pump the queued loads first (a resolve this tick promotes
    // the waiting prepares before the budget can expire them), then the
    // model's budget pass and the failed-instance reap, then the membership
    // reconciliation against the avatar runtime — null on a node without one,
    // which leaves every participant (a node whose avatars are all gone hosts
    // empty instances, exactly what close-when-empty needs to see).
    void tick(AvatarRuntime* avatars) {
        pump_loads();
        scenes_.tick();
        scenes_.reap_failed();
        reconcile(avatars);
    }

private:
    void queue_load(const std::string& definition) {
        const auto& digest = sources_.at(definition).digest;
        if (scenes_.cache_state(definition, digest) == SceneCacheState::Loading)
            loads_.insert({definition, digest});
    }

    // The real loader: each queued (definition, digest) is read from its
    // registered source on this tick. An unreadable or digest-mismatching
    // source fails the load — recorded for the requests that asked, never for
    // the key, so the same key stays retryable by another prepare and a ready
    // entry is never touched. The resolved bytes are the cache's static data;
    // they live as long as the cache entry, and a key the model evicted
    // (reference zero, no load in flight) drops them so a later prepare
    // re-reads its source, exactly as the model re-loads.
    void pump_loads() {
        for (const auto& key : loads_) {
            if (scenes_.cache_state(key.first, key.second) != SceneCacheState::Loading) continue;
            const auto source = sources_.find(key.first);
            if (source == sources_.end()) { scenes_.fail(key.first, key.second); continue; }
            SceneSource loaded;
            if (!scene_read(source->second.path, loaded) || loaded.digest != key.second) {
                scenes_.fail(key.first, key.second);
                continue;
            }
            data_[key] = loaded.bytes;
            scenes_.resolve(key.first, key.second);
        }
        loads_.clear();
        for (auto it = data_.begin(); it != data_.end();)
            it = scenes_.cache_state(it->first.first, it->first.second) == SceneCacheState::Missing
                     ? data_.erase(it) : std::next(it);
    }

    // Membership reconciliation, the one mechanism for every enter and leave:
    // the desired set is what the live avatars hold (a suspended or
    // transferring avatar still holds its scene — only a closed one holds
    // nothing), the actual set is each instance's participants, and the tick
    // walks both into agreement. Entry needs a ready instance and is retried
    // on the next tick when the instance is still preparing; leaving needs
    // nothing. This covers the switch's old instance, the reclaim sweep, the
    // transfer release and discard, and the SIGHUP retirement uniformly —
    // no avatar exit path needs its own scene leave.
    void reconcile(AvatarRuntime* avatars) {
        std::map<std::string, std::set<std::uint64_t>> wanted;
        if (avatars)
            for (const auto& [key, scene] : avatars->held_scenes())
                wanted[scene].insert(scene_participant_id(key));
        for (const auto& name : scenes_.instance_names()) {
            const auto desired = wanted.find(name);
            for (const auto participant : scenes_.participants_of(name))
                if (desired == wanted.end() || !desired->second.count(participant))
                    scenes_.leave(name, participant);
            if (desired == wanted.end() || scenes_.state(name) != SceneInstanceState::Ready) continue;
            for (const auto participant : desired->second) scenes_.enter(name, participant);
        }
    }

    SceneRuntime scenes_;
    std::map<std::string, SceneDef> sources_;                      // definition name -> source
    std::set<std::pair<std::string, std::string>> loads_;          // loads in flight this tick
    std::map<std::pair<std::string, std::string>, std::string> data_; // resolved static data
};

} // namespace mistlane
