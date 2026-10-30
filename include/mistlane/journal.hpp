// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/migration.hpp"
#include "mistlane/record.hpp"
#include <memory>
#include <string>
#include <vector>

namespace mistlane {
// One persisted coordinator transaction row. The snapshot is the complete
// trusted aggregate after a mutation: home facts, coordinator route and lease,
// the journal's prepared/terminal ids and the current request. Retaining
// terminal ids across restart is what makes late prepare/commit replays safe
// and keeps every consumed generation visible to the home authority.
struct MigrationJournalRecord {
    std::string namespace_;
    MigrationKey key;
    MigrationTransaction::Snapshot snapshot;
};

// One persisted home-authority cursor row: the authoritative high-water mark
// of the home generation authority for a (namespace, realm, character) key.
// It is written atomically with the transaction snapshot that consumed the
// generation, so a crash can never resurrect an aborted or already-assigned
// generation by re-deriving it from a partial journal.
struct MigrationAuthorityRecord {
    std::string namespace_;
    MigrationKey key;
    std::uint64_t cursor = 0;
};

// Single-process SQLite storage for the coordinator migration journal. This is
// the first persistence-chain layer: it makes coordinator intent durable, but
// it is not the home-data record store. The MLD1/Mld1 durable confirmation and
// the cross-return queue can reuse this store boundary in later slices.
class MigrationJournal : public RecordBackend {
public:
    explicit MigrationJournal(std::string path);
    ~MigrationJournal();
    MigrationJournal(const MigrationJournal&) = delete;
    MigrationJournal& operator=(const MigrationJournal&) = delete;
    MigrationJournal(MigrationJournal&&) = delete;
    MigrationJournal& operator=(MigrationJournal&&) = delete;
    void save(const MigrationJournalRecord&);
    void save(const MigrationJournalRecord&, std::uint64_t cursor);
    void save_authority(const MigrationAuthorityRecord&);
    std::vector<MigrationAuthorityRecord> restore_authorities() const;
    std::vector<MigrationJournalRecord> restore() const;
    void save_record(const RecordSnapshot&) override;
    std::vector<RecordSnapshot> restore_records() const override;
    void save_fence(const HomeSequenceAuthority::Pin& pin) override;
    std::vector<HomeSequenceAuthority::Pin> restore_fences() const override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
