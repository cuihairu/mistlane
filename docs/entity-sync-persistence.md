English | [简体中文](zh/entity-sync-persistence.md)
# BigWorld and KBEngine: property synchronization and contract data persistence

## Scope, evidence and relation to earlier proposals

This document verifies the chain "contract → runtime properties → visible replicas → binary representation → database → contract evolution" and discusses design rationale and costs. **This is engine-behavior research; it does not mean Mistlane has implemented these capabilities.** Data persistence here means entity-property persistence, not storing `.def` files into a database.

[entity-contract.md](./entity-contract.md) already covers Mistlane's XML `.def` proposal, stable IDs, generated artifacts, controlled Lua properties, nested deltas, visible baselines and data-versioned migration goals; its "Verified BigWorld and KBEngine behavior" verified container-change deltas on both sides. This document keeps that conclusion and adds a comparison of both engines' actual dispatch flags, AOI, stream encoding, SQL mapping, write-back/caching and migration entry points. The original proposal's sproto, home data and revision/ownership generation cannot be read back into either engine as existing mechanisms.

Verification date: 2026-10-19. Version boundaries:

- **BW local source: unverified.** The task-designated `/home/cui/workspaces/kbengine/BigWorld-Engine-14.4.1/` directory was found empty on this pass (no source files), so no claim of having read a local BigWorld implementation can be made. The **BW-S** conclusions carried over from the earlier draft come from a public BigWorld 2.0.1 source mirror, commit `481e69a837a9f6d63f298a4f24d423b6329226c6`; it is not local source, not a maintained official distribution site, and some server-side `.cpp` files are missing. Code conclusions from it count only as static evidence of that mirror and are not extrapolated to local 14.4.1.
- **BW-G**: public archives of the Server Programming Guide / Server Operations Guide; the programming guide's front page says **BigWorld 2.1 (2012)**, not 14.4.1. Guide evidence and 2.0.1-mirror evidence are labeled separately, with no claim of version identity; that archive URL also does not substitute for the missing local source.
- **KB local source**: `/home/cui/workspaces/kbengine`, branch `master`, checked out this pass at commit `c9ca49ed848f597f3fe66cee831638b7043a5c62` (ahead 1 of `origin/master`, clean worktree). The body and the source index give reproducible local paths and line numbers; it is not another branch, not Nex and not a commercial derivative. The earlier draft's KB links remain pinned to the old commit `7d379b9f943d2731b09c45398855f98efe74d046`; this pass relies on local file line numbers.
- **Verified** means the cited local KB source path is statically traceable or the cited guide states it explicitly; BigWorld code conclusions are not part of this pass's local verification. Neither engine nor database integration tests were run. **Design analysis** is inference and does not impersonate author intent; statements lacking evidence are explicitly marked **unverified**.

Sources: [BW source mirror][bw-root], [vendor programming guide version page][bw-guide], [KBEngine source][kb-root].

**Reading limitation notice:** the text below keeps the earlier draft's conclusions supported by the BW-S mirror and BW-G archives and does not override the container-callback conclusions verified in `entity-contract.md`; but BigWorld code could not be re-checked in the designated local directory this time, as the task required. Any implementation detail supported only by BW-S and not confirmed by the guides must not be treated as a proven fact of 14.4.1.

## Item-by-item comparison table

Source codes in the table point to the pinned sources or vendor docs at the end; the five sections after it expand conditions and exceptions.

| Aspect/question | BigWorld | KBEngine | Source |
|---|---|---|---|
| 1. Contract entry | `scripts/entities.xml`, `entity_defs/*.def`; `alias.xml` defines type aliases | `scripts/entities.xml`, `entity_defs/*.def`, `types.xml`; loaded into ScriptDefModule / PropertyDescription | BW-E; KB-E, KB-T |
| 1. Types/defaults | `<Type>`, `<Default>`; DataType builds types and values | `<Type>`, `<Default>`; DataTypes registration and type factory | BW-D; KB-E, KB-P |
| 1. Dispatch flags | XML `BASE`, `OWN_CLIENT`, `ALL_CLIENTS` etc. map to `DATA_*` | XML ranges of the same names map to `ED_FLAG_*`, then form `ENTITY_BROADCAST_*` masks | BW-F; KB-F |
| 1. Persistence/editing | `<Persistent>` independent of dispatch; `<Editable>` differs from `EDITOR_ONLY` | `<Persistent>` an independent boolean; a generic `editor` tag unverified, `broadcast` expressed by range masks | BW-F; KB-E, KB-F |
| 2. Change trigger | Cell authoritative property assignment and controlled container changes trigger propagation; Base has exceptions | Entity property assignment entry calls `onDefDataChanged`; Base/Cell dispatch separately | BW-GP, BW-C; KB-M, KB-B, KB-C |
| 2. Own/others | OWN to the owner; OTHER to AOI observers; ALL to both | Corresponding OWN/OTHER masks; OTHER walks witnesses and checks actual view and distance level | BW-GP; KB-C |
| 2. Base to client | BW-G states `BASE_AND_CLIENT` syncs only at creation; later updates need method calls | KB-S's Base `onDefDataChanged` sends subsequent updates to bound clients | BW-GP; KB-B |
| 2. Delta/full | Initial visible data; then single properties, nested paths and slice changes | Initial visible property set; then full typed values of changed properties; in-place ordinary container changes cannot be assumed to notify | BW-C, BW-PC; KB-W, KB-C, KB-M |
| 2. AOI/filter | Cell/Witness manage interest range; property flags and LOD decide what may be sent | Cell ViewTrigger/Witness manage observation; dispatch re-checks view and property DetailLevel | BW-W; KB-W, KB-C |
| 3. Field mapping | `tbl_<Entity>`, `sm_` fields and sequence child tables; DataType → PropertyMapping | `tbl_<Entity>`, `sm_` fields; scalar/dict mappings and ARRAY/component child tables | BW-PM; KB-SQL |
| 3. DDL generation | `sync_db` reads definitions and generates/executes CREATE/ALTER/DROP; can launch a child tool from config | EntityTables sync at non-pure DB initialization, generating/executing DDL | BW-SYNC, BW-TS; KB-DBI, KB-SQL |
| 3. Write-back timing | Explicit `writeToDB`, automatic archive; secondary DB may batch commits then consolidate | Explicit `writeToDB`, periodic Archiver, destroy paths with save options | BW-DB, BW-CFG; KB-B, KB-A |
| 3. Cache/flush | Live-entity memory, per-entity DB task queue; optional SQLite secondary DB | Live entity/Cell data, persistence digest, per-entity DB task queue; not an independent persistent cache | BW-BUF, BW-CFG; KB-B, KB-Q |
| 4. Wire representation | DataType + BinaryOStream; property index/path encoding, dedicated formats for nesting and slices | DataType + MemoryStream; `onUpdatePropertys`, entity id and component/property UID or uint8 alias | BW-PC, BW-D; KB-C, KB-R, KB-AL |
| 4. "First byte" | BW-S shows no basis for naming property flags `UTYPE_` | Outer message ID is uint16; property alias may be uint8; a fixed `ENTITY_PROPERTY` first byte unverified | BW-F; KB-F, KB-N, KB-R |
| 5. Existing data changes | Tools align columns/tables; added-column default handling exists, complex semantics need project migration | Initialization sync can add/modify/drop columns and drop old tables; complex semantics still need project migration | BW-SYNC, BW-PM; KB-SQL, KB-TABLES |
| 5. Wire compatibility | Both ends depend on matching definitions/indexes; not self-describing XML messages | UID/alias and DataType depend on matching definitions; a definition digest is not data migration | BW-E, BW-PC; KB-AL, KB-H |

## 1. Contract definition: what is declared, who interprets it

### BigWorld

The entity list registers types; EntityDescription parses a `.def`'s properties, parents/interfaces and methods; DataDescription stores type, flags, default value and other metadata. DataType handles validation, defaults and serialization — network/database structure is not derived from the member list of arbitrary Python objects.

```xml
<root>
  <Properties>
    <hp>
      <Type>UINT16</Type>
      <Default>100</Default>
      <Flags>ALL_CLIENTS</Flags>
      <Persistent>false</Persistent>
    </hp>
  </Properties>
</root>
```

The XML range mapping can be checked directly in BW-S's `s_entityDataFlagMappings`:

- `BASE` → `DATA_BASE`; `BASE_AND_CLIENT` additionally `DATA_OWN_CLIENT`.
- `CELL_PRIVATE` → dispatch bits 0; `CELL_PUBLIC` → `DATA_GHOSTED`, letting ghosts on other Cells fetch it.
- `OWN_CLIENT` → `DATA_OWN_CLIENT`; `OTHER_CLIENTS` → `DATA_GHOSTED | DATA_OTHER_CLIENT`.
- `CELL_PUBLIC_AND_OWN` and `ALL_CLIENTS` combine the above bits; ALL's client set is bounded by AOI.
- `<Persistent>` adds the separate `DATA_PERSISTENT` bit; the default construction excludes it. `<Editable>` controls editor editability; `EDITOR_ONLY` / `DATA_EDITOR_ONLY` means editor-only data and must not be conflated.

Defaults can come from types/aliases and property declarations; fixed-length arrays must initialize to their length. `databaseID` is the database identity; the runtime `id` is unsuitable as a permanent reference.

**Regarding the task's `UTYPE_`: unverified as BigWorld property flags.** This pass's BW-S DataDescription uses `DATA_*`, not `UTYPE_*`; USER_TYPE (user types), type IDs, property indexes and KBEngine's Utype must not be conflated with dispatch bits. Whether other BW versions use that naming: unverified.

Sources: BW-S [EntityDescription][bw-e], [DataDescription parsing][bw-f], [flag definitions][bw-fh], [DataType][bw-d]; BW-G's [property declaration and defaults][bw-gp], [database identity][bw-db].

### KBEngine

EntityDef loads entities, types, interfaces/parents and properties; `loadDefPropertys` looks up `<Flags>` and `<Type>`, parses `<Persistent>`, `<Default>`, `<Utype>`, `<DatabaseLength>`, `<Index>` and `<DetailLevel>`, constructs PropertyDescription, and adds it to both the execution-side and persistence description sets.

The hp example above also matches this KBEngine XML structure, but that does not imply full `.def` or wire compatibility. KB-S errors on a property missing Flags/Type; Persistent defaults to false and defaults are created by the concrete DataType. `types.xml` supplies aliases, ARRAY, FIXED_DICT and other composite types.

- The dispatch constants are `ED_FLAG_BASE`, `ED_FLAG_BASE_AND_CLIENT`, `ED_FLAG_CELL_PRIVATE`, `ED_FLAG_CELL_PUBLIC`, `ED_FLAG_OWN_CLIENT`, `ED_FLAG_OTHER_CLIENTS`, `ED_FLAG_ALL_CLIENTS`, `ED_FLAG_CELL_PUBLIC_AND_OWN`.
- `ENTITY_BROADCAST_CELL_FLAGS`, `ENTITY_BROADCAST_OWN_CLIENT_FLAGS`, `ENTITY_BROADCAST_OTHER_CLIENT_FLAGS` are derived masks of those ranges; **there is no evidence they should be written as `<broadcast>true</broadcast>`**.
- persistent is a boolean policy on the description object, not something business code ORs into the range enum.
- **A generic `editor` / `<Editor>` property marker is unverified**: the inspected `loadDefPropertys` and PropertyDescription show no corresponding reads/members; BigWorld's Editable must not be projected onto KBEngine. No claim is made here about other branches.
- Utype is the property identifier; KB-S's `ENTITY_PROPERTY_UID` is uint16. It may be set explicitly or assigned by the engine; it carries no persistent/editor/broadcast boolean semantics.

Sources: KB-S [EntityDef loading][kb-e], [type registration][kb-t], [PropertyDescription][kb-p], [flags and UID][kb-f]; [official definition format][kb-defdoc].

## 2. Property synchronization: from one assignment to a visible replica

### BigWorld

After an authoritative Cell property changes, the engine propagates to ghosts, the owner and observers per the definitions. OWN_CLIENT goes only to the owner; OTHER_CLIENTS to clients observing the entity; ALL_CLIENTS is the union, not a server-wide broadcast. Cell's spatial interest management and Witness decide who observes, and property LOD reduces visible detail at distance.

The exception BW-G states explicitly is **BASE_AND_CLIENT initializes only when the client entity is created; later Base changes do not push automatically and need method calls**. One cannot promise every assignment auto-syncs just because DATA_OWN_CLIENT appears in the flags.

Initial replica creation needs initialization data of the visible properties; later ordinary properties send on change; a top-level replacement sends that property's value, and in-container changes can be expressed as path, single-item and slice operations. BW-S has PropertyOwner, PropertyChange, SinglePropertyChange, SlicePropertyChange; BW-G has `set_<property>`, `setNested_<property>`, `setSlice_<property>` callbacks. **Keeping the earlier verified conclusion: BW supports more than top-level assignment.**

The logical unit is the property-change event, not resending the whole entity each time; transport bundles/packets can carry many messages, and one property write must not be equated with one UDP packet. This document does not promise every 2.0.1 build coalesces duplicate writes in a tick; SendLatestOnly/IsReliable in the 2.1 guide are versioned options and cannot support a claim that only the latest value is sent by default. The concrete cache-reuse/event-replay policy for leaving and re-entering AOI was not verified item by item this pass, so no claim of always-full-rebuild either.

Sources: BW-G [dispatch, LOD, propagation and bandwidth options][bw-gp], [client property callbacks][bw-callback], [Witness][bw-w]; BW-S [PropertyOwner][bw-po], [PropertyChange encoding][bw-pc].

### KBEngine

KB-S's traceable chain:

1. The property-assignment branch in `entity_macro.h` checks definition/type and calls the Entity's `onDefDataChanged`.
2. Base skips notification during initialization; persistent properties are marked dirty; if BASE_AND_CLIENT and a client is bound, it builds `onUpdatePropertys` and sends. **This differs from the BW-G Base exception above.**
3. Cell dispatches only for real entities outside initialization; by mask it updates ghosts, other clients and the owner's client.
4. The OTHER branch walks `witnesses_`; it checks the observer, its connection and `entityInView(id)`, then filters by the property's DetailLevel distance; OWN is sent separately.
5. Values are encoded by `propertyDescription->getDataType()->addToStream`. The logical payload of one assignment notification is that property's typed value; bundles can accumulate messages, and one cannot assume only the last assignment per tick survives.

When Witness handles `ENTER_CLIENT_PENDING` it calls `addPositionAndDirectionToStream` and `addClientDataToStream(..., true)`, sending initialization properties first and enter-world after; leaving has a leave-pending path. The "full set" here is **the property set this receiver is allowed to see** — not private fields and not the whole database record.

**The container limitation keeps the original conclusion**: in-place element/slice modifications of ordinary FixedDicts and Sequences showed no owner-entity notification in the inspected paths; automatic network deltas must not be assumed. Top-level reassignment goes through property notification and encodes the container value. EntityComponent has its own notification mechanism — not proof of arbitrary-depth container path deltas. Re-serialization at save time may read the modified container, but does not prove the modification synced live.

The assignment entry, baseline and filtering paths are verified; cross-disconnect recovery, catch-up for all LOD switches, and a full recovery state machine for mixed client versions are **unverified** and must not be padded into the sequence-gap/rebaseline protocol of the Mistlane proposal.

Sources: KB-S [assignment entry][kb-m], [Base dispatch][kb-b], [Cell dispatch][kb-c], [Witness init/leave][kb-w], [FixedDict][kb-fd], [Sequence][kb-seq], [EntityComponent][kb-comp].

## 3. Contract persistence: from definition to SQL to write-back

### BigWorld

Loaded EntityDescription/DataDescription also drive the database mapping. The persistence stream filters through `ONLY_PERSISTENT_DATA`; data domains like base/client and cell/client have fixed traversal organization — XML or Python object memory is not written to the database directly. Built-in position/direction save rules also need engine-specific handling, not just an enumeration of business fields.

The MySQL backend's PropertyMapping builds column/child-table mappings by DataType: the main table `tbl_<Entity>` with the database ID in `id`; scalars generally map to main-table columns, vector components and FIXED_DICT fields expand, ARRAY/TUPLE use child tables joined to the parent record. String/binary/user types have dedicated mappings — one must not summarize "all types are the same kind of BLOB". On load the corresponding mapping rebuilds typed values; non-persistent properties re-initialize.

`sync_db`'s MySqlSynchronise / MySqlTableInitialiser compares definitions with the actual structure and generates/executes CREATE, ALTER, DROP, including added/dropped/modified columns. **What is verified is that the tool generates and executes SQL — not that every build necessarily emits a reviewable `.sql` file.** The `syncTablesToDefs` start option can launch a separate tool via TableSynchroniser; production can disable automatic structure modification and maintain the database first.

Writes are managed by Base's `writeToDB`, gathering Cell state when needed; explicit writes are asynchronous with a completion notification. Automatic archive schedules by `baseApp/archivePeriod`, not an immediate UPDATE per assignment. Whether destroy saves depends on options/lifecycle policy; an arbitrary destroy is not a forced flush.

Cache and flush separate into three layers:

- Live entities' in-memory values are the online state; the Base save path extracts what should persist.
- MySQL background tasks buffer/serialize by the same entity identity, preventing same-entity task reordering; the queue is not a persistent log nor a business-queryable cache database.
- With secondary DB enabled, BaseApp writes a local SQLite; automatic archive can batch commits by `secondaryDB/maxCommitPeriod`, and explicit writeToDB triggers a commit. Later `consolidate_dbs` merges into the primary database on shutdown/crash recovery flows. **SQLite-committed and primary-MySQL-updated are two different moments**; before consolidation, backing up only the primary and assuming it holds the newest state is wrong.

Without secondary DB, DBMgr/the MySQL backend performs the writes; database commit/disk reliability still depends on database configuration. Network delivery, in-memory backup, SQLite commit and primary commit must not share the vague promise of "saved".

Sources: BW-S [persistence filtering][bw-e], [PropertyMapping][bw-pm], [DDL sync][bw-sync], [sync child tool launch][bw-ts], [per-entity queue][bw-buf], [Base save interface][bw-base]; BW-G [database mapping/writes/secondary DB][bw-db], [archive/SQLite commit config][bw-cfg], [consolidation and sync_db operations][bw-ops].

### KBEngine

At startup DBMgr initializes EntityDef; DBUtil's database initialization loads EntityTables, and a non-pure database calls `syncToDB`. EntityTableMysql builds TableItems from persistent property descriptions: basic numerics to numeric columns, vectors expand per component, FIXED_DICT organizes fields recursively, ARRAY/EntityComponent create child tables. Table/field prefixes are `tbl_` and `sm_`, and child-table records carry a parent ID. DatabaseLength/Index affect length and indexing for applicable types; not every DataType is indiscriminately indexable.

`sync_item_to_db` / `EntityTableMysql::syncToDB` generate and execute SQL, and EntityTables aggregates child-table sync; likewise **no offline SQL script file is verified as generated by default**. The risk of startup DDL drops and type changes is covered in section 5.

The runtime write-back path:

1. Base's explicit `writeToDB`, or the Archiver spreading entities with existing DB records and auto-save allowed across ticks by `archivePeriod`; `destroy` with a write-back option also enters the save path.
2. With a Cell present it first requests `reqWriteToDBFromBaseapp`, completing into `onCellWriteToDBCompleted`; the save hooks include `onPreArchive` / `onWriteToDB`.
3. `addPersistentsDataToStream` builds the persistent-property stream; Base compares the full stream's SHA1 digest and skips the actual DB write when unchanged. It is not a network-path delta, nor a per-property SQL UPDATE log.
4. The changed stream goes to DBMgr's `writeEntity`; Buffered_DBTasks queue by DBID or the new entity's runtime ID, and DBTaskWriteEntity calls EntityTables / EntityTableMysql, decoding to column values and executing INSERT/UPDATE plus child-table operations.
5. When the DB task returns, `onWriteToDBCallback` fires, updating the database identity and invoking the callback. Returning from the called function or enqueuing a message must not be taken as database success.

Caches include online entities, Cell data in Base, the persistence digest and queued tasks. In the source the digest updates before submitting the DB task, and there is a "digest unchanged → return" path, **so the digest must not be called the last successfully committed revision, nor can every writeToDB be promised to trigger a database operation/callback**. Failure retries, new changes during save and callback completeness need dedicated verification.

No general mechanism equivalent to BW's secondary SQLite → MySQL consolidation was found this pass; **that KBEngine has an independent property-persistence cache and a callable flush API is unverified**. The verifiable meaning of flush here is only: submit a selected in-memory snapshot as an async DB task and wait for the result. MySQL's transactions, fsync and cross-entity atomicity/crash durability are not guaranteed by the property-sync mechanism.

Sources: KB-S [DBMgr init/write entry][kb-db], [database initialization][kb-dbi], [EntityTables][kb-tables], [MySQL fields and writes][kb-sql], [Base save and digest][kb-b], [Archiver][kb-a], [queueing][kb-q], [DBTaskWriteEntity][kb-task].

## 4. Wire representation: typed values, property identity and the outer message

### BigWorld

`DataType::addToStream` / `createFromStream`, together with DataDescription/EntityDescription, control binary read/write; numerics follow declared widths, containers follow type structure, and receivers need identical definitions. Initialization/persistence filter by their data domains — the database stream and the observer-facing stream must not be assumed byte-identical.

BW-S's PropertyChange encoding distinguishes simple top-level properties, extended single items and slices: low-numbered top-level properties can use a message-identifier encoding; complex paths write path indexes/continuation bits through a BitWriter, while actual values still encode via DataType. The "delta" here mainly means transmitting only changed fields/container operations — not universally sending `newValue - oldValue`. Definition indexes, paths and message framing together form the format; it cannot be abstracted into a uniform `{name, JSON value}`.

Sources: BW-S [DataType][bw-d], [EntityDescription streams][bw-e], [PropertyChange][bw-pc], [reader][bw-reader].

### KBEngine, and the `ENTITY_PROPERTY` first-byte claim

KB-S's verified representation has three layers:

```text
outer:   message ID:uint16 + [variable-length message size] + message body
ordinary property update body: entityID + { componentPropertyID, propertyID, typedValue }...
optimized property update body: in-view entity alias:uint8 + the same property records...
property ID: uint8 alias when enabled and capacity allows; otherwise uint16 UID
ordinary top-level properties have componentPropertyID 0; component properties identify their component
```

`onUpdatePropertys` and `onUpdatePropertysOptimized` are the current update messages; a Client Entity reads the entity identity, then loops reading component/property identifiers, looks up PropertyDescription, and decodes values via DataType. ARRAY and FIXED_DICT encode via their type serializers; the full structure must not be inferred from some fixed type byte in this message. Whether aliases are used follows ScriptDefModule's assignment; client and server must agree.

**"KBEngine uses ENTITY_PROPERTY as the first byte of a network packet" is unverified.** In the checked-out implementation no client property packet path supporting that fixed marker was found; the outer layer reads a uint16 message ID first, and in alias mode the first uint8 of a property record may instead be a component identifier of 0 — not a property type enum. `ENTITY_PROPERTY_UID` is a uint16 typedef, not a fixed first byte. If the claim refers to an older SDK or a custom transport protocol, the version and implementation must be supplied; this document does not guess values or semantics.

Sources: KB-S [Base encoding][kb-b], [Cell encoding][kb-c], [client reading][kb-r], [UID definition][kb-f], [alias assignment][kb-al], [DataType][kb-d]; [official message framing][kb-netdoc].

## 5. Version compatibility: structural sync is not business migration

### BigWorld's existing data

`sync_db` compares entity definitions with the database structure and can add columns, drop redundant columns/tables, and modify types and indexes; BW-S's MySqlTableInitialiser includes ADD/DROP/MODIFY plus added-column default handling. The operations guide notes sync_db must not run while an unconsolidated secondary DB remains: old secondary data is still bound by old definitions, so migrating only the primary is not enough.

One cannot promise "adding a property gives all old data the same default as new entities". BW-S's PropertyMapping records a concrete counterexample: when adding an ARRAY/TUPLE, existing database entities default to no elements while newly created entities use the declared array default; new FIXED_DICT fields inside an ARRAY use the subtype default. Column defaults, missing runtime values and backfill of existing records are three different problems.

Dropping fields can lose data; renames cannot be auto-recognized from type equality alone; narrowing integers, changing string length/encoding, and scalar-to-structure conversions need business decisions. **Lossless automatic conversion of arbitrary changes, online mixed-version read/write and automatic rollback are unverified.** DDL can succeed while numeric units, ID meanings and business constraints are still wrong.

Sources: BW-S [column/table sync and defaults][bw-sync], [array-default exception][bw-pm]; BW-G [sync_db and consolidation][bw-ops].

### KBEngine's existing data

KB-S's initialization for a non-pure database syncs the contract structure:

- Missing columns generate ADD; existing columns with mismatched type/length go through the duplicate-column branch into MODIFY COLUMN.
- EntityTableMysql walks database columns and drops ones no longer recognized by the current TableItems; EntityTables also cleans entity tables absent from the mapping. So deleting/renaming properties or removing the persistent marker can reach destructive paths — history columns are not silently kept.
- Numeric TableItems' DDL can carry declared defaults; **not all types share the same SQL DEFAULT rule**. Changing only `Default` also cannot be assumed to change existing rows: in particular `sync_item_to_db`'s quick comparison checks type/flags/length but not default, returning directly on match.

Conversion effects depend on MySQL's rules and configuration; the source's MODIFY contains no business conversion functions. Renames, splitting/merging fields, backfilling old array elements, changing currency units and rollback still need project migration scripts and verification. A stable `Utype` does not prevent name-based SQL column changes.

The network side uses shared definitions, alias maps and definition digests; DBMgr/client digest checks help detect mismatches but **cannot convert already-stored data**. Both engines' "online entity hot-reload automatically keeps old data semantics" and "arbitrary old/new client interop" are unverified; do not conflate them with the version-routing/migration goals in the existing Mistlane draft.

Sources: KB-S [initialization conditions][kb-dbi], [column sync/default comparison][kb-sql], [old-table cleanup][kb-tables], [aliases][kb-al], [digest handshake][kb-h].

### Why migration is complex, and a suggested release strategy (design analysis)

One property simultaneously has a **network identity, runtime type, database mapping and business meaning**. Changing any one can affect client decoding, online objects, old records, backups and rollback procedures; automatic DDL only knows structural differences and cannot derive "gold changed to cents, multiply by 100" or "this drop is actually a rename".

BW leans on a separate structure-sync tool with preconditions around secondary-DB consolidation; KB-S puts structure alignment for non-pure DBs on the initialization path — convenient for development, larger startup blast radius. Neither can let `.def` alone replace business migration design.

Suggested release practice: back up and verify recovery first, freeze/drain old writers, and handle unfinished persistence; review DDL and business conversions on a copy of the old data, then deploy the matching contract/clients. For rolling releases use add field → dual read/controlled dual write → backfill verification → switch reads → deferred drop; this is an additional engineering scheme, not a verified default of either engine. Rollback for destructive changes must plan data recovery together — swapping back the old `.def` alone is not enough.

Basis: both sides' sync, default and old-table cleanup sources above; the existing [Mistlane persistence and compatibility proposal](./entity-contract.md#loading-and-persistence) already requires explicit migration, and this document does not change that choice.

## Six design rationales and trade-offs (design analysis)

The following derives from the mechanisms listed; apart from explicitly quoted guide statements, no claim of author intent is made.

1. **Declarative contract vs hard-coded properties.** One type model serves validation, client description/generation, dispatch and database mapping, avoiding separately maintained structures in Python/Lua, network and SQL. The cost: parsers, descriptors, generation tools and ID/digest compatibility become infrastructure; dynamic business-code changes no longer mean freely changing the data layout. A few purely local temporaries need not enter the contract; state that crosses boundaries or needs recovery should be declared. Basis: BW-E/BW-D and KB-E/KB-P.

2. **Persistent vs sync-only.** They are independent dimensions, not two mutually exclusive classes: "persist and sync", "persist but private", "sync but not persisted", "neither" are all valid. Gold needs reliable persistence but is usually owner-visible only; animations/temporary combat effects can sync without surviving restarts. Separation reduces DB write amplification and privacy exposure while preventing a persistence choice from accidentally changing visibility. Non-persistent state may still need process-migration/disaster-recovery transfer; database-recovered values rebuild by default rules. Basis: BW-F/BW-E and KB-F/KB-B.

3. **Delta vs full snapshot.** First sight of an entity uses a visible full set to establish a consistent initial state; subsequent per-property/nested deltas save bandwidth, especially for single-item changes in large containers. The cost: tracking all modifications, maintaining order/baselines, handling delete/replace and loss recovery; full replacement is simpler to implement and possibly cheaper for small containers. BW has paths/slices; KB's ordinary assignment granularity is the full property value — not a full entity snapshot. Client sync and DB snapshots may use different granularities, and payments/events must not depend on state-callback counts. Basis: BW-PC/BW-C and KB-C/KB-W.

4. **Server authority vs client prediction.** Base/real Cell decide ordinary persistent/synced properties; clients may predict movement and animation locally to cut input latency, but a local assignment of gold, drops or HP must not become a trusted write. Clients submit actions; the server validates and updates authoritative values; presentation then corrects the prediction. The engine has dedicated paths for controlled entity movement — not proof that any property can be client-written; specific projects' prediction/rollback algorithms are unverified. The cost is correction jumps and business-validation effort; the benefit is shared state and assets not depending on untrusted replicas. Basis: BW-G's [Entity Control/Physics Correction][bw-w]; KB-C and the client's [local vs authoritative position handling][kb-r].

5. **Where AOI lives.** Spatial interest should be established in the server Cell's spatial/Witness layer; the property-dispatch layer intersects it with OWN/OTHER, permissions and DetailLevel, then sends at the egress. Sending everything and letting the client filter is wrong — bandwidth is already spent and private information already leaked; nor should the DataType encoder or the DB layer compute spatial visibility. The cost: cross-Cell ghosts, observer join/leave, baselines and bandwidth scheduling must coordinate; AOI is a candidate set, not automatically faction/stealth business authorization. Basis: BW-W/BW-F and KB-W/KB-C.

6. **Why contract migration is complex.** Columns, field IDs, container layout, defaults and business meaning are not one dimension; persisted data outlives a process or client version. BW's tooling DDL/secondary consolidation and KB's startup DDL each solve structural alignment; neither proves business-lossless, rolling interop or reversibility. Choosing auto-alignment reduces development friction; choosing explicit migration makes data loss, conversion and rollback reviewable at a deployment-orchestration cost. Basis: section 5's BW-SYNC/BW-PM and KB-SQL/KB-TABLES.

## Source index and re-check entry points

Codes are used in the table above and the design analysis. BW-S links pin a public mirror commit and BW-G is an archived guide; neither is the missing local BigWorld 14.4.1 source. KB entries list both the archived link and this pass's local source `path:line` (paths relative to `/home/cui/workspaces/kbengine`); those line numbers are the basis of this pass's reading. Unverified items concentrate on the local BigWorld implementation, naming differences, parts of recovery/prediction flows, arbitrary migration guarantees and un-run failure scenarios, and must not be read as feature promises.

- **BW-E**: [EntityDescription: loading, data domains and streams][bw-e].
- **BW-D**: [DataType: type and stream interfaces][bw-d].
- **BW-F**: [DataDescription: XML flags/default parsing; bit definitions in the same directory's .hpp][bw-f].
- **BW-GP**: [2.1 guide: Properties][bw-gp].
- **BW-C**: [2.1 guide: client property callbacks in Methods][bw-callback].
- **BW-PC**: [PropertyChange: path and slice encoding][bw-pc].
- **BW-W**: [2.1 guide: Proxies and Players, Witness/control/correction][bw-w].
- **BW-PM**: [MySQL PropertyMapping: column/container mapping and array defaults][bw-pm].
- **BW-SYNC**: [MySqlTableInitialiser: DDL and defaults][bw-sync].
- **BW-TS**: [TableSynchroniser: launch-time sync_db][bw-ts].
- **BW-DB**: [2.1 guide: The Database Layer][bw-db].
- **BW-CFG**: [operations guide: archive/secondaryDB configuration][bw-cfg].
- **BW-BUF**: [BufferedEntityTasks: same-entity task ordering][bw-buf].
- **KB-E**: [EntityDef: contract loading][kb-e]; local `kbe/src/lib/entitydef/entitydef.cpp:933-934,1097-1297`.
- **KB-T**: [DataTypes: type registration][kb-t]; local `kbe/src/lib/entitydef/datatypes.cpp:94-188`.
- **KB-P**: [PropertyDescription: typed values and persistence serialization][kb-p]; local `kbe/src/lib/entitydef/property.h:23-64,75-93,137-163`, `kbe/src/lib/entitydef/property.cpp:22-64,82-129`.
- **KB-F**: [EntityDataFlags, broadcast ranges and UID][kb-f]; local `kbe/src/lib/entitydef/common.h:16-55,79-84`, `kbe/src/lib/entitydef/entitydef.cpp:161-174,1018-1092`, `kbe/src/lib/entitydef/scriptdef_module.cpp:113-145`.
- **KB-M**: [entity property assignment entry][kb-m]; local `kbe/src/lib/entitydef/entity_macro.h:790-820`. Component property assignment callback entry: `kbe/src/lib/entitydef/entity_component.cpp:414-488`.
- **KB-B**: [Base: dispatch, write-back, digest and callback][kb-b]; local `kbe/src/server/baseapp/entity.cpp:96-160,398-505,1141-1210,1233-1305,1319-1384`.
- **KB-C**: [Cell: real/ghost, AOI and property delivery][kb-c]; local `kbe/src/server/cellapp/entity.cpp:658-892,1090-1113`. LOD-filtered initialization properties: `kbe/src/lib/entitydef/entity_macro.h:583-612`; client-visible property initialization: `:614-655`.
- **KB-W**: [Witness: view state and initialization][kb-w]; local `kbe/src/server/cellapp/witness.cpp:782-835`; view enter/leave triggers: `kbe/src/server/cellapp/view_trigger.cpp:27-51`.
- **KB-CONTAINERS**: local `kbe/src/lib/entitydef/fixeddict.cpp:272-309` (FixedDict element operations only validate and mutate the inner dict); `kbe/src/lib/pyscript/sequence.cpp:18-30,252-285,295-382,395-445` (Sequence element/slice/concat mutate the container directly). No owner-entity `onDefDataChanged` call was seen on these paths; this is a static check of the listed paths, not of all container wrappers or component callbacks.
- **KB-SQL**: [EntityTableMysql: DDL/fields/child tables/writes][kb-sql]; local `kbe/src/lib/db_mysql/entity_table_mysql.cpp:136-230,370-470,505-763,1212-1298,1302-1354`.
- **KB-TABLES**: [EntityTables: load, sync and old-table cleanup][kb-tables]; local `kbe/src/lib/db_interface/entity_table.cpp:117-143,169-235,296-320`.
- **KB-DBI**: [DBUtil: non-pure initialization calls syncToDB][kb-dbi]; local `kbe/src/lib/db_interface/db_interface.cpp:221-247`; MySQL table factory `kbe/src/lib/db_mysql/db_interface_mysql.cpp:419-423`, drop implementation `:426-451`.
- **KB-A**: [periodic archive scheduling][kb-a]; local `kbe/src/server/baseapp/archiver.cpp:23-63,66-85`.
- **KB-Q**: [per-entity DB task queue][kb-q]; local `kbe/src/server/dbmgr/buffered_dbtasks.cpp:26-85,89-153`.
- **KB-R**: [client property parsing][kb-r]; local `kbe/src/lib/client_lib/entity.cpp:259-424`.
- **KB-AL**: [property alias assignment and capacity limit][kb-al]; local `kbe/src/lib/entitydef/scriptdef_module.cpp:113-180`.
- **KB-N**: [official message framing format][kb-netdoc]; this document was not re-checked in local source this pass; client update parsing and Cell/Base packing are at `kbe/src/lib/client_lib/entity.cpp:293-315`, `kbe/src/server/cellapp/entity.cpp:750-781`, `kbe/src/server/baseapp/entity.cpp:131-159`; message declarations `kbe/src/lib/client_lib/client_interface.h:89-90`.
- **KB-H**: [BaseApp: handshake sends the EntityDef digest][kb-h]; the handshake-digest path was not re-checked in local source this pass; the digest must not be equated with data migration.
- **KB-DBTASK**: local DB write thread and main-thread callback `kbe/src/server/dbmgr/dbtasks.cpp:328-397`; entity table creation `kbe/src/lib/db_mysql/db_interface_mysql.cpp:419-423`.
- **KB-ASSIGN**: local `<Flags>/<Persistent>/<Type>/<Default>/<Utype>` reading and descriptor creation `kbe/src/lib/entitydef/entitydef.cpp:1097-1297`; serialization/delegation `kbe/src/lib/entitydef/property.cpp:82-129`.
- **KB-DATA**: local value wire encoding goes through the DataType `addToStream` interface, `kbe/src/lib/entitydef/property.cpp:82-108`; concrete type implementations are spread across `kbe/src/lib/entitydef/datatype.cpp`.

[bw-root]: https://github.com/v2v3v4/BigWorld-Engine-2.0.1/blob/481e69a837a9f6d63f298a4f24d423b6329226c6
[kb-root]: https://github.com/kbengine/kbengine/blob/7d379b9f943d2731b09c45398855f98efe74d046
[bw-guide]: https://howarduong.github.io/github.io/doc/html/server_programming_guide/index.html
[bw-e]: https://github.com/v2v3v4/BigWorld-Engine-2.0.1/blob/481e69a837a9f6d63f298a4f24d423b6329226c6/src/lib/entitydef/entity_description.cpp
[bw-f]: https://github.com/v2v3v4/BigWorld-Engine-2.0.1/blob/481e69a837a9f6d63f298a4f24d423b6329226c6/src/lib/entitydef/data_description.cpp
[bw-fh]: https://github.com/v2v3v4/BigWorld-Engine-2.0.1/blob/481e69a837a9f6d63f298a4f24d423b6329226c6/src/lib/entitydef/data_description.hpp
[bw-d]: https://github.com/v2v3v4/BigWorld-Engine-2.0.1/blob/481e69a837a9f6d63f298a4f24d423b6329226c6/src/lib/entitydef/data_type.hpp
[bw-pc]: https://github.com/v2v3v4/BigWorld-Engine-2.0.1/blob/481e69a837a9f6d63f298a4f24d423b6329226c6/src/lib/entitydef/property_change.cpp
[bw-reader]: https://github.com/v2v3v4/BigWorld-Engine-2.0.1/blob/481e69a837a9f6d63f298a4f24d423b6329226c6/src/lib/entitydef/property_change_reader.cpp
[bw-po]: https://github.com/v2v3v4/BigWorld-Engine-2.0.1/blob/481e69a837a9f6d63f298a4f24d423b6329226c6/src/lib/entitydef/property_owner.cpp
[bw-pm]: https://github.com/v2v3v4/BigWorld-Engine-2.0.1/blob/481e69a837a9f6d63f298a4f24d423b6329226c6/src/lib/dbmgr_mysql/mappings/property_mapping.cpp
[bw-sync]: https://github.com/v2v3v4/BigWorld-Engine-2.0.1/blob/481e69a837a9f6d63f298a4f24d423b6329226c6/src/server/tools/sync_db/mysql_table_initialiser.cpp
[bw-ts]: https://github.com/v2v3v4/BigWorld-Engine-2.0.1/blob/481e69a837a9f6d63f298a4f24d423b6329226c6/src/lib/dbmgr_mysql/table_synchroniser.cpp
[bw-buf]: https://github.com/v2v3v4/BigWorld-Engine-2.0.1/blob/481e69a837a9f6d63f298a4f24d423b6329226c6/src/lib/dbmgr_mysql/buffered_entity_tasks.cpp
[bw-base]: https://github.com/v2v3v4/BigWorld-Engine-2.0.1/blob/481e69a837a9f6d63f298a4f24d423b6329226c6/src/server/baseapp/base.hpp
[bw-gp]: https://howarduong.github.io/github.io/doc/html/server_programming_guide/ch05.html
[bw-callback]: https://howarduong.github.io/github.io/doc/html/server_programming_guide/ch06.html
[bw-db]: https://howarduong.github.io/github.io/doc/html/server_programming_guide/ch09.html
[bw-w]: https://howarduong.github.io/github.io/doc/html/server_programming_guide/ch12.html
[bw-cfg]: https://howarduong.github.io/github.io/doc/html/server_operations_guide/ch02.html
[bw-ops]: https://howarduong.github.io/github.io/doc/html/server_operations_guide/server_operations_guide.html
[kb-e]: https://github.com/kbengine/kbengine/blob/7d379b9f943d2731b09c45398855f98efe74d046/kbe/src/lib/entitydef/entitydef.cpp
[kb-t]: https://github.com/kbengine/kbengine/blob/7d379b9f943d2731b09c45398855f98efe74d046/kbe/src/lib/entitydef/datatypes.cpp
[kb-p]: https://github.com/kbengine/kbengine/blob/7d379b9f943d2731b09c45398855f98efe74d046/kbe/src/lib/entitydef/property.cpp
[kb-f]: https://github.com/kbengine/kbengine/blob/7d379b9f943d2731b09c45398855f98efe74d046/kbe/src/lib/entitydef/common.h
[kb-m]: https://github.com/kbengine/kbengine/blob/7d379b9f943d2731b09c45398855f98efe74d046/kbe/src/lib/entitydef/entity_macro.h
[kb-b]: https://github.com/kbengine/kbengine/blob/7d379b9f943d2731b09c45398855f98efe74d046/kbe/src/server/baseapp/entity.cpp
[kb-c]: https://github.com/kbengine/kbengine/blob/7d379b9f943d2731b09c45398855f98efe74d046/kbe/src/server/cellapp/entity.cpp
[kb-w]: https://github.com/kbengine/kbengine/blob/7d379b9f943d2731b09c45398855f98efe74d046/kbe/src/server/cellapp/witness.cpp
[kb-fd]: https://github.com/kbengine/kbengine/blob/7d379b9f943d2731b09c45398855f98efe74d046/kbe/src/lib/entitydef/fixeddict.cpp
[kb-seq]: https://github.com/kbengine/kbengine/blob/7d379b9f943d2731b09c45398855f98efe74d046/kbe/src/lib/pyscript/sequence.cpp
[kb-comp]: https://github.com/kbengine/kbengine/blob/7d379b9f943d2731b09c45398855f98efe74d046/kbe/src/lib/entitydef/entity_component.cpp
[kb-dbi]: https://github.com/kbengine/kbengine/blob/7d379b9f943d2731b09c45398855f98efe74d046/kbe/src/lib/db_interface/db_interface.cpp
[kb-db]: https://github.com/kbengine/kbengine/blob/7d379b9f943d2731b09c45398855f98efe74d046/kbe/src/server/dbmgr/dbmgr.cpp
[kb-tables]: https://github.com/kbengine/kbengine/blob/7d379b9f943d2731b09c45398855f98efe74d046/kbe/src/lib/db_interface/entity_table.cpp
[kb-sql]: https://github.com/kbengine/kbengine/blob/7d379b9f943d2731b09c45398855f98efe74d046/kbe/src/lib/db_mysql/entity_table_mysql.cpp
[kb-a]: https://github.com/kbengine/kbengine/blob/7d379b9f943d2731b09c45398855f98efe74d046/kbe/src/server/baseapp/archiver.cpp
[kb-q]: https://github.com/kbengine/kbengine/blob/7d379b9f943d2731b09c45398855f98efe74d046/kbe/src/server/dbmgr/buffered_dbtasks.cpp
[kb-task]: https://github.com/kbengine/kbengine/blob/7d379b9f943d2731b09c45398855f98efe74d046/kbe/src/server/dbmgr/dbtasks.cpp
[kb-r]: https://github.com/kbengine/kbengine/blob/7d379b9f943d2731b09c45398855f98efe74d046/kbe/src/lib/client_lib/entity.cpp
[kb-al]: https://github.com/kbengine/kbengine/blob/7d379b9f943d2731b09c45398855f98efe74d046/kbe/src/lib/entitydef/scriptdef_module.cpp
[kb-d]: https://github.com/kbengine/kbengine/blob/7d379b9f943d2731b09c45398855f98efe74d046/kbe/src/lib/entitydef/datatype.cpp
[kb-h]: https://github.com/kbengine/kbengine/blob/7d379b9f943d2731b09c45398855f98efe74d046/kbe/src/server/baseapp/baseapp.cpp
[kb-defdoc]: https://kbengine.github.io/docs/programming/entitydef.html
[kb-netdoc]: https://kbengine.github.io/docs/programming/kbe_message_format.html
