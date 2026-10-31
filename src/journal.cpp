// SPDX-License-Identifier: Apache-2.0
#include "mistlane/journal.hpp"
#include "mistlane/record.hpp"
#include <sqlite3.h>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace mistlane {
namespace {
constexpr unsigned kVersion = 1;
constexpr std::size_t kMaxBlob = 16u*1024u*1024u;

void sqlite_throw(sqlite3* db, const std::string& what) {
    const char* message = sqlite3_errmsg(db);
    throw std::runtime_error(what + (message && *message ? ": " : "") + (message ? message : ""));
}

void sqlite_exec(sqlite3* db, const char* sql) {
    char* error = nullptr;
    const int code = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (code != SQLITE_OK) {
        const std::string message = error ? error : "sqlite error";
        sqlite3_free(error);
        throw std::runtime_error("migration journal schema failed: "+message);
    }
}

void sqlite_record_exec(sqlite3* db, const char* sql) {
    char* error = nullptr;
    const int code = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (code != SQLITE_OK) {
        const std::string message = error ? error : "sqlite error";
        sqlite3_free(error);
        throw std::runtime_error("record store schema failed: "+message);
    }
}

class RecordStatement {
public:
    RecordStatement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK)
            sqlite_throw(db, "record store prepare failed");
    }
    ~RecordStatement() {
        if (stmt_) sqlite3_finalize(stmt_);
    }
    RecordStatement(const RecordStatement&) = delete;
    RecordStatement& operator=(const RecordStatement&) = delete;
    sqlite3_stmt* get() { return stmt_; }
    void bind_int(int index, int value) {
        if (sqlite3_bind_int(stmt_, index, value) != SQLITE_OK)
            sqlite_throw(db_, "record store bind int failed");
    }
    void bind_u64(int index, std::uint64_t value) {
        if (sqlite3_bind_int64(stmt_, index, static_cast<sqlite3_int64>(value)) != SQLITE_OK)
            sqlite_throw(db_, "record store bind int failed");
    }
    void bind_text(int index, std::string_view value) {
        if (sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                SQLITE_TRANSIENT) != SQLITE_OK)
            sqlite_throw(db_, "record store bind text failed");
    }
    void bind_blob(int index, const std::vector<unsigned char>& value) {
        if (sqlite3_bind_blob64(stmt_, index, value.data(),
                static_cast<sqlite3_uint64>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            sqlite_throw(db_, "record store bind blob failed");
    }
    void step(const char* what) {
        const int code = sqlite3_step(stmt_);
        if (code != SQLITE_DONE && code != SQLITE_ROW)
            sqlite_throw(db_, what);
    }
private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        // The statement is compiled once per call; the journal is small and
        // coordinator traffic is not a throughput path.
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK)
            sqlite_throw(db, "migration journal prepare failed");
    }
    ~Statement() {
        if (stmt_) sqlite3_finalize(stmt_);
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    sqlite3_stmt* get() { return stmt_; }
    void bind_u64(int index, std::uint64_t value) {
        if (sqlite3_bind_int64(stmt_, index, static_cast<sqlite3_int64>(value)) != SQLITE_OK)
            sqlite_throw(db_, "migration journal bind int failed");
    }
    void bind_text(int index, std::string_view value) {
        if (sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                SQLITE_TRANSIENT) != SQLITE_OK)
            sqlite_throw(db_, "migration journal bind text failed");
    }
    void bind_blob(int index, const std::vector<unsigned char>& value) {
        if (sqlite3_bind_blob64(stmt_, index, value.data(),
                static_cast<sqlite3_uint64>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            sqlite_throw(db_, "migration journal bind blob failed");
    }
    void bind_int(int index, int value) {
        if (sqlite3_bind_int(stmt_, index, value) != SQLITE_OK)
            sqlite_throw(db_, "migration journal bind int failed");
    }
    void step() {
        const int code = sqlite3_step(stmt_);
        if (code != SQLITE_DONE && code != SQLITE_ROW)
            sqlite_throw(db_, "migration journal step failed");
    }
private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

void put_u8(std::vector<unsigned char>& out, unsigned char value) { out.push_back(value); }
void put_u32(std::vector<unsigned char>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<unsigned char>((value >> shift) & 255));
}
void put_u64(std::vector<unsigned char>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<unsigned char>((value >> shift) & 255));
}
void put_string(std::vector<unsigned char>& out, std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) throw std::length_error("journal string overflow");
    put_u32(out, static_cast<std::uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}
void put_bool(std::vector<unsigned char>& out, bool value) { put_u8(out, value ? 1 : 0); }

void put_record(std::vector<unsigned char>& out, const RecordSnapshot& snapshot) {
    put_u64(out, snapshot.generation);
    put_u64(out, snapshot.sequence);
    put_string(out, snapshot.payload);
}

class Reader {
public:
    Reader(const void* data, int size) : data_(static_cast<const unsigned char*>(data)),
        size_(static_cast<std::size_t>(size)), pos_(0) {}
    unsigned char byte() {
        require(1);
        return data_[pos_++];
    }
    std::uint32_t u32() {
        require(4);
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) value = (value << 8) | data_[pos_++];
        return value;
    }
    std::uint64_t u64() {
        require(8);
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i) value = (value << 8) | data_[pos_++];
        return value;
    }
    std::string text() {
        const auto length = u32();
        if (length > kMaxBlob) fail("journal string too large");
        require(length);
        const auto value = std::string(reinterpret_cast<const char*>(data_+pos_), length);
        pos_ += length;
        return value;
    }
    bool boolean() {
        const auto value = byte();
        if (value > 1) fail("journal bool is not 0/1");
        return value == 1;
    }
    std::uint32_t count(std::uint32_t limit) {
        const auto value = u32();
        if (value > limit) fail("journal collection too large");
        return value;
    }
    void done() const { if (pos_ != size_) fail("journal has trailing bytes"); }
private:
    void require(std::size_t bytes) const {
        if (bytes > size_-pos_) fail("journal is truncated");
    }
    [[noreturn]] void fail(const char* reason) const { throw std::invalid_argument(reason); }
    const unsigned char* data_;
    std::size_t size_, pos_;
};

void put_owner(std::vector<unsigned char>& out, const MigrationOwner& owner) {
    put_u8(out, static_cast<unsigned char>(owner.scope.kind == MigrationScope::Kind::Realm ? 0 : 1));
    put_string(out, owner.scope.realm);
    put_string(out, owner.scope.cross_group);
    put_string(out, owner.scope.namespace_);
    put_string(out, owner.role);
    put_string(out, owner.instance);
    put_string(out, owner.scene);
}
void put_route(std::vector<unsigned char>& out, const MigrationRoute& route) {
    put_owner(out, route.owner);
    put_u64(out, route.generation);
}
void put_token(std::vector<unsigned char>& out, const MigrationToken& token) {
    put_string(out, token.key.first);
    put_string(out, token.key.second);
    put_string(out, token.id);
    put_u64(out, token.generation);
}
void put_request(std::vector<unsigned char>& out, const MigrationRequest& request) {
    put_string(out, request.key.first);
    put_string(out, request.key.second);
    put_string(out, request.id);
    put_route(out, request.expected);
    put_owner(out, request.destination);
}
void put_entry(std::vector<unsigned char>& out, const MigrationTransaction::Entry& entry) {
    put_request(out, entry.request);
    put_token(out, entry.token);
    put_u8(out, static_cast<unsigned char>(entry.state));
    put_bool(out, entry.source_drained);
    put_bool(out, entry.home_fenced);
    put_bool(out, entry.destination_ready);
    put_bool(out, entry.destination_staged);
}
void put_snapshot(std::vector<unsigned char>& out, const MigrationTransaction::Snapshot& snapshot) {
    put_string(out, snapshot.key.first);
    put_string(out, snapshot.key.second);
    put_route(out, snapshot.route);
    put_route(out, snapshot.home_owner);
    const auto lease = snapshot.lease.view();
    put_u8(out, static_cast<unsigned char>(lease.state));
    put_u64(out, lease.generation);
    put_u64(out, lease.next);
    put_u32(out, static_cast<std::uint32_t>(lease.in_flight));
    put_bool(out, lease.timers_running);
    put_u32(out, static_cast<std::uint32_t>(snapshot.journal.size()));
    for (const auto& [id, entry] : snapshot.journal) {
        put_string(out, id);
        put_entry(out, entry);
    }
    put_string(out, snapshot.current);
}

std::vector<unsigned char> encode_snapshot(const MigrationTransaction::Snapshot& snapshot) {
    std::vector<unsigned char> out;
    put_snapshot(out, snapshot);
    return out;
}

RecordSnapshot read_record(const std::string& realm, const std::string& character, Reader& in) {
    RecordSnapshot snapshot;
    snapshot.realm = realm;
    snapshot.character = character;
    snapshot.generation = in.u64();
    snapshot.sequence = in.u64();
    snapshot.payload = in.text();
    return snapshot;
}

MigrationOwner read_owner(Reader& in) {
    MigrationOwner owner;
    const auto kind = in.byte();
    if (kind > 1) throw std::invalid_argument("journal scope kind is invalid");
    owner.scope.kind = kind == 0 ? MigrationScope::Kind::Realm : MigrationScope::Kind::CrossGroup;
    owner.scope.realm = in.text();
    owner.scope.cross_group = in.text();
    owner.scope.namespace_ = in.text();
    owner.role = in.text();
    owner.instance = in.text();
    owner.scene = in.text();
    return owner;
}
MigrationRoute read_route(Reader& in) {
    MigrationRoute route;
    route.owner = read_owner(in);
    route.generation = in.u64();
    return route;
}
MigrationToken read_token(Reader& in) {
    MigrationToken token;
    token.key.first = in.text();
    token.key.second = in.text();
    token.id = in.text();
    token.generation = in.u64();
    return token;
}
MigrationRequest read_request(Reader& in) {
    MigrationRequest request;
    request.key.first = in.text();
    request.key.second = in.text();
    request.id = in.text();
    request.expected = read_route(in);
    request.destination = read_owner(in);
    return request;
}
MigrationTransaction::Entry read_entry(Reader& in) {
    MigrationTransaction::Entry entry;
    entry.request = read_request(in);
    entry.token = read_token(in);
    const auto state = in.byte();
    if (state > static_cast<unsigned char>(MigrationState::Aborted))
        throw std::invalid_argument("journal migration state is invalid");
    entry.state = static_cast<MigrationState>(state);
    entry.source_drained = in.boolean();
    entry.home_fenced = in.boolean();
    entry.destination_ready = in.boolean();
    entry.destination_staged = in.boolean();
    return entry;
}
MigrationTransaction::Snapshot read_snapshot(Reader& in) {
    MigrationTransaction::Snapshot snapshot;
    snapshot.key.first = in.text();
    snapshot.key.second = in.text();
    snapshot.route = read_route(in);
    snapshot.home_owner = read_route(in);
    const auto lease = in.byte();
    if (lease > static_cast<unsigned char>(WriterLease::State::Relinquished))
        throw std::invalid_argument("journal lease state is invalid");
    if (lease == static_cast<unsigned char>(WriterLease::State::None))
        throw std::invalid_argument("journal lease is unstarted");
    WriterLease::View lease_view;
    lease_view.state = static_cast<WriterLease::State>(lease);
    lease_view.generation = in.u64();
    lease_view.next = in.u64();
    const auto in_flight = in.u32();
    if (in_flight > 1000000) throw std::invalid_argument("journal in-flight count is invalid");
    lease_view.in_flight = in_flight;
    lease_view.timers_running = in.boolean();
    snapshot.lease.restore(lease_view);
    const auto entries = in.count(1000000);
    for (std::uint32_t i = 0; i < entries; ++i) {
        const auto id = in.text();
        if (!snapshot.journal.emplace(id, read_entry(in)).second)
            throw std::invalid_argument("journal contains a duplicate request id");
    }
    snapshot.current = in.text();
    in.done();
    return snapshot;
}
}

struct MigrationJournal::Impl {
    explicit Impl(std::string path) : path(std::move(path)) {
        if (sqlite3_open_v2(this->path.c_str(), &db, SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,
                nullptr) != SQLITE_OK) {
            const std::string message = db ? sqlite3_errmsg(db) : "cannot open sqlite";
            if (db) sqlite3_close(db);
            db = nullptr;
            throw std::runtime_error("cannot open migration journal "+this->path+": "+message);
        }
        sqlite3_busy_timeout(db, 5000);
        sqlite_exec(db, "PRAGMA journal_mode=WAL");
        sqlite_exec(db, "PRAGMA synchronous=FULL");
        sqlite_exec(db,
            "CREATE TABLE IF NOT EXISTS migration_journal ("
            "version INTEGER NOT NULL, namespace TEXT NOT NULL, realm TEXT NOT NULL,"
            "character TEXT NOT NULL, snapshot BLOB NOT NULL,"
            "PRIMARY KEY(namespace, realm, character))");
        sqlite_record_exec(db,
            "CREATE TABLE IF NOT EXISTS record_store ("
            "version INTEGER NOT NULL, realm TEXT NOT NULL, character TEXT NOT NULL,"
            "snapshot BLOB NOT NULL, PRIMARY KEY(realm, character))");
        sqlite_exec(db,
            "CREATE TABLE IF NOT EXISTS home_authority ("
            "version INTEGER NOT NULL, namespace TEXT NOT NULL, realm TEXT NOT NULL,"
            "character TEXT NOT NULL, cursor INTEGER NOT NULL,"
            "PRIMARY KEY(namespace, realm, character))");
        sqlite_record_exec(db,
            "CREATE TABLE IF NOT EXISTS record_fence ("
            "version INTEGER NOT NULL, realm TEXT NOT NULL, character TEXT NOT NULL,"
            "generation INTEGER NOT NULL, sequence INTEGER NOT NULL,"
            "PRIMARY KEY(realm, character))");
        sqlite_exec(db,
            "CREATE TABLE IF NOT EXISTS placement ("
            "version INTEGER NOT NULL, namespace TEXT NOT NULL, scope TEXT NOT NULL,"
            "realm TEXT NOT NULL, character TEXT NOT NULL, role TEXT NOT NULL,"
            "instance TEXT NOT NULL, PRIMARY KEY(namespace, realm, character))");
        // A WAL/schema setting is a process-level hint; this pins the row
        // format so a future incompatible writer refuses old files instead of
        // silently decoding garbage.
        sqlite_exec(db, "PRAGMA user_version=1");
    }
    ~Impl() { if (db) sqlite3_close(db); }
    std::string path;
    sqlite3* db = nullptr;
};

MigrationJournal::MigrationJournal(std::string path) : impl_(std::make_unique<Impl>(std::move(path))) {}
MigrationJournal::~MigrationJournal() = default;

namespace {
std::uint64_t snapshot_high_water(const MigrationTransaction::Snapshot& snapshot) {
    std::uint64_t high = snapshot.route.generation;
    for (const auto& [id, entry] : snapshot.journal) {
        (void)id;
        high = std::max(high, entry.token.generation);
    }
    return high;
}
}

void MigrationJournal::save(const MigrationJournalRecord& record) {
    save(record, snapshot_high_water(record.snapshot));
}

void MigrationJournal::save(const MigrationJournalRecord& record, std::uint64_t cursor) {
    if (impl_->db == nullptr) throw std::runtime_error("migration journal is closed");
    if (!record_identifier(record.key.first) || !record_identifier(record.key.second))
        throw std::invalid_argument("invalid migration journal key");
    // The durable authority cursor is the atomic home fence: it records that
    // every generation at or below it is consumed before the transaction that
    // reserved them is exposed. It must never trail the snapshot's own rows.
    if (!cursor || cursor < snapshot_high_water(record.snapshot))
        throw std::invalid_argument("migration journal authority cursor is behind the snapshot");
    const auto snapshot = encode_snapshot(record.snapshot);
    sqlite_exec(impl_->db, "BEGIN");
    // The snapshot and the authority must become visible together: a crash
    // between them would let a re-scan re-derive a consumed generation, which
    // is precisely the single-writer fence this authority exists to protect.
    try {
        Statement upsert(impl_->db, "INSERT INTO migration_journal(version, namespace, realm, character, snapshot) "
            "VALUES(?, ?, ?, ?, ?) ON CONFLICT(namespace, realm, character) DO UPDATE SET "
            "version=excluded.version, snapshot=excluded.snapshot");
        upsert.bind_int(1, static_cast<int>(kVersion));
        upsert.bind_text(2, record.namespace_);
        upsert.bind_text(3, record.key.first);
        upsert.bind_text(4, record.key.second);
        upsert.bind_blob(5, snapshot);
        upsert.step();
        Statement authority(impl_->db,
            "INSERT INTO home_authority(version, namespace, realm, character, cursor) "
            "VALUES(?, ?, ?, ?, ?) ON CONFLICT(namespace, realm, character) DO UPDATE SET "
            "version=excluded.version, cursor=excluded.cursor");
        authority.bind_int(1, static_cast<int>(kVersion));
        authority.bind_text(2, record.namespace_);
        authority.bind_text(3, record.key.first);
        authority.bind_text(4, record.key.second);
        authority.bind_u64(5, cursor);
        authority.step();
        sqlite_exec(impl_->db, "COMMIT");
    } catch (...) {
        sqlite_exec(impl_->db, "ROLLBACK");
        throw;
    }
}

void MigrationJournal::save_authority(const MigrationAuthorityRecord& record) {
    if (impl_->db == nullptr) throw std::runtime_error("migration journal is closed");
    if (!record_identifier(record.key.first) || !record_identifier(record.key.second) || !record.cursor)
        throw std::invalid_argument("invalid home authority key or cursor");
    Statement authority(impl_->db,
        "INSERT INTO home_authority(version, namespace, realm, character, cursor) "
        "VALUES(?, ?, ?, ?, ?) ON CONFLICT(namespace, realm, character) DO UPDATE SET "
        "version=excluded.version, cursor=excluded.cursor");
    authority.bind_int(1, static_cast<int>(kVersion));
    authority.bind_text(2, record.namespace_);
    authority.bind_text(3, record.key.first);
    authority.bind_text(4, record.key.second);
    authority.bind_u64(5, record.cursor);
    authority.step();
}

std::vector<MigrationAuthorityRecord> MigrationJournal::restore_authorities() const {
    if (impl_->db == nullptr) throw std::runtime_error("migration journal is closed");
    std::vector<MigrationAuthorityRecord> records;
    Statement select(impl_->db,
        "SELECT version, namespace, realm, character, cursor FROM home_authority "
        "ORDER BY namespace, realm, character");
    for (;;) {
        const int code = sqlite3_step(select.get());
        if (code == SQLITE_DONE) break;
        if (code != SQLITE_ROW) sqlite_throw(impl_->db, "home authority restore step failed");
        const int version = sqlite3_column_int(select.get(), 0);
        const auto* namespace_ = reinterpret_cast<const char*>(sqlite3_column_text(select.get(), 1));
        const auto* realm = reinterpret_cast<const char*>(sqlite3_column_text(select.get(), 2));
        const auto* character = reinterpret_cast<const char*>(sqlite3_column_text(select.get(), 3));
        const int namespace_size = sqlite3_column_bytes(select.get(), 1);
        const int realm_size = sqlite3_column_bytes(select.get(), 2);
        const int character_size = sqlite3_column_bytes(select.get(), 3);
        const sqlite3_int64 cursor = sqlite3_column_int64(select.get(), 4);
        std::string ns(namespace_ ? namespace_ : "", static_cast<std::size_t>(namespace_size));
        std::string key_realm(realm ? realm : "", static_cast<std::size_t>(realm_size));
        std::string key_character(character ? character : "", static_cast<std::size_t>(character_size));
        if (version != static_cast<int>(kVersion))
            throw std::runtime_error("home authority version mismatch");
        if (!record_identifier(key_realm) || !record_identifier(key_character) || cursor <= 0)
            throw std::runtime_error("home authority key or cursor corrupted");
        records.push_back({std::move(ns), {std::move(key_realm), std::move(key_character)},
            static_cast<std::uint64_t>(cursor)});
    }
    return records;
}

std::vector<MigrationJournalRecord> MigrationJournal::restore() const {
    if (impl_->db == nullptr) throw std::runtime_error("migration journal is closed");
    std::vector<MigrationJournalRecord> records;
    Statement select(impl_->db,
        "SELECT version, namespace, realm, character, snapshot FROM migration_journal "
        "ORDER BY namespace, realm, character");
    for (;;) {
        const int code = sqlite3_step(select.get());
        if (code == SQLITE_DONE) break;
        if (code != SQLITE_ROW) sqlite_throw(impl_->db, "migration journal restore step failed");
        const int version = sqlite3_column_int(select.get(), 0);
        const auto* namespace_ = reinterpret_cast<const char*>(sqlite3_column_text(select.get(), 1));
        const auto* realm = reinterpret_cast<const char*>(sqlite3_column_text(select.get(), 2));
        const auto* character = reinterpret_cast<const char*>(sqlite3_column_text(select.get(), 3));
        const int namespace_size = sqlite3_column_bytes(select.get(), 1);
        const int realm_size = sqlite3_column_bytes(select.get(), 2);
        const int character_size = sqlite3_column_bytes(select.get(), 3);
        std::string ns(namespace_ ? namespace_ : "", static_cast<std::size_t>(namespace_size));
        std::string key_realm(realm ? realm : "", static_cast<std::size_t>(realm_size));
        std::string key_character(character ? character : "", static_cast<std::size_t>(character_size));
        if (version != static_cast<int>(kVersion))
            throw std::runtime_error("migration journal version mismatch");
        if (!record_identifier(key_realm) || !record_identifier(key_character))
            throw std::runtime_error("migration journal key corrupted");
        Reader reader(sqlite3_column_blob(select.get(), 4), sqlite3_column_bytes(select.get(), 4));
        auto snapshot = read_snapshot(reader);
        if (snapshot.key != MigrationKey{key_realm, key_character})
            throw std::runtime_error("migration journal snapshot key mismatch");
        records.push_back({std::move(ns), {std::move(key_realm), std::move(key_character)}, std::move(snapshot)});
    }
    return records;
}

void MigrationJournal::save_record(const RecordSnapshot& snapshot) {
    if (impl_->db == nullptr) throw std::runtime_error("record store is closed");
    if (!record_identifier(snapshot.realm) || !record_identifier(snapshot.character)
            || !snapshot.generation || !snapshot.sequence)
        throw std::invalid_argument("invalid record store key or fence");
    std::vector<unsigned char> bytes;
    put_record(bytes, snapshot);
    RecordStatement upsert(impl_->db, "INSERT INTO record_store(version, realm, character, snapshot) "
        "VALUES(?, ?, ?, ?) ON CONFLICT(realm, character) DO UPDATE SET "
        "version=excluded.version, snapshot=excluded.snapshot");
    upsert.bind_int(1, static_cast<int>(kVersion));
    upsert.bind_text(2, snapshot.realm);
    upsert.bind_text(3, snapshot.character);
    upsert.bind_blob(4, bytes);
    upsert.step("record store save step failed");
}

void MigrationJournal::save_fence(const HomeSequenceAuthority::Pin& pin) {
    if (impl_->db == nullptr) throw std::runtime_error("record store is closed");
    if (!record_identifier(pin.key.first) || !record_identifier(pin.key.second)
            || !pin.generation || !pin.sequence)
        throw std::invalid_argument("invalid record fence key or pin");
    RecordStatement upsert(impl_->db, "INSERT INTO record_fence(version, realm, character, generation, sequence) "
        "VALUES(?, ?, ?, ?, ?) ON CONFLICT(realm, character) DO UPDATE SET "
        "version=excluded.version, generation=excluded.generation, sequence=excluded.sequence");
    upsert.bind_int(1, static_cast<int>(kVersion));
    upsert.bind_text(2, pin.key.first);
    upsert.bind_text(3, pin.key.second);
    upsert.bind_u64(4, pin.generation);
    upsert.bind_u64(5, pin.sequence);
    upsert.step("record fence save step failed");
}

std::vector<HomeSequenceAuthority::Pin> MigrationJournal::restore_fences() const {
    if (impl_->db == nullptr) throw std::runtime_error("record store is closed");
    std::vector<HomeSequenceAuthority::Pin> pins;
    RecordStatement query(impl_->db,
        "SELECT version, realm, character, generation, sequence FROM record_fence "
        "ORDER BY realm, character");
    for (;;) {
        const int code = sqlite3_step(query.get());
        if (code == SQLITE_DONE) break;
        if (code != SQLITE_ROW) sqlite_throw(impl_->db, "record fence restore step failed");
        const int version = sqlite3_column_int(query.get(), 0);
        const auto* realm = reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 1));
        const auto* character = reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 2));
        const int realm_size = sqlite3_column_bytes(query.get(), 1);
        const int character_size = sqlite3_column_bytes(query.get(), 2);
        const sqlite3_int64 generation = sqlite3_column_int64(query.get(), 3);
        const sqlite3_int64 sequence = sqlite3_column_int64(query.get(), 4);
        std::string key_realm(realm ? realm : "", static_cast<std::size_t>(realm_size));
        std::string key_character(character ? character : "", static_cast<std::size_t>(character_size));
        if (version != static_cast<int>(kVersion) || !record_identifier(key_realm)
                || !record_identifier(key_character) || generation <= 0 || sequence <= 0)
            throw std::runtime_error("record fence row corrupted");
        pins.push_back({{std::move(key_realm), std::move(key_character)},
            static_cast<std::uint64_t>(generation), static_cast<std::uint64_t>(sequence)});
    }
    return pins;
}

std::vector<RecordSnapshot> MigrationJournal::restore_records() const {
    if (impl_->db == nullptr) throw std::runtime_error("record store is closed");
    std::vector<RecordSnapshot> records;
    RecordStatement query(impl_->db,
        "SELECT version, realm, character, snapshot FROM record_store ORDER BY realm, character");
    for (;;) {
        const int code = sqlite3_step(query.get());
        if (code == SQLITE_DONE) break;
        if (code != SQLITE_ROW) sqlite_throw(impl_->db, "record store restore step failed");
        const int version = sqlite3_column_int(query.get(), 0);
        const auto* realm = reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 1));
        const auto* character = reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 2));
        const int realm_size = sqlite3_column_bytes(query.get(), 1);
        const int character_size = sqlite3_column_bytes(query.get(), 2);
        const std::string key_realm(realm ? realm : "", static_cast<std::size_t>(realm_size));
        const std::string key_character(character ? character : "",
            static_cast<std::size_t>(character_size));
        if (version != static_cast<int>(kVersion))
            throw std::runtime_error("record store version mismatch");
        if (!record_identifier(key_realm) || !record_identifier(key_character))
            throw std::runtime_error("record store key corrupted");
        Reader reader(sqlite3_column_blob(query.get(), 3), sqlite3_column_bytes(query.get(), 3));
        records.push_back(read_record(key_realm, key_character, reader));
    }
    return records;
}
void MigrationJournal::save_placement(const PlacementRecord& record) {
    if (impl_->db == nullptr) throw std::runtime_error("migration journal is closed");
    if (!record_identifier(record.scope) || !record_identifier(record.realm)
            || !record_identifier(record.character) || !record_identifier(record.instance)
            || (record.role != "gameServer" && record.role != "crossServer"))
        throw std::invalid_argument("invalid placement record");
    if (!record.namespace_.empty() && !record_identifier(record.namespace_))
        throw std::invalid_argument("invalid placement namespace");
    Statement upsert(impl_->db, "INSERT INTO placement(version, namespace, scope, realm, character, role, instance) "
        "VALUES(?, ?, ?, ?, ?, ?, ?) ON CONFLICT(namespace, realm, character) DO UPDATE SET "
        "version=excluded.version, scope=excluded.scope, role=excluded.role, instance=excluded.instance");
    upsert.bind_int(1, static_cast<int>(kVersion));
    upsert.bind_text(2, record.namespace_);
    upsert.bind_text(3, record.scope);
    upsert.bind_text(4, record.realm);
    upsert.bind_text(5, record.character);
    upsert.bind_text(6, record.role);
    upsert.bind_text(7, record.instance);
    upsert.step();
}

void MigrationJournal::drop_placement(const std::string& namespace_, const std::string& scope,
                                      const std::string& role, const std::string& instance) {
    if (impl_->db == nullptr) throw std::runtime_error("migration journal is closed");
    Statement drop(impl_->db, "DELETE FROM placement WHERE namespace=? AND scope=? AND role=? AND instance=?");
    drop.bind_text(1, namespace_);
    drop.bind_text(2, scope);
    drop.bind_text(3, role);
    drop.bind_text(4, instance);
    drop.step();
}

std::vector<PlacementRecord> MigrationJournal::restore_placements() const {
    if (impl_->db == nullptr) throw std::runtime_error("migration journal is closed");
    std::vector<PlacementRecord> records;
    Statement select(impl_->db,
        "SELECT version, namespace, scope, realm, character, role, instance FROM placement "
        "ORDER BY namespace, realm, character");
    for (;;) {
        const int code = sqlite3_step(select.get());
        if (code == SQLITE_DONE) break;
        if (code != SQLITE_ROW) sqlite_throw(impl_->db, "placement restore step failed");
        const int version = sqlite3_column_int(select.get(), 0);
        const auto* namespace_ = reinterpret_cast<const char*>(sqlite3_column_text(select.get(), 1));
        const auto* scope = reinterpret_cast<const char*>(sqlite3_column_text(select.get(), 2));
        const auto* realm = reinterpret_cast<const char*>(sqlite3_column_text(select.get(), 3));
        const auto* character = reinterpret_cast<const char*>(sqlite3_column_text(select.get(), 4));
        const auto* role = reinterpret_cast<const char*>(sqlite3_column_text(select.get(), 5));
        const auto* instance = reinterpret_cast<const char*>(sqlite3_column_text(select.get(), 6));
        const int namespace_size = sqlite3_column_bytes(select.get(), 1);
        const int scope_size = sqlite3_column_bytes(select.get(), 2);
        const int realm_size = sqlite3_column_bytes(select.get(), 3);
        const int character_size = sqlite3_column_bytes(select.get(), 4);
        const int role_size = sqlite3_column_bytes(select.get(), 5);
        const int instance_size = sqlite3_column_bytes(select.get(), 6);
        PlacementRecord record;
        record.namespace_ = std::string(namespace_ ? namespace_ : "", static_cast<std::size_t>(namespace_size));
        record.scope = std::string(scope ? scope : "", static_cast<std::size_t>(scope_size));
        record.realm = std::string(realm ? realm : "", static_cast<std::size_t>(realm_size));
        record.character = std::string(character ? character : "", static_cast<std::size_t>(character_size));
        record.role = std::string(role ? role : "", static_cast<std::size_t>(role_size));
        record.instance = std::string(instance ? instance : "", static_cast<std::size_t>(instance_size));
        if (version != static_cast<int>(kVersion) || !record_identifier(record.scope)
                || !record_identifier(record.realm) || !record_identifier(record.character)
                || !record_identifier(record.instance)
                || (record.role != "gameServer" && record.role != "crossServer")
                || (!record.namespace_.empty() && !record_identifier(record.namespace_)))
            throw std::runtime_error("placement row corrupted");
        records.push_back(std::move(record));
    }
    return records;
}

}
