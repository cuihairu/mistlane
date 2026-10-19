// SPDX-License-Identifier: Apache-2.0
#pragma once
namespace mistlane {
// `mistlane contract check|build` is the generation half of the entity
// contract toolchain. check validates .def documents against the
// definition-time rules in include/mistlane/entity_contract.hpp; build
// validates first and only then emits the per-document project XSD, release
// lock and version digest. Release-compatibility reports and server bundle
// loading are not implemented yet.
int run_contract(int argc, char** argv);
}
