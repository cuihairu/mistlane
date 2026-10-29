// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/api.hpp"
#include <memory>
#include <string>

namespace mistlane {
// Durable MLA1 acceptance backend: the apiServer's "persistent acceptance
// store" behind `--journal FILE` (todo.md gap 6).
// An accepted command is a pending work row: the idempotency key, the exact
// accepted body and the identity that submitted it must survive a restart, or
// a lost reply could let the caller re-submit the same logical command as a
// fresh one. This store mirrors ApiMemoryAcceptanceStore's create-or-read
// semantics but commits each row to SQLite before it is reported, so the
// caller sees Durable only after the row is on disk. A changed body under one
// key stays Conflict and nothing is overwritten; the table is bounded, and a
// full store refuses a fresh key (Full) instead of growing without bound.
class ApiSqliteAcceptanceStore final : public ApiAcceptanceStore {
public:
    explicit ApiSqliteAcceptanceStore(std::string path, std::size_t capacity = 4096);
    ~ApiSqliteAcceptanceStore() override;
    ApiSqliteAcceptanceStore(const ApiSqliteAcceptanceStore&) = delete;
    ApiSqliteAcceptanceStore& operator=(const ApiSqliteAcceptanceStore&) = delete;
    ApiSqliteAcceptanceStore(ApiSqliteAcceptanceStore&&) = delete;
    ApiSqliteAcceptanceStore& operator=(ApiSqliteAcceptanceStore&&) = delete;
    ApiAcceptance create(const ApiCommand& command) override;
    std::size_t size() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
