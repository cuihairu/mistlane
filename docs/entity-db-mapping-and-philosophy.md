English | [简体中文](zh/entity-db-mapping-and-philosophy.md)
# KBEngine entity property → database mapping and engine design trade-offs

> Scope note: the local checkout at `/home/cui/workspaces/kbengine/BigWorld-Engine-14.4.1/` is empty; `.gitmodules:1-3` marks it as a submodule, but the source has not been fetched. This document therefore does not pass KBEngine's implementation off as line-by-line evidence for BigWorld 14.4.1. The mechanism conclusions below are grounded in the readable KBEngine source; BigWorld-specific differences are marked "unverified". Source citations use `path:line`.

## Chapter 1: property → DB storage mapping

### 1.1 The mapping is driven by static entity definitions, not runtime reflection

A `.def` property's `Flags`, `Persistent`, `Type`, `Index`, `DatabaseLength` and `Default` are parsed in `EntityDef::loadDefPropertys()`, turned into `PropertyDescription` objects, and placed into the corresponding module descriptions according to Base/Cell/Client flags; `ScriptDefModule::addPropertyDescription()` additionally stores persistent descriptions in a persistent-property table. See `kbe/src/lib/entitydef/entitydef.cpp:1097-1297`, `kbe/src/lib/entitydef/scriptdef_module.cpp:588-616`.

At DB initialization, `EntityTables::load()` walks every `ScriptDefModule` in `EntityDef`, calls the backend's `createEntityTable()` and initializes the table with the module name; entities without archived properties get no entity table. The MySQL factory returns `EntityTableMysql`. See `kbe/src/lib/db_interface/entity_table.cpp:117-143`, `kbe/src/lib/db_mysql/db_interface_mysql.cpp:420-423`.

### 1.2 Table names, column names and concrete types

A MySQL entity table is named `tbl_<EntityModuleName>`; the table has at least the auto-increment primary key `id`. Ordinary property columns are named `sm_<propertyName>` (fixed prefix in `entity_table.h:37-44`, column assembly in `entity_table_mysql.cpp:938-941`). MySQL table initialization walks the persistent property descriptions; only persistent properties become DB items. See `kbe/src/lib/db_mysql/entity_table_mysql.cpp:136-170`, `370-425`.

Common MySQL mappings (note `DatabaseLength` is a schema length setting, not a runtime variable-length model for the property value):

| Entity type | MySQL storage | Notes |
|---|---|---|
| `INT8/16/32/64` | `tinyint/smallint/int/bigint` | Signed integers; defaults come from the definition, otherwise 0.|
| `UINT8/16/32/64` | the matching integer type with `unsigned` | Unsigned integers.|
| `FLOAT/DOUBLE` | `float/double` | Numbers serialize by their scalar type and convert to SQL numerics.|
| `STRING/UNICODE` | `varchar(n)` | Defaults to 255 when `DatabaseLength` is unset; strings are escaped by the connection encoding.|
| `BLOB`, `PYTHON`, `PY_DICT`, `PY_TUPLE`, `PY_LIST`, `ENTITYCALL` | `blob` | These are not relational columns queryable by inner fields; `ENTITYCALL` is force-cleared of persistent by the definition layer.|
| `VECTOR2/3/4` | multiple scalar columns | Stored per component under the default float configuration; int under the `CLIENT_NO_FLOAT` build switch.|
| `FIXED_DICT` | expanded into several columns in the same table | Only keys marked persistent are created; column names compose as `sm_<dict>_<key>`, nested dicts keep prefixing.|
| `ARRAY` | a dedicated child table | The array is not stuffed into a single BLOB column; each element is a child-table record.|

The base-type factory mapping is at `kbe/src/lib/db_mysql/entity_table_mysql.cpp:505-759`; the string-length default at `:1819-1838`; per-component vector column write/read at `kbe/src/lib/db_mysql/entity_table_mysql.cpp:1037-1049`, `:1068-1092` (VECTOR3) and `:1120-1132`, `:1151-1175` (VECTOR4). Numeric SQL values are read from the MemoryStream according to the declared integer/float type, see `kbe/src/lib/db_mysql/entity_table_mysql.cpp:1665-1807`.

Although the `FIXED_DICT` factory branch constructs its item with a `BLOB` flag, its `initialize()` then creates child items for each persistent key and composes column prefixes recursively from the parent dict key; the actual data is read/written per column by those child items. You therefore cannot infer a single blob column in the database from the factory's initial marker alone. See `kbe/src/lib/db_mysql/entity_table_mysql.cpp:721-724`, `:1384-1417`, `:1453-1497`.

`ARRAY`'s `initialize()` creates an `EntityTableMysql` child table. The table name is composed from the entity table and the property's nested path, e.g. `tbl_Player_inventory`; for non-`FIXED_DICT` elements the column is `sm_value`, the array child table carries `parentID`, and each record has its own `id`. Exact columns also depend on the element type and nesting. See `kbe/src/lib/db_mysql/entity_table_mysql.cpp:1218-1288`, `:1250-1276`, `:379-395`, plus `kbe/src/lib/db_interface/entity_table.h:37-44`. Array INSERT/UPDATE goes through `WriteEntityHelper` recursively into the child table; updates locate old child records by parent ID and reuse/add/delete records. See `kbe/src/lib/db_mysql/write_entity_helper.h:60-89`, `:92-109`, `:195-280`.

Entity Components also use child tables: the table name is the parent entity table name plus the component property name, the component's persistent properties are expanded into it, and the relation is joined by parent ID. See `kbe/src/lib/db_mysql/entity_table_mysql.cpp:1507-1559`, `kbe/src/lib/db_mysql/sqlstatement.h:82-91`.

**Do not confuse inheritance with the child tables above.** A `.def` `<Parent>` recursively loads the parent definition into the same `ScriptDefModule`; `EntityTables` creates tables per module name. So in this implementation, inherited entity properties land in the child module's descriptions and that module's entity table — there is no automatic ORM-style parent-table join. The child tables that actually use `parentID` are ARRAY/Component-like structures. See `kbe/src/lib/entitydef/entitydef.cpp:890-923`, `kbe/src/lib/db_interface/entity_table.cpp:117-143`, `kbe/src/lib/db_mysql/entity_table_mysql.cpp:379-395`. Whether BigWorld 14.4.1 has different rules for inherited tables: unverified.

### 1.3 Who generates the schema, and when

After `DBUtil::initInterface()` initializes the database interface and its connection thread pool, it calls `EntityTables::load()` to build the in-memory mapping; when the environment check succeeds and the interface is not `pure`, it calls `EntityTables::syncToDB()`. This happens at DB-interface initialization, not per entity creation or first property write. See `kbe/src/lib/db_interface/db_interface.cpp:218-245`.

`EntityTables::syncToDB()` queues the KBE-table and entity-table sync tasks into the DB thread pool and waits for completion; MySQL's `EntityTableMysql::syncToDB()` issues `CREATE TABLE IF NOT EXISTS tbl_... (id ..., PRIMARY KEY...)`, then syncs property columns and indexes. Missing columns go through `ALTER TABLE ADD`, type/length changes through `MODIFY COLUMN`; the source also drops entity tables/property columns that no longer exist in the definitions. See `kbe/src/lib/db_interface/entity_table.cpp:169-227`, `kbe/src/lib/db_mysql/entity_table_mysql.cpp:370-425`, `:51-107`, `:427-463`. This auto-sync is convenient but means deployments must review the migration risk of dropped columns/retyped fields.

### 1.4 Property change to DB write: dirtying is not an immediate flush

KBEngine's `setDirty()` is not an SQL write and not a per-property dirty-bit map: it zeroes the five-word `persistentDigest_`; `isDirty()` tests for all-zero. Base and Cell definitions are at `kbe/src/server/baseapp/entity.inl:107-131` and `kbe/src/server/cellapp/entity.inl:263-287`. Property change callbacks call `setDirty()` only when the property is marked persistent; the Cell side additionally requires the entity to be real and out of initialization. See `kbe/src/server/baseapp/entity.cpp:96-104`, `kbe/src/server/cellapp/entity.cpp:658-666`.

The actual write path is:

1. Base `Entity::writeToDB()` saves an optional Python callback; if a Cell entity exists it sends `reqWriteToDBFromBaseapp`, otherwise it finishes the Base-side collapse directly. See `kbe/src/server/baseapp/entity.cpp:1141-1229`.
2. On receiving the request, Cell calls the Cell script `onWriteToDB()`; `backupCellData()` collapses Cell state into Base, then sends `onCellWriteToDBCompleted` to Base. See `kbe/src/server/cellapp/cellapp.cpp:775-793`, `kbe/src/server/cellapp/entity.cpp:1192-1249`.
3. Base calls `addPersistentsDataToStream()`, serializing only persistent properties (property UID + type) into a MemoryStream; Cell persistent properties prefer the Cell mirror, others come from the Base namespace. See `kbe/src/server/baseapp/entity.cpp:398-509`. The stream is then SHA1-hashed; if identical to `persistentDigest_` nothing is written, otherwise the digest updates and `DbmgrInterface::writeEntity` fires. See `:1319-1387`.
4. `Dbmgr::writeEntity()` routes by DB interface into `Buffered_DBTasks`, creating a `DBTaskWriteEntity`; the DB thread calls `EntityTables::writeEntity()`, and the MySQL table implementation builds the INSERT/UPDATE. See `kbe/src/server/dbmgr/dbmgr.cpp:1044-1065`, `kbe/src/server/dbmgr/dbtasks.cpp:328-355`, `kbe/src/lib/db_interface/entity_table.cpp:295-302`, `kbe/src/lib/db_mysql/entity_table_mysql.cpp:778-817`. When the DB task completes, `presentMainThread()` reports success/failure back to Base, see `kbe/src/server/dbmgr/dbtasks.cpp:378-397`.

### 1.5 Periodic archive, shutdown, cell switch: three different semantics

- **Periodic DB archiving**: Base calls `Archiver::tick()` every game tick; entities with `shouldAutoArchive` enabled and an existing DB record are spread across ticks by `archivePeriod` and call `writeToDB()`. The Cell→Base→Dbmgr path still applies, and an unchanged digest skips the real DB write. See `kbe/src/server/baseapp/baseapp.cpp:558-583`, `kbe/src/server/baseapp/archiver.cpp:23-63`, `:66-85`.
- **Periodic Cell→Base backup is not a DB flush**: `Backuper` batches `writeBackupData()` by `backupPeriod`; the default implementation requests Cell state and loads changed Cell data back into Base. It does not issue a DB task. See `kbe/src/server/baseapp/backuper.cpp:25-74`, `kbe/src/server/baseapp/entity.cpp:1091-1138`, `kbe/src/server/cellapp/entity.cpp:1129-1181`.
- **Switching/destroying a Cell**: on Cell destruction/migration `backupCellData()` may run first, then Base is notified via `onLoseCell`; this guarantees state returns to Base but is not a DB write. See `kbe/src/server/cellapp/entity.cpp:204-221`. Only an explicit `writeToDB()` enters the persistence path.
- **Shutdown**: the `Baseapp::finalise()` inspected here does resource cleanup, cancels timers and calls the parent finalise — there is no full `writeToDB()` sweep. See `kbe/src/server/baseapp/baseapp.cpp:650-670`. So do not assume "shutdown auto-flushes"; whether the outer stop orchestration requests archiving first is unverified, and a reliable stop should explicitly wait for persistence to complete.

### 1.6 What about non-persistent properties that still need caching?

A `Persistent=false` property does not enter `persistentPropertyDescr_` and is not written by the generic entity-table path above; but it still exists on the runtime object per its Base/Cell flags, participates in the corresponding sync, and its Cell data can be periodically backed up to Base. That backup is in-process protection of the authoritative/mirrored state, not recoverable database persistence. Evidence: `kbe/src/lib/entitydef/scriptdef_module.cpp:603-614`, `kbe/src/server/baseapp/entity.cpp:398-509`, `kbe/src/server/cellapp/entity.cpp:1129-1181`.

If it is only a performance cache, keep it a non-persistent runtime property; if it must survive restarts, declare it persistent or implement an explicit business storage/cache interface. The source provides a raw DB interface (for storage outside the entity schema) and DB task entry points, but their use depends on project code; do not treat `shouldAutoBackup` as a durability guarantee. The condition for a `pure` interface to bypass entity-table sync is at `kbe/src/lib/db_interface/db_interface.cpp:241-245`; Entity `writeToDB`'s refusal for pure interfaces is at `kbe/src/server/baseapp/entity.cpp:1153-1165`.

### 1.7 How relations span multiple DB tables

Array and component child tables have `id` and `parentID` in the schema; inserting a child record passes the parent table's generated DBID into the child context and writes it as `parentID`. See `kbe/src/lib/db_mysql/entity_table_mysql.cpp:379-395`, `kbe/src/lib/db_mysql/write_entity_helper.h:75-89`, `kbe/src/lib/db_mysql/sqlstatement.h:82-91`. Reads query the main table first, then the child table by the main row's ID; `ReadEntityHelper::queryChildDB()` reads the child table's `id,parentID` and organizes data by parent ID. See `kbe/src/lib/db_mysql/read_entity_helper.h:123-149`, `:173-205`.

A persisted property stream of one entity type maps property UIDs to `EntityTableItem`s, and each type's item handles its scalar column/nested structure; the entity DBID is the main table's primary key. `EntityTable::writeTable()` finds the item by UID, see `kbe/src/lib/db_interface/entity_table.cpp:33-61`; MySQL's `writeTable()` reads `pid, child_pid` from the stream and generates SQL per item, see `kbe/src/lib/db_mysql/entity_table_mysql.cpp:778-817`. The source shows cross-table parent/child relations with serial recursive tasks; do not assume every child table is enforced by a database foreign key (no FK creation code was found here).

### Implications for Mistlane

1. Make the entity contract the single source of truth for schema/serialization, but keep automatic schema changes separate from explicit migration review; dropping a field must not silently become destructive DDL in production.
2. Define "dirty marking", "state mirroring/backup" and "durable flush" as three distinct API/guarantee levels; migration, shutdown and periodic jobs must state explicitly which one they wait for.
3. Collection/aggregate storage needs explicit parent keys, ordering and update/delete semantics; neither stuff complex types into unqueryable blobs nor make every array item a relational table by default.

## Chapter 2: why this is not a MyBatis-style ORM

### 2.1 Source conclusion: not "no mapping layer", but an engine-owned dedicated mapper

KBEngine has the layering `EntityTables → EntityTableMysql/Redis → DBInterface`, with table items created from `ScriptDefModule` and `PropertyDescription`; the MySQL backend performs column mapping and SQL. See `kbe/src/lib/db_interface/entity_table.cpp:117-143`, `:295-302`, `kbe/src/lib/db_mysql/entity_table_mysql.cpp:136-170`. The accurate statement is: it does not use a general-purpose application ORM; it uses a dedicated mapper bound to the entity protocol, lifecycle and threading model.

### 2.2 Engine constraints and the cost of an ORM

1. **Masses of online entities live on high-frequency ticks, and creation/destruction can be frequent.** An Entity carries its own script object, property descriptions, state and entity-container management on Base/Cell; the engine also explicitly controls object memory, serialization and cross-component messaging. Layering an ORM identity map, session lifecycle, proxies/lazy loading on top of every runtime entity would grow the object graph and lifecycle-management cost. "Massive/short-lived" here is the common target of such game servers, not a fixed quantity guaranteed by the source; MyBatis's `SqlSession`/mappers and Hibernate-style lazy proxies are not the same mechanism either — one cannot lump all ORMs together as proxying.
2. **Property access is far more frequent than DB access.** Base/Cell property setters go through `PropertyDescription::onSetValue`; persistent property changes set dirty. Game logic reads/writes entities directly, not via SQL/session per access. See `kbe/src/lib/entitydef/property.cpp:206-215`, `kbe/src/server/baseapp/entity.cpp:96-104`, `kbe/src/server/cellapp/entity.cpp:658-666`. Putting an ORM proxy/tracking layer on that hot path, even at a small per-call indirect cost, multiplies across the huge number of property accesses per tick; this is a performance/architecture inference, not a benchmark conclusion from this source.
3. **The static contract already provides runtime metadata.** The `.def` types, flags, persistent, UID, defaults and lengths load into `PropertyDescription`/`ScriptDefModule`, directly driving property wire serialization, DB table-item generation and schema sync. See `kbe/src/lib/entitydef/entitydef.cpp:1097-1297`, `kbe/src/lib/entitydef/scriptdef_module.cpp:603-614`, `kbe/src/server/baseapp/entity.cpp:398-509`. Describing the same columns again with ORM annotations/XML makes a duplicated schema; unless the addition supplies query capability, most of its mapping config is redundant.
4. **State authority crosses Base/Cell/Dbmgr boundaries.** Cell is the pre-DB collapse point for spatial state, Base serializes the persistence stream centrally and posts tasks to Dbmgr; DB-thread task completions call back onto the main thread. See `kbe/src/server/baseapp/entity.cpp:1217-1229`, `:1289-1387`, `kbe/src/server/dbmgr/dbmgr.cpp:1044-1065`, `kbe/src/server/dbmgr/dbtasks.cpp:328-397`. If an ORM session/entity proxy were treated as a cross-process shared identity, invalidation, ownership, concurrency and reconnection semantics would all need defining — conflicting with the "each process owns its Entity/snapshot and migrates by message" model. The workable boundary is using an ORM only inside Dbmgr's persistence adapter; it cannot replace the cross-process state protocol.
5. **DB access is kept off the game's main logic thread.** Interface initialization configures a DB thread pool; `Dbmgr::writeEntity()` wraps single-entity `DBTaskWriteEntity` into a delivery queue, executes in the background and returns to the main thread. See `kbe/src/lib/db_interface/db_interface.cpp:218-245`, `kbe/src/server/dbmgr/dbmgr.cpp:1044-1065`, `kbe/src/server/dbmgr/dbtasks.cpp:328-397`. The existing source proves an async task queue and a multi-connection thread pool; **no ORM-style batched SQL flush was verified on this generic write path**, so "the engine batches and ORMs necessarily break batching" cannot be stated as fact. If Mistlane wants batched writes, design a batch API directly and evaluate transaction/ordering/callback semantics.

### 2.3 Why "more flexible" does not automatically mean more suitable

A MyBatis-style mapper's flexibility lies in SQL, projections, joins, dynamic conditions and result mapping; the core needs of the entity hot path are fixed-type property access, protocol serialization, Cell/Base mirrors and asynchronous snapshot writes. The engine's table items are looked up by property UID with dedicated handling for ARRAY/FIXED_DICT/Component rather than materializing arbitrary SQL results into online entities, see `kbe/src/lib/db_interface/entity_table.cpp:33-61`, `kbe/src/lib/db_mysql/entity_table_mysql.cpp:778-817`. This is workload-scoped flexibility traded for performance and consistency — not a claim that ORMs are inflexible in general business development.

### 2.4 The other side: where an ORM genuinely is better

- For admin backends, operations tooling, accounting/ordering, reports and other non-tick-hot-path business where CRUD/join queries dominate over high-frequency property access, an ORM cuts SQL boilerplate substantially and brings mature transaction, migration and projection capabilities.
- When the domain object count is bounded, lifetimes are request/transaction-scoped, and objects never migrate across Cell/Base, ORM session/identity-map costs are more controllable and yield a consistent query model.
- When the schema evolves often and needs many query combinations, explicit migration + an ORM is usually more manageable than "auto-syncing tables from game entity definitions".
- A hybrid boundary works: online Entities keep the lightweight contract and async DB tasks; Dbmgr's account/admin/report services can use an ORM in a separate module. The precondition is that the ORM holds no long-lived session over cross-process Entities and never blocks the game tick.

### Implications for Mistlane

1. There is no need to copy a traditional ORM, nor to hand-write every query in the name of "no ORM": layer hot/cold paths — an explicit schema mapper for the core entity protocol, an ORM on the admin/report side.
2. The data-access abstraction must not swallow the execution boundary: game threads only construct serializable commands; I/O completes on worker threads; results return to the owning process by ID/callback/event.
3. For any new proxy/change-tracking mechanism, benchmark per-entity memory, property access and tick tail latency first; the local source has no benchmarks, so conclusions like "ORMs are always slower" are unfounded.

## Chapter 3: design philosophy read back from the KBEngine implementation

> This chapter addresses the readable KBEngine source. The responsibility boundaries of same-named components in BigWorld 14.4.1 and the differences below: unverified.

### 3.1 Multi-process split: isolating different authorities and loads

The local KBEngine code contains the `baseapp`, `baseappmgr`, `cellapp`, `cellappmgr`, `dbmgr`, `loginapp` process directories; there is no process directory named `gateapp`. Nor is there evidence to equate KBEngine's `loginapp` with BigWorld's `gateapp`. Loginapp accepts login requests and processes account information, eventually returning login success to the client once it receives the Baseapp address; see `kbe/src/server/loginapp/loginapp.cpp:877-921`, `:1303-1339`. Baseappmgr picks a Baseapp for an entity by load, see `kbe/src/server/baseappmgr/baseappmgr.cpp:279-318`; cellappmgr likewise assigns Cellapps by process load/entity count and forwards space-creation requests, see `kbe/src/server/cellappmgr/cellappmgr.cpp:230-263`, `:310-382`. Dbmgr pushes data tasks onto a background thread pool, see `kbe/src/server/dbmgr/dbmgr.cpp:1044-1065`.

Benefits inferable from the implementation: Base sessions/long-term properties, Cell spatial simulation, DB I/O and access/login each have their own scheduling and failure boundaries; Cellapp count can scale independently with load distribution by the mgr. The costs are inter-process message protocols, component discovery/registration, state snapshot and recovery complexity; even one DB write needs the Cell→Base→Dbmgr multi-hop with async callbacks. This describes KBEngine's readable architecture; what BigWorld's gateapp specifically handles: unverified.

### 3.2 Cell vs Base properties: driven by spatial locality and authority

`.def` properties carry Base/Cell/Client flags; parsing places descriptions into different property sets by flags, and persistent is an independent dimension — not "Base property". See `kbe/src/lib/entitydef/entitydef.cpp:1134-1168`, `:1277-1297`. Base keeps long-term entity/session context and a mirror of Cell data; Cell owns real-time spatial state, marks dirty on persistent Cell property change and can broadcast to nearby ghosts. See `kbe/src/server/baseapp/entity.cpp:398-414`, `kbe/src/server/cellapp/entity.cpp:658-708`.

The implicit spatial-locality assumption: an entity has a real owner on some space/Cellapp and the overwhelming majority of simulation interactions happen within that local range; across Cellapps only the necessary ghosts/state near boundaries replicate, rather than all nodes sharing one globally mutable object. The Cell code explicitly broadcasts selected Cell properties to ghosts only when ghosts exist, see `kbe/src/server/cellapp/entity.cpp:684-708`. Splitting Base/Cell lets low-frequency long-term logic and high-frequency spatial simulation live in different resource pools, but cross-boundary interaction and migration pay copying, synchronization and consistency costs.

### 3.3 Built-in AOI: spatial triggers wire entity visibility directly to network replication

Cell's `Witness` installs `ViewTrigger`s on entity coordinate nodes by view radius; entering/leaving a trigger maintains view-entity references, witness relations and client pending state. See `kbe/src/server/cellapp/witness.cpp:277-353`, `:356-438`, `:441-470`. Witness attach sends entity enter-world/position-initialization messages to the client, see `:112-160`. AOI is therefore not just a business query of "who is nearby": it directly decides which entities a client sees, when they enter/leave view, and the spatial replication load. Building it in reuses one set of coordinate indexing, triggering and lifecycle logic and avoids re-implementing it per gameplay; the cost is coupling the engine's spatial model to AOI semantics — non-traditional-3D-space business must adapt.

### 3.4 Strong assumptions inside the distributed premise

Strong constraints/assumptions visible in the code:

1. **A clear single current owner and per-component addressing.** `componentID`, `entityID`, `dbid` travel explicitly in DB messages, and the online-entity DB log records which process owns an entity; it is not multiple services directly mutating the same entity row. See `kbe/src/server/dbmgr/dbmgr.cpp:1047-1065`, `kbe/src/server/dbmgr/dbtasks.cpp:357-371`.
2. **Entity state is packagable per contract.** Persistent properties serialize into a MemoryStream by UID and type, and Cell data has dedicated collapse/backup; process boundaries are serialization boundaries. See `kbe/src/server/baseapp/entity.cpp:398-509`, `:1319-1387`, `kbe/src/server/cellapp/entity.cpp:1129-1181`.
3. **Cellapps carry spatial-partition semantics; mgrs manage placement.** A new space is assigned to a Cellapp chosen by Cellappmgr which forwards the creation request — not arbitrary stateless microservices scaling on HTTP requests. See `kbe/src/server/cellappmgr/cellappmgr.cpp:230-263`, `:310-382`.
4. **Consistency comes more from message ordering, authority migration and snapshot constraints than from shared DB transactions.** DB writes are independent async tasks; cell/base data is gathered before writing; the online-entity log prevents duplicate checkouts from multiple places. See `kbe/src/server/dbmgr/dbtasks.cpp:328-397`, `kbe/src/server/dbmgr/dbtasks.cpp:357-371`.

So "the game server is distributed" here does not mean flattening a monolith into stateless replicas: entities and spaces have owners by default, network partitions need explicit migration/mirroring, and persisted state is a recovery source rather than a per-run read source. The risk of losing unflushed data on crash exists too; the archive period defines the potential loss window. The source provides no exactly-once or cross-DB/game-process transaction guarantees.

### 3.5 What looks dated today and what still holds

**Implementation forms to re-evaluate:**

- Dedicated mgr/central-directory control planes that pick concrete processes integrate differently from cloud service discovery, elastic scheduling and failure-domain/region placement; the "route by entity/space owner" semantics can stay while the control-plane implementation is replaced.
- Entity definitions directly drive production schemas with automatic add/modify/drop of columns/tables at startup (`kbe/src/lib/db_interface/entity_table.cpp:208-226`, `kbe/src/lib/db_mysql/entity_table_mysql.cpp:427-463`) and no explicit versioned migration/approval boundary; this especially needs redesign for online rolling upgrades.
- Strongly-stateful processes, Python-object entities and single-authority migration are incompatible with the cloud-native default of "fully stateless services where any replica takes any request"; forcibly stateless-izing pushes spatial simulation into distributed shared state and high-frequency network sync, which is not necessarily simpler.
- KBEngine has no process named GateApp; how gateway duties split needs per-distribution verification, and BigWorld original is unverified here.

**Cores that still hold:**

- High-frequency simulation partitions by space and keeps local authority, reducing shared writes and global sync; modern services still need sharded ownership/actor placement.
- AOI/interest management is part of the replication and bandwidth budget; incremental updates driven by spatial indexes and view boundaries remain effective.
- Decoupling DB I/O from the game tick, explicit async callbacks, and skipping writes when the snapshot digest is unchanged remain valid principles for tail latency.
- A static schema helps serialization compatibility: client protocol and DB mapping share one contract; the modern equivalent can be generated code/explicit migrations without regressing to hand-written DTOs, SQL and network fields.

### Implications for Mistlane

1. Adopt the distinction "domain state has an owner; services are replaceable/restartable": cloud-native does not mean ownerless simulation state; define an authoritative node, lease/migration and crash recovery for every entity/scene.
2. Treat AOI/visibility as a transport-layer resource-control mechanism: business supplies interest policy; the runtime owns spatial indexing, incremental diffs, throttling and client lifecycle.
3. Keep the value of Cell/Base-style responsibility isolation without copying process names: deploy independently scalable workers based on real CPU/network/persistence bottlenecks, and decouple the control plane from the game data plane.
4. Persistence flush periods, shutdown drain, migration confirmation and DB failure retries must form a testable protocol; never mistake dirty flags, cached mirrors or process exit for a durable commit.
