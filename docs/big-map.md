# Big maps: assessment and requirements

Status: assessment and design requirements (2026-10-19), no implementation. This page answers the large-map item on the model page by separating three genuinely different problems and stating what the repository evidence does and does not prove for each. It fixes the vocabulary so later slices cannot conflate instantiation, chunking, AOI and cross-process handoff, and it lists the requirements a streaming or spatial-partition design must meet. No code changes and no CLI behavior follow from this page. The scene-state side is the [scene lifecycle](./scene-lifecycle.md) page; legacy evidence is the [model page](./player-scene-model.md) and the [capability assessment](./gameserver-capabilities.md).

## Three axes, assessed separately

| Axis | What it is | What the evidence proves | What it does not prove |
|---|---|---|---|
| Resource volume | The size and count of map assets (models, textures, server obstacles/navigation) | The model page separates server-visible static data from client material; the scene-lifecycle page puts server data in a process-local read-only cache with reference release | That chunking, streaming or prefetching exists, or that client assets are loaded in any particular way — client loading cannot be inferred from server code |
| Active entity count | Simultaneously simulated/observed entities in one scene | Legacy uses AOI-style position sync (`UPDATE_INTERVAL=1000`) and scene-based organization; nothing claims one scene must hold the whole world | That any AOI, activation budget or simulation budget is implemented, measured or tuned |
| Cross-process seamless | One logical world spread over several processes with invisible authority transfer | Player transfer between scenes/processes is an observed legacy path | That seamless spatial partitioning, neighbor state or authority handoff exist |

The three are independent: a small resource set can host many active entities, a huge resource set can host few, and seamless partitioning is orthogonal to both. Solving one must never be reported as solving another.

## What must not be conflated

The following are different mechanisms with different proofs, and no implementation status claim may equate them:

- **Instantiation** — allocating a scene instance from a definition. Proving scenes can be created proves nothing about chunking or AOI.
- **Chunking / streaming** — delivering part of a large map on demand. A spatial cache that shares static data does not stream it.
- **AOI** — limiting which entities synchronize with each other. It bounds sync traffic, not activation, simulation cost or memory for the whole map.
- **Avatar cross-server handoff** — transferring a participant and its context between processes. Transferring a whole character is not providing seamless spatial partitioning.
- **Server static-data sharing** — one definition serving many instances (the cache on the scene-lifecycle page). It is not client resource management.

## Client and server loading are separate

| Side | What it loads | Where it lives in this design |
|---|---|---|
| Client | Models, textures, effects; large and latency-sensitive | Client asset pipeline, out of scope for server code; cannot be inferred from this repository |
| Server | Obstacles, walkability, navigation, rules-referenced static data | The process-local read-only spatial cache on the [scene lifecycle](./scene-lifecycle.md) page |

The server needs only the static data that gameplay decisions and routing actually consult. A definition's client-only material is referenced, never loaded into the server cache. A streaming design therefore has two budgets to reconcile — server bytes-per-enter and client bytes-per-region — and the two are not interchangeable.

## Streaming requirements (no implementation)

A chunked delivery design, when it is built, must satisfy these requirements:

1. **Chunking is a delivery mechanism, not a cache.** Streaming must interact with the spatial-cache release rule on the scene-lifecycle page: a streamed chunk is either part of the instance's referenced data or an evictable part of the cache; it is never both at once.
2. **Never transmit on entry.** The no-repeat-load rule applies to chunks too: entering or migrating must not re-download or re-send data the destination process already holds.
3. **Chunk identity is stable and digests-pinned.** A chunk is addressed by (definition id, digest, chunk id); the digest pins the whole definition, so a chunk cannot change underneath a reader.
4. **Prefetch and eviction are explicit.** The framework defines prefetch triggers and eviction rules up front, driven by the reference model — an instance that references a chunk keeps it, and eviction never races a load.
5. **Failure is per-request and retryable.** A chunk that fails to load is reported to the request that asked, the prepare of any instance depending on it fails or waits inside its budget, and no partial "ready with a hole" state may be published.
6. **Budgets are measured, not assumed.** Chunk size, prefetch distance and eviction policy are chosen from measurement on target hardware; the environment cannot profile reliably today, so no numbers are claimed here.

## Spatial partition requirements (no implementation)

A multi-process seamless design, when it is built, must satisfy these requirements:

1. **Authority is handed off, never guessed.** An entity crossing a partition boundary changes authority only through an explicit, acknowledged handoff; a missed heartbeat or a lost update must not create two owners or leave a no-man's land. This mirrors the single-writer discipline of the [cross return protocol](./cross-return-protocol.md).
2. **Neighbor state is explicit.** Cross-partition interactions that need another partition's state must declare it; nothing implies whole-world state is visible to every partition.
3. **Seamlessness is a player-visible property, not a code layout.** "One map in code" is not evidence of seamlessness; the acceptance criterion is that a player crossing a boundary observes no ownership stall or duplicated state.
4. **A scene instance stays one-writer.** Partitioning is about placement of entities and authority over a logical world, and it must not create two writers for one instance — the one-writer-per-instance rule on the scene-lifecycle page holds.
5. **Budget boundaries are activation and simulation, not area.** Compute load is decided by what is active in a partition, so a partition budget is expressed in active entities and simulation cost, not in square metres.

## Interaction with scene lifecycle

Streaming and partitioning plug into the prepare state machine on the [scene lifecycle](./scene-lifecycle.md) page: `preparing` is where chunk loads and partition references resolve, `ready` is the only state that may accept entry, and a chunk or handoff that cannot complete inside the budget makes the prepare fail — never a "ready with a hole". This keeps this page's requirements consistent with the lifecycle's states instead of inventing a parallel model.

## Deliberately not decided here

- Whether streaming or partitioning is built at all, and when: this page is requirements, not a schedule.
- Chunk sizes, prefetch distances, activation budgets and partition shapes: measurement, not this page.
- The map definition file format and how chunks are addressed on disk: the implementation-bound choice named on the scene-lifecycle page.
- Any use of third-party streaming systems: the model page already notes external references (e.g. UE World Partition) are references only, not integrated features.

## Verification

None today. There is no streaming or spatial-partition code, no test and no runtime acceptance; this page states requirements. Validation records must not claim large-map behaviour.

Related: [players, scenes and cross-region execution](./player-scene-model.md), [scene lifecycle](./scene-lifecycle.md), [gameServer capability assessment](./gameserver-capabilities.md), [cross return and single-writer protocol](./cross-return-protocol.md).
