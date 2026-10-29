// SPDX-License-Identifier: Apache-2.0
#include "mistlane/api_store.hpp"
#include <sqlite3.h>
#include <mutex>
#include <stdexcept>

namespace mistlane {
namespace {
constexpr unsigned kVersion = 1;

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
        throw std::runtime_error("api acceptance store schema failed: "+message);
    }
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK)
            sqlite_throw(db_, "api acceptance store prepare failed");
    }
    ~Statement() { if (stmt_) sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    sqlite3_stmt* get() { return stmt_; }
    void bind_text(int index, std::string_view value) {
        if (sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                SQLITE_TRANSIENT) != SQLITE_OK)
            sqlite_throw(db_, "api acceptance store bind text failed");
    }
    void bind_int(int index, int value) {
        if (sqlite3_bind_int(stmt_, index, value) != SQLITE_OK)
            sqlite_throw(db_, "api acceptance store bind int failed");
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

bool identifier(std::string_view s) {
    if (s.empty() || s.size() > 64) return false;
    for (unsigned char ch : s)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return false;
    return true;
}
}

struct ApiSqliteAcceptanceStore::Impl {
    explicit Impl(std::string path, std::size_t capacity) : path(std::move(path)), capacity(capacity) {
        if (sqlite3_open_v2(this->path.c_str(), &db, SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,
                nullptr) != SQLITE_OK) {
            const std::string message = db ? sqlite3_errmsg(db) : "cannot open sqlite";
            if (db) sqlite3_close(db);
            db = nullptr;
            throw std::runtime_error("cannot open api acceptance store "+this->path+": "+message);
        }
        sqlite3_busy_timeout(db, 5000);
        sqlite_exec(db, "PRAGMA journal_mode=WAL");
        sqlite_exec(db, "PRAGMA synchronous=FULL");
        sqlite_exec(db,
            "CREATE TABLE IF NOT EXISTS api_acceptance ("
            "version INTEGER NOT NULL, audience TEXT NOT NULL, principal TEXT NOT NULL,"
            "realm TEXT NOT NULL, character TEXT NOT NULL, idempotency_key TEXT NOT NULL,"
            "payload BLOB NOT NULL,"
            "PRIMARY KEY(audience, principal, realm, character, idempotency_key))");
        sqlite_exec(db, "PRAGMA user_version=1");
    }
    ~Impl() { if (db) sqlite3_close(db); }
    std::string path;
    std::size_t capacity;
    std::mutex mutex;
    sqlite3* db = nullptr;
};

ApiSqliteAcceptanceStore::ApiSqliteAcceptanceStore(std::string path, std::size_t capacity)
  : impl_(std::make_unique<Impl>(std::move(path), capacity)) {}
ApiSqliteAcceptanceStore::~ApiSqliteAcceptanceStore() = default;

ApiAcceptance ApiSqliteAcceptanceStore::create(const ApiCommand& command) {
    const auto& key = command.key;
    if (!identifier(key.audience) || !identifier(key.principal)
            || !identifier(key.character.first) || !identifier(key.character.second)
            || !identifier(key.idempotency_key) || command.payload.size() > 1024*1024)
        return {};
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->db == nullptr) throw std::runtime_error("api acceptance store is closed");
    // Read the exact key first: a full store still answers an existing
    // command's lost reply, matching the memory store's check-before-capacity
    // semantics. Only a fresh key is refused by the bound.
    {
        Statement read(impl_->db,
            "SELECT payload FROM api_acceptance WHERE audience=? AND principal=? AND realm=? "
            "AND character=? AND idempotency_key=?");
        read.bind_text(1, key.audience);
        read.bind_text(2, key.principal);
        read.bind_text(3, key.character.first);
        read.bind_text(4, key.character.second);
        read.bind_text(5, key.idempotency_key);
        if (sqlite3_step(read.get()) == SQLITE_ROW) {
            const auto* stored = static_cast<const char*>(sqlite3_column_blob(read.get(), 0));
            const int size = sqlite3_column_bytes(read.get(), 0);
            const std::string body(stored ? stored : "", static_cast<std::size_t>(size));
            if (body != command.payload)
                return {ApiCreate::Conflict, ApiDurability::Durable, std::nullopt};
            return {ApiCreate::Existing, ApiDurability::Durable, ApiReceipt{command}};
        }
        if (sqlite3_errcode(impl_->db) != SQLITE_DONE)
            sqlite_throw(impl_->db, "api acceptance read failed");
    }
    {
        Statement count(impl_->db, "SELECT COUNT(*) FROM api_acceptance");
        if (sqlite3_step(count.get()) != SQLITE_ROW) sqlite_throw(impl_->db, "api acceptance count failed");
        const auto rows = static_cast<std::size_t>(sqlite3_column_int64(count.get(), 0));
        if (rows >= impl_->capacity)
            return {ApiCreate::Full, ApiDurability::Durable, std::nullopt};
    }
    try {
        Statement insert(impl_->db,
            "INSERT INTO api_acceptance(version, audience, principal, realm, character, idempotency_key, payload) "
            "VALUES(?, ?, ?, ?, ?, ?, ?)");
        insert.bind_int(1, static_cast<int>(kVersion));
        insert.bind_text(2, key.audience);
        insert.bind_text(3, key.principal);
        insert.bind_text(4, key.character.first);
        insert.bind_text(5, key.character.second);
        insert.bind_text(6, key.idempotency_key);
        insert.bind_text(7, command.payload);
        insert.step("api acceptance insert failed");
        return {ApiCreate::Inserted, ApiDurability::Durable, ApiReceipt{command}};
    } catch (const std::exception&) {
        // Only a uniqueness violation on the primary key is the expected
        // duplicate path. Any other storage failure (I/O, busy, disk full)
        // must propagate; mistaking it for a Conflict would report a lost
        // reply as an accepted different body and silently drop the command.
        if (sqlite3_extended_errcode(impl_->db) != SQLITE_CONSTRAINT_PRIMARYKEY) throw;
        // A duplicate key is the expected conflict path; re-read and compare.
        Statement read(impl_->db,
            "SELECT payload FROM api_acceptance WHERE audience=? AND principal=? AND realm=? "
            "AND character=? AND idempotency_key=?");
        read.bind_text(1, key.audience);
        read.bind_text(2, key.principal);
        read.bind_text(3, key.character.first);
        read.bind_text(4, key.character.second);
        read.bind_text(5, key.idempotency_key);
        if (sqlite3_step(read.get()) != SQLITE_ROW) sqlite_throw(impl_->db, "api acceptance read failed");
        const auto* stored = static_cast<const char*>(sqlite3_column_blob(read.get(), 0));
        const int size = sqlite3_column_bytes(read.get(), 0);
        const std::string body(stored ? stored : "", static_cast<std::size_t>(size));
        if (body != command.payload)
            return {ApiCreate::Conflict, ApiDurability::Durable, std::nullopt};
        return {ApiCreate::Existing, ApiDurability::Durable, ApiReceipt{command}};
    }
}

std::size_t ApiSqliteAcceptanceStore::size() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->db == nullptr) throw std::runtime_error("api acceptance store is closed");
    Statement count(impl_->db, "SELECT COUNT(*) FROM api_acceptance");
    if (sqlite3_step(count.get()) != SQLITE_ROW) sqlite_throw(impl_->db, "api acceptance count failed");
    return static_cast<std::size_t>(sqlite3_column_int64(count.get(), 0));
}
}
