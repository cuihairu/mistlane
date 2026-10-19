// SPDX-License-Identifier: Apache-2.0
#include "mistlane/contract_tool.hpp"
#include "mistlane/entity_contract.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
namespace fs = std::filesystem;
namespace mistlane {
namespace {
struct Artifact {
    fs::path xsd, lock, digest, sproto, bindings;
    std::string xsd_text, lock_text, digest_text, sproto_text, bindings_text;
};
void write_artifact(const fs::path& target, const std::string& content) {
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot open "+target.string());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) throw std::runtime_error("cannot write "+target.string());
}
}
int run_contract(int argc, char** argv) {
    if (argc < 3) throw std::invalid_argument("contract needs a verb: check or build");
    const std::string verb(argv[2]);
    if (verb != "check" && verb != "build")
        throw std::invalid_argument("unknown contract verb: "+verb);
    std::vector<fs::path> files;
    fs::path out;
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: mistlane contract check FILE...\n"
                         "       mistlane contract build --out DIR FILE...\n"
                         "check validates each entity-contract document (.def) against the\n"
                         "definition-time rules. build validates first and, only when every\n"
                         "file passes, writes XSD, lock, digest, sproto and C++ bindings.\n"
                         "Each file is an independent contract, so cross-file type references\n"
                         "and release-compatibility reports are not implemented yet.\n";
            return 0;
        }
        if (arg == "--out") {
            if (verb != "build") throw std::invalid_argument("--out only applies to build");
            if (++i >= argc) throw std::invalid_argument("missing value for --out");
            out = argv[i];
            continue;
        }
        if (arg.starts_with('-')) throw std::invalid_argument("unknown contract option: "+std::string(arg));
        files.emplace_back(arg);
    }
    if (files.empty())
        throw std::invalid_argument("contract "+verb+" needs at least one document");
    if (verb == "build" && out.empty()) throw std::invalid_argument("contract build needs --out DIR");

    std::vector<Artifact> artifacts;
    std::set<std::string> written;
    bool failed = false;
    for (const auto& file : files) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            std::cerr << file.string() << ": cannot read\n";
            failed = true;
            continue;
        }
        std::ostringstream text;
        text << in.rdbuf();
        try {
            const EntityContract contract(text.str());
            std::cout << file.string() << ": ok\n";
            if (verb == "build") {
                const auto stem = fs::path(file).stem().string();
                if (!written.insert(stem).second)
                    throw std::invalid_argument("output name collision for "+stem);
                Artifact a;
                a.xsd = out/(stem+".xsd");
                a.lock = out/(stem+".lock");
                a.digest = out/(stem+".digest");
                a.sproto = out/(stem+".sproto");
                a.bindings = out/(stem+".bindings.hpp");
                a.xsd_text = contract.xsd();
                a.lock_text = contract.lock();
                std::ostringstream hex;
                hex << std::hex << std::setw(16) << std::setfill('0') << contract.digest() << '\n';
                a.digest_text = hex.str();
                a.sproto_text = contract.sproto();
                a.bindings_text = contract.cpp_bindings();
                artifacts.push_back(std::move(a));
            }
        } catch (const ContractError& e) {
            std::cerr << file.string() << ": " << e.what() << '\n';
            failed = true;
        }
    }
    if (failed) {
        std::cerr << "contract "+verb+" failed\n";
        return 1;
    }
    if (verb == "check") return 0;
    // Nothing is written until every document validated.
    if (fs::exists(out) && !fs::is_directory(out))
        throw std::invalid_argument("contract build --out is not a directory: "+out.string());
    fs::create_directories(out);
    for (const auto& a : artifacts) {
        write_artifact(a.xsd, a.xsd_text);
        write_artifact(a.lock, a.lock_text);
        write_artifact(a.digest, a.digest_text);
        write_artifact(a.sproto, a.sproto_text);
        write_artifact(a.bindings, a.bindings_text);
        std::cout << "wrote "+a.lock.string() << '\n';
    }
    return 0;
}
}
