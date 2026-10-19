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
#include <map>
#include <regex>
namespace fs = std::filesystem;
namespace mistlane {
namespace {
struct Artifact {
    fs::path xsd, lock, digest, sproto, bindings, javascript, csharp;
    std::string xsd_text, lock_text, digest_text, sproto_text, bindings_text, javascript_text, csharp_text;
};
void write_artifact(const fs::path& target, const std::string& content) {
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot open "+target.string());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) throw std::runtime_error("cannot write "+target.string());
}
using LockEntries = std::map<std::string, std::string>;
LockEntries read_lock(const fs::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot read release lock: "+path.string());
    LockEntries entries; std::string line;
    while (std::getline(in, line)) {
        std::istringstream row(line); std::string kind, a, b, c, d, e, f, g;
        row >> kind;
        if (kind == "entity") { row >> a >> b; entries["entity "+a] = b; }
        else if (kind == "prop") { row >> a >> b >> c >> d >> e; entries["prop "+a+" "+b] = c+" "+d+(e.empty()?"":" "+e); }
        else if (kind == "method") { row >> a >> b >> c >> d >> e >> f; entries["method "+a+" "+b+" "+c] = d+" "+e+" "+f; }
        else if (kind == "arg" || kind == "return") {
            row >> a >> b >> c >> d >> e >> f >> g;
            entries[kind+" "+a+" "+b+" "+c+" "+d] = e+" "+f+(g.empty()?"":" "+g);
        }
        else if (kind == "struct") {
            row >> a;
            static const std::regex field(R"(\{([0-9]+) ([A-Za-z0-9_]+) ([A-Za-z0-9_]+)(?: of ([A-Za-z0-9_]+))?\})");
            for (std::sregex_iterator it(line.begin(), line.end(), field), end; it != end; ++it)
                entries["struct "+a+" "+(*it)[1].str()] = (*it)[2].str()+" "+(*it)[3].str()+((*it)[4].matched ? " of "+(*it)[4].str() : "");
        }
    }
    return entries;
}
std::string read_digest(const fs::path& path) {
    std::ifstream in(path); std::string line;
    while (std::getline(in,line)) if (line.starts_with("# digest ")) return line.substr(9);
    throw std::runtime_error("release lock has no version digest: "+path.string());
}
int compare_locks(const fs::path& old_path, const fs::path& new_path) {
    const auto before = read_lock(old_path), after = read_lock(new_path);
    std::cout << "VERSION_DIGEST old=" << read_digest(old_path) << " new=" << read_digest(new_path) << '\n';
    bool breaking = false;
    for (const auto& [key, value] : before) {
        const auto it = after.find(key);
        if (it == after.end()) { std::cout << "REMOVED " << key << " [BREAKING]\n"; breaking = true; }
        else if (it->second != value) { std::cout << "TYPE_CHANGED " << key << ": " << value << " -> " << it->second << " [BREAKING]\n"; breaking = true; }
    }
    for (const auto& [key, value] : after)
        if (!before.contains(key)) std::cout << "ADDED " << key << " = " << value << " [COMPATIBLE]\n";
    std::cout << (breaking ? "COMPATIBILITY_GATE BLOCKED\n" : "COMPATIBILITY_GATE PASSED\n");
    return breaking ? 1 : 0;
}
}
int run_contract(int argc, char** argv) {
    if (argc < 3) throw std::invalid_argument("contract needs a verb: check or build");
    const std::string verb(argv[2]);
    if (verb == "compat") {
        if (argc != 5) throw std::invalid_argument("contract compat needs OLD.lock NEW.lock");
        return compare_locks(argv[3], argv[4]);
    }
    if (verb != "check" && verb != "build")
        throw std::invalid_argument("unknown contract verb: "+verb);
    std::vector<fs::path> files;
    fs::path out;
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: mistlane contract check FILE...\n"
                         "       mistlane contract build --out DIR FILE...\n"
                         "       mistlane contract compat OLD.lock NEW.lock\n"
                         "check validates each entity-contract document (.def) against the\n"
                         "definition-time rules. build validates first and, only when every\n"
                         "file passes, writes XSD, lock, digest, sproto and C++/JS/C# bindings.\n"
                         "compat compares old and new locks; breaking removals or changes return nonzero.\n";
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
                a.javascript = out/(stem+".bindings.js");
                a.csharp = out/(stem+".bindings.cs");
                a.xsd_text = contract.xsd();
                a.lock_text = contract.lock();
                std::ostringstream hex;
                hex << std::hex << std::setw(16) << std::setfill('0') << contract.digest() << '\n';
                a.digest_text = hex.str();
                a.sproto_text = contract.sproto();
                a.bindings_text = contract.cpp_bindings();
                a.javascript_text = contract.javascript_bindings();
                a.csharp_text = contract.csharp_bindings();
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
        write_artifact(a.javascript, a.javascript_text);
        write_artifact(a.csharp, a.csharp_text);
        std::cout << "wrote "+a.lock.string() << '\n';
    }
    return 0;
}
}
