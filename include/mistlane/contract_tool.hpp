// SPDX-License-Identifier: Apache-2.0
#pragma once
namespace mistlane {
// `mistlane contract check|build|load` is the generation half of the entity
// contract toolchain. check validates .def documents against the
// definition-time rules in include/mistlane/entity_contract.hpp; build
// validates first and only then emits the per-document project XSD, release
// lock, version digest, sproto schema, C++/JS/C# bindings, client-audience
// JS/C# bindings (SERVER_ONLY properties and server-internal methods
// excluded) and a bundle.manifest digesting every artifact. load verifies a
// build directory all-or-nothing: every manifest digest, no unlisted files,
// and the published digest agreeing with the release lock; any failure
// refuses readiness.
int run_contract(int argc, char** argv);
}
