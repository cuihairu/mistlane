// SPDX-License-Identifier: Apache-2.0
#include "mistlane/contract_tool.hpp"
#include "mistlane/entity_contract.hpp"
#include <openssl/evp.h>
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
    fs::path xsd, lock, digest, sproto, bindings, javascript, csharp, js_client, cs_client;
    std::string xsd_text, lock_text, digest_text, sproto_text, bindings_text, javascript_text, csharp_text;
    std::string js_client_text, cs_client_text;
};
void write_artifact(const fs::path& target, const std::string& content) {
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot open "+target.string());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) throw std::runtime_error("cannot write "+target.string());
}
std::string sha256_hex(std::string_view data) {
    unsigned char md[EVP_MAX_MD_SIZE];
    std::size_t len = 0;
    if (EVP_Q_digest(nullptr, "SHA256", nullptr, data.data(), data.size(), md, &len) != 1 || len != 32)
        throw std::runtime_error("sha256 failed");
    static const char* digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(2*len);
    for (unsigned int i = 0; i < len; ++i) { hex += digits[md[i] >> 4]; hex += digits[md[i] & 15]; }
    return hex;
}
std::string read_lock_text(const fs::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot read release lock: "+path.string());
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}
std::string read_digest(const fs::path& path) {
    std::ifstream in(path); std::string line;
    while (std::getline(in,line)) if (line.starts_with("# digest ")) return line.substr(9);
    throw std::runtime_error("release lock has no version digest: "+path.string());
}
int compare_locks(const fs::path& old_path, const fs::path& new_path) {
    // Both sides go through parse_lock, so every lock line participates —
    // reserved lines included, since they are what keeps retired ids retired.
    const auto diff = compat_diff(parse_lock(read_lock_text(old_path)),
                                  parse_lock(read_lock_text(new_path)));
    std::cout << "VERSION_DIGEST old=" << read_digest(old_path) << " new=" << read_digest(new_path) << '\n';
    for (const auto& line : diff.breaking) std::cout << line << " [BREAKING]\n";
    for (const auto& line : diff.additive) std::cout << line << " [COMPATIBLE]\n";
    std::cout << (diff.compatible() ? "COMPATIBILITY_GATE PASSED\n" : "COMPATIBILITY_GATE BLOCKED\n");
    return diff.compatible() ? 0 : 1;
}
struct ManifestEntry {
    fs::path name;
    std::string sha256;
};
// A bundle manifest is a header plus one line per shipped artifact. Entries
// are plain filenames: the loader joins them to the bundle directory, so a
// name that escapes the directory cannot be listed.
std::vector<ManifestEntry> parse_manifest(std::string text) {
    std::vector<ManifestEntry> entries;
    std::set<std::string> listed;
    bool header = false;
    bool format = false;
    std::istringstream lines(text);
    std::string line;
    for (std::size_t no = 0; std::getline(lines, line); ++no) {
        if (line.starts_with("# mistlane server bundle manifest")) { header = true; continue; }
        if (line.starts_with("# format ")) {
            format = true;
            if (line.substr(9) != "1") throw std::invalid_argument("unsupported bundle format: "+line.substr(9));
            continue;
        }
        if (line.empty()) continue;
        const auto at = line.find(" sha256 ");
        if (!line.starts_with("file ") || at == std::string::npos)
            throw std::invalid_argument("malformed bundle manifest line "+std::to_string(no+1));
        ManifestEntry entry;
        entry.name = fs::path(line.substr(5, at-5));
        entry.sha256 = line.substr(at + 8);
        if (entry.sha256.size() != 64 || entry.sha256.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::invalid_argument("malformed bundle manifest line "+std::to_string(no+1));
        if (entry.name.filename() == fs::path() || entry.name.parent_path().string().rfind(".",0) == 0
            || entry.name.filename().string().rfind(".",0) == 0)
            throw std::invalid_argument("bundle manifest name escapes the bundle: "+entry.name.string());
        if (listed.insert(entry.name.string()).second == false)
            throw std::invalid_argument("bundle manifest lists "+entry.name.string()+" twice");
        entries.push_back(std::move(entry));
    }
    if (!header || !format || entries.empty()) throw std::invalid_argument("malformed bundle manifest");
    return entries;
}
std::string trimmed(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text;
}
std::string file_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read "+path.string());
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}
// Bundle loading is all-or-nothing: every listed artifact is verified against
// its digest before anything else is trusted, the directory is then swept for
// strays, and the published digest must still agree with the release lock. A
// bundle that fails any check reports the cause and never reports readiness.
int load_bundle(const fs::path& dir) {
    const auto manifest = dir/"bundle.manifest";
    std::ifstream in(manifest, std::ios::binary);
    if (!in) throw std::runtime_error("no bundle manifest: "+manifest.string());
    std::ostringstream text;
    text << in.rdbuf();
    const auto entries = parse_manifest(text.str());
    for (const auto& entry : entries) {
        const auto path = dir/entry.name;
        std::ifstream file(path, std::ios::binary);
        if (!file) throw std::runtime_error("bundle missing artifact: "+entry.name.string());
        std::ostringstream bytes;
        bytes << file.rdbuf();
        if (sha256_hex(bytes.str()) != entry.sha256)
            throw std::runtime_error("bundle digest mismatch: "+entry.name.string());
    }
    std::set<std::string> listed;
    for (const auto& entry : entries) listed.insert(entry.name.string());
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file())
            throw std::runtime_error("bundle contains a non-file entry: "+entry.path().filename().string());
        const auto name = entry.path().filename().string();
        if (name != "bundle.manifest" && listed.count(name) == 0)
            throw std::runtime_error("bundle has unlisted file: "+name);
    }
    for (const auto& entry : entries) {
        if (!entry.name.string().ends_with(".digest")) continue;
        const auto published = trimmed(file_text(dir/entry.name));
        const auto stem = entry.name.string().substr(0, entry.name.string().size() - 7);
        const auto lock = dir/fs::path(stem+".lock");
        if (!fs::is_regular_file(lock) || read_digest(lock) != published)
            throw std::runtime_error("bundle model digest disagreement: "+entry.name.string());
    }
    std::cout << "BUNDLE_READY files=" << entries.size() << '\n';
    return 0;
}
}
int run_contract(int argc, char** argv) {
    if (argc < 3) throw std::invalid_argument("contract needs a verb: check or build");
    const std::string verb(argv[2]);
    if (verb == "compat") {
        if (argc != 5) throw std::invalid_argument("contract compat needs OLD.lock NEW.lock");
        return compare_locks(argv[3], argv[4]);
    }
    if (verb == "load") {
        if (argc != 4) throw std::invalid_argument("contract load needs BUNDLE_DIR");
        return load_bundle(argv[3]);
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
                         "       mistlane contract load DIR\n"
                         "check validates each entity-contract document (.def) against the\n"
                         "definition-time rules. build validates first and, only when every\n"
                         "file passes, writes XSD, lock, digest, sproto, C++/JS/C# bindings,\n"
                         "client-audience JS/C# bindings and the bundle manifest.\n"
                         "compat compares old and new locks; breaking removals or changes return nonzero.\n"
                         "load verifies every manifest digest, sweeps for unlisted files and\n"
                         "checks the published digest against the release lock; a bundle that\n"
                         "fails any check never reports readiness.\n";
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
                a.js_client_text = contract.javascript_bindings(true);
                a.cs_client_text = contract.csharp_bindings(true);
                a.js_client = out/(stem+".client.js");
                a.cs_client = out/(stem+".client.cs");
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
    std::ostringstream manifest;
    manifest << "# mistlane server bundle manifest\n# format 1\n";
    const auto emit_file = [&](const fs::path& path, const std::string& content) {
        write_artifact(path, content);
        manifest << "file " << path.filename().string() << " sha256 " << sha256_hex(content) << '\n';
    };
    for (const auto& a : artifacts) {
        emit_file(a.xsd, a.xsd_text);
        emit_file(a.lock, a.lock_text);
        emit_file(a.digest, a.digest_text);
        emit_file(a.sproto, a.sproto_text);
        emit_file(a.bindings, a.bindings_text);
        emit_file(a.javascript, a.javascript_text);
        emit_file(a.csharp, a.csharp_text);
        emit_file(a.js_client, a.js_client_text);
        emit_file(a.cs_client, a.cs_client_text);
        std::cout << "wrote "+a.lock.string() << '\n';
    }
    // The manifest ships last so a half-finished build directory is never a
    // complete bundle: load refuses directories holding unlisted files.
    write_artifact(out/"bundle.manifest", manifest.str());
    return 0;
}
}
