// @mistlane/devcore — IDE-agnostic core for mistlane devtools.
//
// Surfaces, all verified against the engine tree (no guessing):
//   frame      u32 BE length | payload, 1 MiB bound   (src/session_plane.hpp,
//                                                      include/mistlane/transport.hpp)
//   handshake  "MLS1 <role> <instance> <token>" -> "MLS1 OK"; "MLC2" -> "MLC2 OK",
//              correlated frames carry a u32 BE request id   (src/session_plane.hpp)
//   entity     "MLE1" | envelope(39B BE) | payload; replies echo the correlation
//              u64 BE and answer with EntityReply   (include/mistlane/entity_envelope.hpp)
//   metrics    GET /metrics on the dashboard/metrics listener, Prometheus text
//              0.0.4   (src/observability.hpp DashboardServer)
//   logs       plain-text stdout/stderr redirect files (<root>/var/log/<instance>.log,
//              src/scaffold.cpp) read tail -F style by FileTailPoller; an engine
//              devlog tee socket is the phase-two upgrade
//   process    /proc/<pid>/status + stat for RSS/CPU   (kernel surface)
//   contract   entity-contract .def documents          (examples/contracts/business.def)

export * from "./frame.js";
export * from "./envelope.js";
export * from "./backoff.js";
export * from "./transport.js";
export * from "./promtext.js";
export * from "./metrics.js";
export * from "./logtail.js";
export * from "./proc.js";
export * from "./contract.js";
export * from "./attach.js";
