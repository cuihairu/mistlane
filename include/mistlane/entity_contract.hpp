// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
namespace mistlane {
// Entity contract prototype: definition-time phase of the entity-contract chain
// (docs/entity-contract.md) — parse the handwritten XML subset and enforce
// its semantic rules. It is a definition/build-time library only: it emits
// project artifacts and sproto schema, but has no runtime wiring or
// persistence. Wire messages stay binary; this header never sees one.
//
//   <Contracts>
//     <Types>
//       <Item><itemId id="0" type="UINT32"/><count id="1" type="UINT32" default="1"/></Item>
//     </Types>
//     <Entity name="Avatar" id="1">
//       <Properties>
//         <level id="1" type="UINT16" default="1" flags="OWN_CLIENT" persistent="true"/>
//       </Properties>
//       <ServerMethods>
//         <Method name="attack" id="1" mode="notify" caller="owner">
//           <Arg name="target" id="0" type="EntityRef"/>
//         </Method>
//       </ServerMethods>
//       <ClientMethods>
//         <Method name="onReward" id="1" mode="notify" audience="owner"/>
//       </ClientMethods>
//     </Entity>
//   </Contracts>
//
// Rules enforced (each traceable to the design page): explicit stable ids
// that traversal order never renumbers; ids unique per entity property, per
// side's methods and per structure/argument list; no duplicate member names
// across an entity's groups; closed type/flag/mode/caller vocabularies;
// typed defaults; missing flags mean SERVER_ONLY and unknown flags are
// errors; ARRAY needs "of"; struct references resolve and do not recurse;
// request methods declare returns and client methods are notify-only; DOCTYPE
// and external entities are refused outright (XML is data, not code).
struct ContractError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct ContractField {
    std::string name, type, of, default_value;
    unsigned id = 0;
};
using ContractArgs = std::vector<ContractField>; // args and return fields share a shape
struct ContractMethod {
    std::string name, mode, caller; // caller holds the audience for client methods
    unsigned id = 0;
    ContractArgs args, returns;
};
struct ContractProperty {
    std::string name, type, of, default_value, flags;
    unsigned id = 0;
    bool persistent = false;
};
struct ContractEntity {
    std::string name;
    unsigned id = 0;
    std::vector<ContractProperty> properties;
    std::vector<ContractMethod> server_methods, client_methods;
    // Retired ids: recorded so they can never be reused by another meaning.
    std::vector<unsigned> reserved_properties, reserved_server_methods, reserved_client_methods;
};
// One flattened release-lock line: a struct field, an entity, a property, a
// method, one of a method's args or returns, or a reservation. `spec` is the
// line's type-shaped meaning ("UINT16", "Inventory", "ARRAY of Item", a
// method's "mode caller"); `sub` holds the qualifier the key needs beyond
// kind/owner/name/id — a method's side, or "<side> <method-id>" for an arg or
// return. Reservations carry the retired id in `id` and no name.
struct ContractEntry {
    std::string kind, owner, name, sub, spec;
    unsigned id = 0;
    auto key() const { return std::tuple(kind, owner, name, sub, id); }
    bool operator==(const ContractEntry&) const = default;
};
// Publication gate verdict: breaking lines block, additive ones are the
// compatible subset.
struct CompatDiff {
    std::vector<std::string> breaking, additive;
    bool compatible() const { return breaking.empty(); }
};
class EntityContract {
public:
    // A type resolver maps a <TypesSource path> to the referenced type
    // document. The XML reader never opens files, so external entities stay
    // disabled; only a resolver a caller supplies may reach the filesystem.
    explicit EntityContract(std::string_view xml,
                            std::function<std::string(const std::string&)> type_resolver = {}) {
        load(xml, std::move(type_resolver));
    }
    const std::map<std::string, ContractArgs>& structs() const { return structs_; }
    const std::vector<ContractEntity>& entities() const { return entities_; }
    // Canonical dump of the resolved model: declaration order, no renumbering,
    // no addresses — the future release lock digests exactly this shape.
    std::string describe() const {
        std::ostringstream out;
        for (const auto& [name, fields] : structs_) {
            out << "struct " << name;
            for (const auto& f : fields) field_line(out, f);
            out << '\n';
        }
        for (const auto& e : entities_) {
            out << "entity " << e.name << ' ' << e.id << '\n';
            for (const auto& p : e.properties) {
                out << " prop ";
                field_line(out, ContractField{p.name, p.type, p.of, p.default_value, p.id});
                out << ' ' << p.flags << (p.persistent ? " persistent" : "") << '\n';
            }
            for (const auto& m : e.server_methods) method_line(out, "server", m);
            for (const auto& m : e.client_methods) method_line(out, "client", m);
            for (unsigned r : e.reserved_properties) out << " reserved property " << r << '\n';
            for (unsigned r : e.reserved_server_methods) out << " reserved server " << r << '\n';
            for (unsigned r : e.reserved_client_methods) out << " reserved client " << r << '\n';
        }
        return out.str();
    }
    std::uint64_t digest() const {
        const auto text = describe();
        std::uint64_t hash = 14695981039346656037ull;
        for (unsigned char ch : text) { hash ^= ch; hash *= 1099511628211ull; }
        return hash;
    }
    // Release lock: the assignment map a published version keeps. New tags
    // allocate here only; nothing renumbers an entry that is already listed.
    std::string lock() const {
        std::ostringstream out;
        out << "# mistlane entity contract lock\n# digest "
            << std::hex << std::setw(16) << std::setfill('0') << digest() << std::dec
            << std::setfill(' ') << '\n';
        for (const auto& [name, fields] : structs_) {
            out << "struct " << name;
            for (const auto& f : fields) out << " {" << f.id << ' ' << f.name << ' ' << f.type
                << (f.of.empty() ? "" : " of "+f.of) << '}';
            out << '\n';
        }
        for (const auto& e : entities_) {
            out << "entity " << e.name << ' ' << e.id << '\n';
            for (const auto& p : e.properties)
                out << " prop " << e.name << ' ' << p.id << ' ' << p.name << ' ' << p.type
                    << (p.of.empty() ? "" : " "+p.of) << '\n';
            auto emit_methods = [&](const char* side, const std::vector<ContractMethod>& methods) {
                for (const auto& m : methods) {
                    out << " method " << e.name << ' ' << side << ' ' << m.id << ' ' << m.name << ' ' << m.mode << ' ' << m.caller << '\n';
                    for (const auto& a : m.args) out << " arg " << e.name << ' ' << side << ' ' << m.id << ' ' << a.id << ' ' << a.name << ' ' << a.type << (a.of.empty() ? "" : " "+a.of) << '\n';
                    for (const auto& r : m.returns) out << " return " << e.name << ' ' << side << ' ' << m.id << ' ' << r.id << ' ' << r.name << ' ' << r.type << (r.of.empty() ? "" : " "+r.of) << '\n';
                }
            };
            emit_methods("server", e.server_methods);
            emit_methods("client", e.client_methods);
            for (unsigned r : e.reserved_properties) out << " reserved " << e.name << " property " << r << '\n';
            for (unsigned r : e.reserved_server_methods) out << " reserved " << e.name << " server " << r << '\n';
            for (unsigned r : e.reserved_client_methods) out << " reserved " << e.name << " client " << r << '\n';
        }
        return out.str();
    }
    // Flattened model view: the same entries parse_lock() reads back, so a
    // live contract and its published lock compare equal under compat_diff().
    std::vector<ContractEntry> entries() const {
        std::vector<ContractEntry> out;
        const auto spec_of = [](const auto& f) {
            return f.of.empty() ? f.type : f.type + " of " + f.of;
        };
        for (const auto& [name, fields] : structs_)
            for (const auto& f : fields)
                out.push_back({"struct", name, f.name, "", spec_of(f), f.id});
        for (const auto& e : entities_) {
            out.push_back({"entity", "", e.name, "", std::to_string(e.id), e.id});
            for (const auto& p : e.properties)
                out.push_back({"prop", e.name, p.name, "", spec_of(p), p.id});
            const auto emit = [&](const char* side, const std::vector<ContractMethod>& methods) {
                for (const auto& m : methods) {
                    out.push_back({"method", e.name, m.name, side, m.mode + " " + m.caller, m.id});
                    for (const auto& a : m.args)
                        out.push_back({"arg", e.name, a.name,
                            std::string(side)+" "+std::to_string(m.id), spec_of(a), a.id});
                    for (const auto& r : m.returns)
                        out.push_back({"return", e.name, r.name,
                            std::string(side)+" "+std::to_string(m.id), spec_of(r), r.id});
                }
            };
            emit("server", e.server_methods);
            emit("client", e.client_methods);
            for (unsigned r : e.reserved_properties) out.push_back({"reserved-property", e.name, "", "", "", r});
            for (unsigned r : e.reserved_server_methods) out.push_back({"reserved-server", e.name, "", "", "", r});
            for (unsigned r : e.reserved_client_methods) out.push_back({"reserved-client", e.name, "", "", "", r});
        }
        std::sort(out.begin(), out.end(),
                  [](const ContractEntry& a, const ContractEntry& b) { return a.key() < b.key(); });
        return out;
    }
    // Project-specific schema generated from the resolved model: a checker for
    // editor support and strict validation, with each member's id and type
    // pinned so no renumber can ever pass the schema. Semantic rules (flags
    // defaults, typed defaults, recursion) stay in the checker.
    std::string xsd() const {
        std::ostringstream out;
        out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            << "<!-- Generated by EntityContract; do not edit by hand. -->\n"
            << "<xs:schema xmlns:xs=\"http://www.w3.org/2001/XMLSchema\">\n";
        enum_block(out, "Persistent", {"true", "false"});
        enum_block(out, "ReplicationFlags", {"OWN_CLIENT", "ALL_CLIENTS", "SERVER_ONLY"});
        {
            const std::vector<std::string> types{"BOOL", "INT32", "UINT16", "UINT32",
                "INT64", "STRING", "BYTES", "EntityRef", "ARRAY"};
            std::vector<std::string> all(types.begin(), types.end());
            for (const auto& [name, fields] : structs_) all.push_back(name);
            enum_block(out, "MemberType", all);
        }
        enum_block(out, "MethodMode", {"notify", "request"});
        enum_block(out, "ServerCaller", {"owner", "server"});
        enum_block(out, "ClientAudience", {"owner"});
        out << "  <xs:element name=\"Contracts\"><xs:complexType><xs:sequence>";
        out << "<xs:element name=\"TypesSource\" minOccurs=\"0\" maxOccurs=\"1\">"
            << "<xs:complexType><xs:attribute name=\"path\" use=\"required\"/></xs:complexType>"
            << "</xs:element>";
        if (!structs_.empty()) {
            out << "<xs:element name=\"Types\" minOccurs=\"0\" maxOccurs=\"1\"><xs:complexType>"
                << "<xs:choice>";
            for (const auto& [name, fields] : structs_) {
                out << "<xs:element name=\"" << name << "\" minOccurs=\"1\" maxOccurs=\"1\"><xs:complexType>"
                    << "<xs:sequence>";
                for (const auto& f : fields) member_element(out, f, false);
                out << "</xs:sequence></xs:complexType></xs:element>";
            }
            out << "</xs:choice></xs:complexType></xs:element>";
        }
        for (const auto& e : entities_) entity_element(out, e);
        out << "</xs:sequence></xs:complexType></xs:element>\n</xs:schema>\n";
        return out.str();
    }
    // Deterministic sproto schema emitter. The source XML is validated by this
    // class before emission; this method only maps the already-resolved model.
    std::string sproto() const {
        auto ident = [](const std::string& s) {
            auto alpha = [](unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
            auto digit = [](unsigned char c) { return c >= '0' && c <= '9'; };
            if (s.empty() || !(alpha(static_cast<unsigned char>(s[0])) || s[0] == '_'))
                throw ContractError("invalid sproto identifier: " + s);
            for (unsigned char c : s)
                if (!(alpha(c) || digit(c) || c == '_'))
                    throw ContractError("invalid sproto identifier: " + s);
        };
        auto type_name = [&](const std::string& name) {
            ident(name);
            return name;
        };
        std::function<std::string(const std::string&, const std::string&)> field_type;
        field_type = [&](const std::string& type, const std::string& of) {
            if (type == "ARRAY") return std::string("*") + field_type(of, {});
            if (type == "BOOL") return std::string("boolean");
            if (type == "INT32" || type == "UINT16" || type == "UINT32" ||
                type == "INT64" || type == "EntityRef") return std::string("integer");
            if (type == "STRING") return std::string("string");
            if (type == "BYTES") return std::string("binary");
            return type_name(type);
        };
        std::ostringstream out;
        auto fields = [&](const ContractArgs& list) {
            for (const auto& f : list) { ident(f.name); out << "\t" << f.name << " " << f.id << " : " << field_type(f.type, f.of) << '\n'; }
        };
        for (const auto& [name, members] : structs_) {
            type_name(name); out << "." << name << " {\n"; fields(members); out << "}\n\n";
        }
        for (const auto& e : entities_) {
            type_name(e.name); out << "." << e.name << " {\n";
            for (const auto& p : e.properties) { ident(p.name); out << "\t" << p.name << " " << p.id << " : " << field_type(p.type, p.of) << '\n'; }
            out << "}\n\n";
            auto methods = [&](const char* side, const std::vector<ContractMethod>& list) {
                for (const auto& m : list) {
                    ident(m.name);
                    // cloudwu/sproto packs protocol tags into a 16-bit parser
                    // field as (tag + 1) * 2, so the largest supported tag is
                    // 32766. Reserve 9 low bits for side + method id.
                    if (e.id > 63 || m.id > 254)
                        throw ContractError("sproto protocol tag range exceeded for " + e.name + "." + m.name);
                    const unsigned long long side_id = std::string_view(side) == "Client" ? 1ull : 0ull;
                    const unsigned long long tag = static_cast<unsigned long long>(e.id) * 512ull + side_id * 256ull + m.id;
                    const auto protocol = "E" + std::to_string(e.id) + "_" + side + "_" + std::to_string(m.id);
                    out << protocol << " " << tag << " {\n\trequest {\n"; fields(m.args); out << "\t}\n";
                    if (m.mode == "request") { out << "\tresponse {\n"; fields(m.returns); out << "\t}\n"; }
                    out << "}\n\n";
                }
            };
            methods("Server", e.server_methods); methods("Client", e.client_methods);
        }
        return out.str();
    }
    // Stable C++-side binding descriptor table. It intentionally contains
    // contract identifiers only; implementation handlers are supplied at runtime.
    std::string cpp_bindings() const {
        std::ostringstream out;
        out << "// Generated by mistlane contract build; do not edit.\n#pragma once\n#include <cstdint>\n#include <string>\n#include <vector>\n#include <utility>\n#include \"mistlane/entity_dispatch.hpp\"\nnamespace mistlane::generated {\n"
               "template<class T> using Array = std::vector<T>;\n";
        std::function<std::string(const std::string&, const std::string&)> cpp_type;
        cpp_type = [&](const std::string& type, const std::string& of) {
            if (type == "ARRAY") return std::string("Array<") + cpp_type(of, {}) + ">";
            if (type == "BOOL") return std::string("bool");
            if (type == "INT32") return std::string("std::int32_t");
            if (type == "UINT16") return std::string("std::uint16_t");
            if (type == "UINT32") return std::string("std::uint32_t");
            if (type == "INT64" || type == "EntityRef") return std::string("std::int64_t");
            if (type == "STRING" || type == "BYTES") return std::string("std::string");
            return type;
        };
        for (const auto& [name, fields] : structs_) {
            out << "struct " << name << " {\n";
            for (const auto& f : fields) out << "  " << cpp_type(f.type, f.of) << " " << f.name << ";\n";
            out << "};\n";
        }
        for (const auto& e : entities_) {
            auto emit_args = [&](const char* side, const std::vector<ContractMethod>& methods) {
                for (const auto& m : methods) {
                    out << "struct " << e.name << "_" << side << "_" << m.name << "Args {\n";
                    for (const auto& f : m.args) out << "  " << cpp_type(f.type, f.of) << " " << f.name << ";\n";
                    out << "};\n";
                }
            };
            emit_args("server", e.server_methods); emit_args("client", e.client_methods);
        }
        out << "struct MethodBinding { const char* entity; std::uint32_t entity_id; const char* side; const char* name; std::uint16_t method_id; const char* mode; const char* caller; };\n"
               "inline constexpr MethodBinding methods[] = {\n";
        for (const auto& e : entities_) {
            auto emit = [&](const char* side, const std::vector<ContractMethod>& methods) {
                for (const auto& m : methods)
                    out << "  {\"" << e.name << "\", " << e.id << ", \"" << side << "\", \"" << m.name << "\", " << m.id << ", \"" << m.mode << "\", \"" << m.caller << "\"},\n";
            };
            emit("server", e.server_methods); emit("client", e.client_methods);
        }
        out << "};\n";
        for (const auto& e : entities_) {
            auto emit = [&](const char* side, const std::vector<ContractMethod>& methods) {
                for (const auto& m : methods) {
                    const auto binding = e.name + "_" + side + "_" + m.name;
                    out << "inline mistlane::EntityDispatchResult call_" << binding << "(const mistlane::EntityDispatcher& d, mistlane::EntityTarget target, mistlane::EntityCaller caller, mistlane::EntityValues args) { return d.dispatch({target, caller, \"" << side << "\", " << m.id << ", true, std::move(args)}); }\n";
                    out << "inline void bind_" << binding << "(mistlane::EntityDispatcher& d, mistlane::EntityDispatcher::Handler h) { d.bind(\"" << e.name << "\", \"" << side << "\", " << m.id << ", std::move(h)); }\n";
                }
            };
            emit("server", e.server_methods); emit("client", e.client_methods);
        }
        out << "}\n";
        return out.str();
    }
    std::string javascript_bindings(bool client = false) const {
        std::ostringstream out;
        out << "// Generated by mistlane contract build; do not edit.\n";
        if (client)
            out << "// Client audience: only OWN_CLIENT/ALL_CLIENTS properties and\n"
                   "// owner-callable or client methods; SERVER_ONLY properties and\n"
                   "// server-internal (caller=\"server\") methods stay out. The wire\n"
                   "// codec body is byte-identical to the server artifact's.\n";
        // The audience filter: clients see replicated properties and the
        // methods they may invoke; everything else is server-internal.
        auto client_visible = [client](const ContractProperty& p) {
            return !client || p.flags == "OWN_CLIENT" || p.flags == "ALL_CLIENTS";
        };
        auto client_callable = [client](const ContractMethod& m) {
            return !client || m.caller == "owner";
        };
        auto js_type = [](const std::string& t, const std::string& of) {
            if (t == "ARRAY") return "Array<" + of + ">";
            if (t == "BOOL") return std::string("boolean");
            if (t == "STRING") return std::string("string");
            if (t == "BYTES") return std::string("Uint8Array");
            if (t == "INT64" || t == "EntityRef") return std::string("bigint");
            if (t == "UINT16" || t == "UINT32" || t == "INT32") return std::string("number");
            return t;
        };
        auto emit = [&](const std::string& name, const ContractArgs& fields) {
            out << "/** @typedef {{";
            for (const auto& f : fields) out << " " << f.name << ": " << js_type(f.type, f.of) << ";";
            out << " }} " << name << " */\n";
        };
        auto properties_of = [&](const ContractEntity& e) {
            ContractArgs properties;
            for (const auto& p : e.properties)
                if (client_visible(p)) properties.push_back({p.name,p.type,p.of,p.default_value,p.id});
            return properties;
        };
        for (const auto& [name, fields] : structs_) emit(name, fields);
        for (const auto& e : entities_) {
            emit(e.name, properties_of(e));
            for (const auto& m : e.server_methods) if (client_callable(m)) emit(e.name+"_"+m.name+"_Args", m.args);
            for (const auto& m : e.client_methods) emit(e.name+"_"+m.name+"_Args", m.args);
        }
        out << "\nexport const sprotoFields = {\n";
        auto emit_fields = [&](const std::string& name, const ContractArgs& fields) {
            out << "  " << name << ": [";
            for (std::size_t i=0;i<fields.size();++i) {
                const std::string kind = field_kind(fields[i]);
                if (i) out << ", ";
                out << "{ id: " << fields[i].id << ", name: '" << fields[i].name << "', kind: '" << kind << "'";
                if (kind == "struct") out << ", of: '" << fields[i].type << "'";
                else if (kind == "struct-array") out << ", of: '" << fields[i].of << "'";
                out << " }";
            }
            out << "],\n";
        };
        for (const auto& [name, fields] : structs_) emit_fields(name, fields);
        for (const auto& e : entities_) {
            emit_fields(e.name, properties_of(e));
            for (const auto& m : e.server_methods) if (client_callable(m)) {
                emit_fields(e.name+"_"+m.name+"_Args", m.args);
                if (!m.returns.empty()) emit_fields(e.name+"_"+m.name+"_Returns", m.returns);
            }
            for (const auto& m : e.client_methods) emit_fields(e.name+"_"+m.name+"_Args", m.args);
        }
        out << "};\n\n"
               "// Standard sproto codec for the generated tag tables. Integers use the\n"
               "// canonical wire forms: an in-band short word for 0..0x7ffe, a 4-byte little-\n"
               "// endian int32, and an 8-byte two's-complement int64. Struct fields are\n"
               "// length-prefixed records, integer arrays carry an intlen byte before 4- or\n"
               "// 8-byte elements, and arrays of structs are per-element [len][record].\n"
               "// Booleans ride the integer in-band word (true=4, false=2), STRING/BYTES\n"
               "// fields are length-prefixed byte objects, boolean arrays are one raw\n"
               "// 0/1 byte per element with no intlen byte, and string/bytes arrays are\n"
               "// per-element [len][bytes]; STRING payloads decode through a fatal UTF-8\n"
               "// decoder so invalid sequences are rejected, not silently replaced.\n"
               "// Values cross as bigint when they do not fit a JS Number. Encoding matches\n"
               "// upstream cloudwu/sproto byte for byte; decode skips tags absent from the\n"
               "// table, rejects malformed buffers, and widens every dword from unsigned\n"
               "// bytes instead of upstream's todword int arithmetic, so negative wide\n"
               "// integers survive.\n"
               "const INT64_MIN = -9223372036854775808n;\n"
               "const INT64_MAX = 9223372036854775807n;\n"
               "const INT32_MIN = -2147483648n;\n"
               "const INT32_MAX = 2147483647n;\n"
               "const utf8 = new TextEncoder();\n"
               "const utf8Strict = new TextDecoder('utf-8', { fatal: true });\n\n"
               "function sprotoAsInt64(value, name) {\n"
               "  if (typeof value === 'bigint') return value < INT64_MIN || value > INT64_MAX ? null : value;\n"
               "  return Number.isInteger(value) ? BigInt(value) : null;\n"
               "}\n\n"
               "export function encodeRecord(fields, value) {\n"
               "  const tags = [], data = [];\n"
               "  const push32 = (word) => data.push(word & 255, (word >>> 8) & 255, (word >>> 16) & 255, (word >>> 24) & 255);\n"
               "  let last = -1;\n"
               "  for (const field of [...fields].sort((a, b) => a.id - b.id)) {\n"
               "    if (!(field.name in value)) continue;\n"
               "    const gap = field.id - last - 1;\n"
               "    if (gap) tags.push((gap - 1) * 2 + 1);\n"
               "    last = field.id;\n"
               "    if (field.kind === 'int') {\n"
               "      const n = sprotoAsInt64(value[field.name], field.name);\n"
               "      if (n === null) throw new RangeError(`${field.name} is not an integer`);\n"
               "      if (n >= 0n && n < 0x7fffn) {\n"
               "        tags.push(Number(n + 1n) * 2);\n"
               "      } else {\n"
               "        const wide = n < INT32_MIN || n > INT32_MAX;\n"
               "        tags.push(0);\n"
               "        push32(wide ? 8 : 4);\n"
               "        push32(Number(n & 0xffffffffn));\n"
               "        if (wide) push32(Number((n >> 32n) & 0xffffffffn));\n"
               "      }\n"
               "    } else if (field.kind === 'struct') {\n"
               "      tags.push(0);\n"
               "      const content = encodeRecord(sprotoFields[field.of], value[field.name]);\n"
               "      push32(content.length);\n"
               "      data.push(...content);\n"
               "    } else if (field.kind === 'int-array') {\n"
               "      tags.push(0);\n"
               "      const list = value[field.name];\n"
               "      if (!Array.isArray(list)) throw new RangeError(`${field.name} is not an array`);\n"
               "      const ints = list.map((element) => {\n"
               "        const n = sprotoAsInt64(element, field.name);\n"
               "        if (n === null) throw new RangeError(`${field.name} element is not an integer`);\n"
               "        return n;\n"
               "      });\n"
               "      const wide = ints.some((n) => n < INT32_MIN || n > INT32_MAX);\n"
               "      const content = [];\n"
               "      if (ints.length > 0) {\n"
               "        content.push(wide ? 8 : 4);\n"
               "        for (const n of ints) for (let i = 0; i < (wide ? 8 : 4); i += 1) content.push(Number((n >> BigInt(8 * i)) & 0xffn));\n"
               "      }\n"
               "      push32(content.length);\n"
               "      data.push(...content);\n"
               "    } else if (field.kind === 'struct-array') {\n"
               "      tags.push(0);\n"
               "      const list = value[field.name];\n"
               "      if (!Array.isArray(list)) throw new RangeError(`${field.name} is not an array`);\n"
               "      const content = [];\n"
               "      for (const item of list) {\n"
               "        const record = encodeRecord(sprotoFields[field.of], item);\n"
               "        content.push(record.length & 255, (record.length >>> 8) & 255, (record.length >>> 16) & 255, (record.length >>> 24) & 255);\n"
               "        content.push(...record);\n"
               "      }\n"
               "      push32(content.length);\n"
               "      data.push(...content);\n"
               "    } else if (field.kind === 'bool') {\n"
               "      const flag = value[field.name];\n"
               "      if (typeof flag !== 'boolean') throw new RangeError(`${field.name} is not a boolean`);\n"
               "      tags.push(flag ? 4 : 2);\n"
               "    } else if (field.kind === 'string') {\n"
               "      const text = value[field.name];\n"
               "      if (typeof text !== 'string') throw new RangeError(`${field.name} is not a string`);\n"
               "      tags.push(0);\n"
               "      const encoded = utf8.encode(text);\n"
               "      push32(encoded.length);\n"
               "      data.push(...encoded);\n"
               "    } else if (field.kind === 'bytes') {\n"
               "      const raw = value[field.name];\n"
               "      if (!(raw instanceof Uint8Array)) throw new RangeError(`${field.name} is not bytes`);\n"
               "      tags.push(0);\n"
               "      push32(raw.length);\n"
               "      data.push(...raw);\n"
               "    } else if (field.kind === 'bool-array') {\n"
               "      tags.push(0);\n"
               "      const list = value[field.name];\n"
               "      if (!Array.isArray(list)) throw new RangeError(`${field.name} is not an array`);\n"
               "      const content = list.map((flag) => {\n"
               "        if (typeof flag !== 'boolean') throw new RangeError(`${field.name} element is not a boolean`);\n"
               "        return flag ? 1 : 0;\n"
               "      });\n"
               "      push32(content.length);\n"
               "      data.push(...content);\n"
               "    } else if (field.kind === 'string-array') {\n"
               "      tags.push(0);\n"
               "      const list = value[field.name];\n"
               "      if (!Array.isArray(list)) throw new RangeError(`${field.name} is not an array`);\n"
               "      const content = [];\n"
               "      for (const text of list) {\n"
               "        if (typeof text !== 'string') throw new RangeError(`${field.name} element is not a string`);\n"
               "        const encoded = utf8.encode(text);\n"
               "        content.push(encoded.length & 255, (encoded.length >>> 8) & 255, (encoded.length >>> 16) & 255, (encoded.length >>> 24) & 255);\n"
               "        content.push(...encoded);\n"
               "      }\n"
               "      push32(content.length);\n"
               "      data.push(...content);\n"
               "    } else if (field.kind === 'bytes-array') {\n"
               "      tags.push(0);\n"
               "      const list = value[field.name];\n"
               "      if (!Array.isArray(list)) throw new RangeError(`${field.name} is not an array`);\n"
               "      const content = [];\n"
               "      for (const raw of list) {\n"
               "        if (!(raw instanceof Uint8Array)) throw new RangeError(`${field.name} element is not bytes`);\n"
               "        content.push(raw.length & 255, (raw.length >>> 8) & 255, (raw.length >>> 16) & 255, (raw.length >>> 24) & 255);\n"
               "        content.push(...raw);\n"
               "      }\n"
               "      push32(content.length);\n"
               "      data.push(...content);\n"
               "    } else {\n"
               "      throw new RangeError(`unsupported field type ${field.kind}`);\n"
               "    }\n"
               "  }\n"
               "  const out = [tags.length & 255, (tags.length >>> 8) & 255];\n"
               "  for (const tag of tags) out.push(tag & 255, (tag >>> 8) & 255);\n"
               "  out.push(...data);\n"
               "  return Uint8Array.from(out);\n"
               "}\n\n"
               "export function decodeRecord(fields, bytes) {\n"
               "  if (!(bytes instanceof Uint8Array) || bytes.length < 2) throw new RangeError('short header');\n"
               "  const byTag = new Map(fields.map((field) => [field.id, field]));\n"
               "  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.length);\n"
               "  const count = view.getUint16(0, true);\n"
               "  if (bytes.length < 2 + 2 * count) throw new RangeError('short tag block');\n"
               "  let tagPos = 2;\n"
               "  let dataPos = 2 + 2 * count;\n"
               "  let tag = -1;\n"
               "  const out = {};\n"
               "  for (let i = 0; i < count; i += 1) {\n"
               "    const word = view.getUint16(tagPos, true);\n"
               "    tagPos += 2;\n"
               "    tag += 1;\n"
               "    if (word & 1) { tag += (word - 1) / 2; continue; }\n"
               "    const field = byTag.get(tag);\n"
               "    if (word !== 0) {\n"
               "      if (field && field.kind !== 'int' && field.kind !== 'bool') throw new RangeError(`in-band value for non-integer ${field.name}`);\n"
               "      if (field && field.kind === 'bool') {\n"
               "        const flag = word / 2 - 1;\n"
               "        if (flag !== 0 && flag !== 1) throw new RangeError(`bad boolean ${field.name}`);\n"
               "        out[field.name] = flag === 1;\n"
               "      } else if (field) out[field.name] = BigInt(word / 2 - 1);\n"
               "      continue;\n"
               "    }\n"
               "    if (bytes.length - dataPos < 4) throw new RangeError('truncated value');\n"
               "    const length = view.getUint32(dataPos, true);\n"
               "    const start = dataPos + 4;\n"
               "    if (bytes.length - start < length) throw new RangeError('truncated value');\n"
               "    if (field && field.kind === 'int') {\n"
               "      if (length !== 4 && length !== 8) throw new RangeError('bad value span');\n"
               "      let n = BigInt(view.getUint32(start, true));\n"
               "      if (length === 8) n |= BigInt(view.getUint32(start + 4, true)) << 32n;\n"
               "      else if (n & 0x80000000n) n -= 1n << 32n;\n"
               "      if (n >= (1n << 63n)) n -= 1n << 64n;\n"
               "      out[field.name] = n;\n"
               "    } else if (field && field.kind === 'struct') {\n"
               "      out[field.name] = decodeRecord(sprotoFields[field.of], bytes.subarray(start, start + length));\n"
               "    } else if (field && field.kind === 'bool') {\n"
               "      if (length !== 4 && length !== 8) throw new RangeError('bad value span');\n"
               "      let n = BigInt(view.getUint32(start, true));\n"
               "      if (length === 8) n |= BigInt(view.getUint32(start + 4, true)) << 32n;\n"
               "      out[field.name] = n !== 0n;\n"
               "    } else if (field && field.kind === 'string') {\n"
               "      try {\n"
               "        out[field.name] = utf8Strict.decode(bytes.subarray(start, start + length));\n"
               "      } catch (error) { throw new RangeError('invalid utf-8 for ' + field.name); }\n"
               "    } else if (field && field.kind === 'bytes') {\n"
               "      out[field.name] = bytes.slice(start, start + length);\n"
               "    } else if (field && field.kind === 'int-array') {\n"
               "      out[field.name] = [];\n"
               "      let p = start;\n"
               "      const end = start + length;\n"
               "      if (p < end) {\n"
               "        const intlen = bytes[p];\n"
               "        p += 1;\n"
               "        if ((intlen !== 4 && intlen !== 8) || (end - p) % intlen !== 0) throw new RangeError('bad array layout');\n"
               "        while (p < end) {\n"
               "          let n = BigInt(view.getUint32(p, true));\n"
               "          if (intlen === 8) n |= BigInt(view.getUint32(p + 4, true)) << 32n;\n"
               "          else if (n & 0x80000000n) n -= 1n << 32n;\n"
               "          if (n >= (1n << 63n)) n -= 1n << 64n;\n"
               "          out[field.name].push(n);\n"
               "          p += intlen;\n"
               "        }\n"
               "      }\n"
               "    } else if (field && field.kind === 'struct-array') {\n"
               "      out[field.name] = [];\n"
               "      let p = start;\n"
               "      const end = start + length;\n"
               "      while (p < end) {\n"
               "        if (end - p < 4) throw new RangeError('bad array layout');\n"
               "        const recordLength = view.getUint32(p, true);\n"
               "        if (end - p - 4 < recordLength) throw new RangeError('bad array layout');\n"
               "        out[field.name].push(decodeRecord(sprotoFields[field.of], bytes.subarray(p + 4, p + 4 + recordLength)));\n"
               "        p += 4 + recordLength;\n"
               "      }\n"
               "    } else if (field && field.kind === 'bool-array') {\n"
               "      out[field.name] = [];\n"
               "      for (let p = start; p < start + length; p += 1) {\n"
               "        const flag = bytes[p];\n"
               "        if (flag > 1) throw new RangeError('bad boolean array');\n"
               "        out[field.name].push(flag === 1);\n"
               "      }\n"
               "    } else if (field && field.kind === 'string-array') {\n"
               "      out[field.name] = [];\n"
               "      let p = start;\n"
               "      const end = start + length;\n"
               "      while (p < end) {\n"
               "        if (end - p < 4) throw new RangeError('bad array layout');\n"
               "        const itemLength = view.getUint32(p, true);\n"
               "        if (end - p - 4 < itemLength) throw new RangeError('bad array layout');\n"
               "        try {\n"
               "          out[field.name].push(utf8Strict.decode(bytes.subarray(p + 4, p + 4 + itemLength)));\n"
               "        } catch (error) { throw new RangeError('invalid utf-8 for ' + field.name); }\n"
               "        p += 4 + itemLength;\n"
               "      }\n"
               "    } else if (field && field.kind === 'bytes-array') {\n"
               "      out[field.name] = [];\n"
               "      let p = start;\n"
               "      const end = start + length;\n"
               "      while (p < end) {\n"
               "        if (end - p < 4) throw new RangeError('bad array layout');\n"
               "        const itemLength = view.getUint32(p, true);\n"
               "        if (end - p - 4 < itemLength) throw new RangeError('bad array layout');\n"
               "        out[field.name].push(bytes.slice(p + 4, p + 4 + itemLength));\n"
               "        p += 4 + itemLength;\n"
               "      }\n"
               "    } else if (field) {\n"
               "      throw new RangeError(`unsupported field type ${field.kind}`);\n"
               "    }\n"
               "    dataPos = start + length;\n"
               "  }\n"
               "  if (dataPos !== bytes.length) throw new RangeError('trailing bytes');\n"
               "  return out;\n"
               "}\n\n"
               "// Invocation envelope (docs/entity-contract.md): a fixed big-endian\n"
               "// header riding the existing bounded frames, followed by the payload as\n"
               "// the remainder. Fields in wire order: scope u32, type_id u32,\n"
               "// entity_id i64, incarnation u32, side u8 (0 = client, 1 = server),\n"
               "// method_id u16, correlation_id u64, hosting_generation u32,\n"
               "// deadline_ms u32. The envelope carries no caller identity:\n"
               "// authentication rides the connection. entity_id and correlation_id\n"
               "// cross as bigint; narrower fields accept bigint or Number.\n"
               "export const ENTITY_ENVELOPE_HEADER = 39;\n"
               "export const ENTITY_ERRORS = ['INVALID', 'UNROUTED', 'STALE', 'FORBIDDEN', 'TIMEOUT',\n"
               "  'CANCELLED', 'NO_IMPLEMENTATION', 'NOT_READY', 'CAPACITY', 'CONFLICT'];\n\n"
               "export function encodeEnvelope(fields, payload) {\n"
               "  const body = payload ?? new Uint8Array(0);\n"
               "  if (!(body instanceof Uint8Array)) throw new RangeError('payload is not bytes');\n"
               "  const asUint32 = (value, name) => {\n"
               "    const n = sprotoAsInt64(value, name);\n"
               "    if (n === null || n < 0n || n > 0xffffffffn) throw new RangeError(`${name} is not a u32`);\n"
               "    return Number(n);\n"
               "  };\n"
               "  const asInt64Wire = (value, name) => {\n"
               "    const n = sprotoAsInt64(value, name);\n"
               "    if (n === null) throw new RangeError(`${name} is not an integer`);\n"
               "    return BigInt.asUintN(64, n);\n"
               "  };\n"
               "  const asUint64Wire = (value, name) => {\n"
               "    const n = typeof value === 'bigint' ? value : Number.isInteger(value) ? BigInt(value) : null;\n"
               "    if (n === null || n < 0n || n > 0xffffffffffffffffn) throw new RangeError(`${name} is not a u64`);\n"
               "    return n;\n"
               "  };\n"
               "  if (asUint32(fields.side, 'side') > 1) throw new RangeError('side must be 0 or 1');\n"
               "  const out = new Uint8Array(ENTITY_ENVELOPE_HEADER + body.length);\n"
               "  const view = new DataView(out.buffer);\n"
               "  view.setUint32(0, asUint32(fields.scope, 'scope'), false);\n"
               "  view.setUint32(4, asUint32(fields.type_id, 'type_id'), false);\n"
               "  view.setBigUint64(8, asInt64Wire(fields.entity_id, 'entity_id'), false);\n"
               "  view.setUint32(16, asUint32(fields.incarnation, 'incarnation'), false);\n"
               "  view.setUint8(20, asUint32(fields.side, 'side'));\n"
               "  view.setUint16(21, asUint32(fields.method_id, 'method_id'), false);\n"
               "  view.setBigUint64(23, asUint64Wire(fields.correlation_id, 'correlation_id'), false);\n"
               "  view.setUint32(31, asUint32(fields.hosting_generation, 'hosting_generation'), false);\n"
               "  view.setUint32(35, asUint32(fields.deadline_ms, 'deadline_ms'), false);\n"
               "  out.set(body, ENTITY_ENVELOPE_HEADER);\n"
               "  return out;\n"
               "}\n\n"
               "export function decodeEnvelope(bytes) {\n"
               "  if (!(bytes instanceof Uint8Array) || bytes.length < ENTITY_ENVELOPE_HEADER) return null;\n"
               "  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);\n"
               "  if (view.getUint8(20) > 1) return null;\n"
               "  return {\n"
               "    scope: view.getUint32(0, false),\n"
               "    type_id: view.getUint32(4, false),\n"
               "    entity_id: BigInt.asIntN(64, view.getBigUint64(8, false)),\n"
               "    incarnation: view.getUint32(16, false),\n"
               "    side: view.getUint8(20),\n"
               "    method_id: view.getUint16(21, false),\n"
               "    correlation_id: view.getBigUint64(23, false),\n"
               "    hosting_generation: view.getUint32(31, false),\n"
               "    deadline_ms: view.getUint32(35, false),\n"
               "    payload: bytes.slice(ENTITY_ENVELOPE_HEADER),\n"
               "  };\n"
               "}\n\n"
               "// A request answers with exactly one of the typed returns or a single\n"
               "// closed error, never both: kind 0 prefixes the returns payload, kind 1\n"
               "// carries one u8 error code; only INVALID may append a u16-length\n"
               "// reason, which decodes through the fatal UTF-8 reader — the reason\n"
               "// rides as text or the reply is malformed. Unknown kinds and codes\n"
               "// are malformed, never mapped.\n"
               "export function encodeReply(reply) {\n"
               "  if (!reply.error) {\n"
               "    const body = reply.payload ?? new Uint8Array(0);\n"
               "    if (!(body instanceof Uint8Array)) throw new RangeError('payload is not bytes');\n"
               "    const out = new Uint8Array(1 + body.length);\n"
               "    out.set(body, 1);\n"
               "    return out;\n"
               "  }\n"
               "  const code = ENTITY_ERRORS.indexOf(reply.error);\n"
               "  if (code < 0) throw new RangeError(`unknown error ${reply.error}`);\n"
               "  if (reply.reason && reply.error !== 'INVALID') throw new RangeError('only INVALID may carry a reason');\n"
               "  const reason = utf8.encode(reply.reason ?? '');\n"
               "  if (reason.length > 0xffff) throw new RangeError('reason too long');\n"
               "  const out = new Uint8Array(reply.error === 'INVALID' ? 4 + reason.length : 2);\n"
               "  out[0] = 1;\n"
               "  out[1] = code;\n"
               "  if (reply.error === 'INVALID') {\n"
               "    out[2] = reason.length >> 8;\n"
               "    out[3] = reason.length & 255;\n"
               "    out.set(reason, 4);\n"
               "  }\n"
               "  return out;\n"
               "}\n\n"
               "export function decodeReply(bytes) {\n"
               "  if (!(bytes instanceof Uint8Array) || bytes.length === 0) return null;\n"
               "  if (bytes[0] === 0) return { error: null, reason: '', payload: bytes.slice(1) };\n"
               "  if (bytes[0] !== 1 || bytes.length < 2) return null;\n"
               "  const code = bytes[1];\n"
               "  if (code >= ENTITY_ERRORS.length) return null;\n"
               "  const error = ENTITY_ERRORS[code];\n"
               "  if (error === 'INVALID') {\n"
               "    if (bytes.length < 4) return null;\n"
               "    const length = (bytes[2] << 8) | bytes[3];\n"
               "    if (bytes.length - 4 !== length) return null;\n"
               "    try {\n"
               "      return { error, reason: utf8Strict.decode(bytes.subarray(4)), payload: new Uint8Array(0) };\n"
               "    } catch (error) { return null; }\n"
               "  }\n"
               "  if (bytes.length !== 2) return null;\n"
               "  return { error, reason: '', payload: new Uint8Array(0) };\n"
               "}\n\n"
               "// Generated per-entity method tables: outbound is what this\n"
               "// audience may send (server methods), inbound is what it\n"
               "// receives (client notifies). The client facade binds method\n"
               "// names straight off its own Avatar — no .server/.client\n"
               "// intermediate layer.\n"
               "export const entityTypes = {\n";
        for (const auto& e : entities_)
            out << "  " << e.name << ": " << e.id << ",\n";
        out << "};\n\nexport const entityMethods = {\n";
        for (const auto& e : entities_) {
            out << "  " << e.name << ": {\n    outbound: {";
            bool first = true;
            for (const auto& m : e.server_methods) {
                if (!client_callable(m)) continue;
                if (!first) out << ",";
                first = false;
                out << "\n      '" << m.name << "': { id: " << m.id << ", mode: '" << m.mode
                    << "', args: '" << e.name << "_" << m.name << "_Args'"
                    << ", returns: " << (m.returns.empty() ? std::string("null") : "'"+e.name+"_"+m.name+"_Returns'")
                    << " }";
            }
            out << "\n    },\n    inbound: {";
            first = true;
            for (const auto& m : e.client_methods) {
                if (!first) out << ",";
                first = false;
                out << "\n      '" << m.name << "': { id: " << m.id << ", args: '"
                    << e.name << "_" << m.name << "_Args' }";
            }
            out << "\n    },\n  },\n";
        }
        out << "};\n\n"
               "// Client-side outstanding-call discipline: the mirror of the server's\n"
               "// bounded RpcQueue. Capacity is fixed at construction, admissions are\n"
               "// typed refusals rather than silent drops, an injected clock makes\n"
               "// deadlines testable, a timed-out call stays a surfaced tombstone until\n"
               "// take() (its outcome is unknown — it may already have been applied), and\n"
               "// retry() only re-arms a call admitted as idempotent with a fresh\n"
               "// correlation id, so a non-idempotent timeout can never be silently\n"
               "// retried. Replies carry no correlation on the wire, so the transport\n"
               "// pairs a reply with the call it belongs to and passes that id here.\n"
               "export class EntityRpcQueue {\n"
               "  constructor(capacity = 64) {\n"
               "    if (!Number.isInteger(capacity) || capacity < 1) throw new RangeError('capacity must be a positive integer');\n"
               "    this.capacity = capacity;\n"
               "    this.calls = new Map();\n"
               "    this.clock = 0;\n"
               "    this.next_id = 0;\n"
               "    this.error = null;\n"
               "  }\n"
               "  call(deadline_ms, idempotent = false, tag = null, id = null) {\n"
               "    this.error = null;\n"
               "    const cid = id ?? ++this.next_id;\n"
               "    if (!Number.isInteger(cid) || cid < 1 || !Number.isInteger(deadline_ms) || deadline_ms < 1) {\n"
               "      this.error = 'INVALID';\n"
               "      return null;\n"
               "    }\n"
               "    if (this.calls.has(cid)) { this.error = 'INVALID'; return null; }\n"
               "    if (this.calls.size >= this.capacity) { this.error = 'CAPACITY'; return null; }\n"
               "    this.calls.set(cid, { state: 'PENDING', deadline: this.clock + deadline_ms,\n"
               "      idempotent, tag, response: null, error: null });\n"
               "    return cid;\n"
               "  }\n"
               "  advance(now) {\n"
               "    if (!Number.isInteger(now) || now < 0) return false;\n"
               "    this.clock = Math.max(this.clock, now);\n"
               "    for (const call of this.calls.values())\n"
               "      if (call.state === 'PENDING' && this.clock >= call.deadline) call.state = 'TIMED_OUT';\n"
               "    return true;\n"
               "  }\n"
               "  cancel(id) {\n"
               "    this.error = null;\n"
               "    const call = this.calls.get(id);\n"
               "    if (!call || call.state !== 'PENDING') { this.error = 'INVALID'; return false; }\n"
               "    call.state = 'CANCELLED';\n"
               "    return true;\n"
               "  }\n"
               "  complete(id, response = null, error = null) {\n"
               "    const call = this.calls.get(id);\n"
               "    if (!call) return 'STALE';\n"
               "    if (call.state === 'PENDING' && this.clock >= call.deadline) call.state = 'TIMED_OUT';\n"
               "    if (call.state === 'TIMED_OUT') return 'TIMEOUT';\n"
               "    if (call.state === 'CANCELLED') return 'CANCELLED';\n"
               "    if (call.state === 'OK') return 'CONFLICT';\n"
               "    call.state = 'OK';\n"
               "    call.response = response;\n"
               "    call.error = error;\n"
               "    return 'OK';\n"
               "  }\n"
               "  retry(old_id, new_deadline_ms) {\n"
               "    const call = this.calls.get(old_id);\n"
               "    if (!call || call.state !== 'TIMED_OUT' || !call.idempotent\n"
               "        || !Number.isInteger(new_deadline_ms) || new_deadline_ms < 1) return null;\n"
               "    const cid = ++this.next_id;\n"
               "    this.calls.delete(old_id);\n"
               "    this.calls.set(cid, { state: 'PENDING', deadline: this.clock + new_deadline_ms,\n"
               "      idempotent: true, tag: call.tag, response: null, error: null });\n"
               "    return cid;\n"
               "  }\n"
               "  take(id) {\n"
               "    const call = this.calls.get(id);\n"
               "    if (!call || call.state === 'PENDING') return null;\n"
               "    this.calls.delete(id);\n"
               "    return call;\n"
               "  }\n"
               "  get(id) { return this.calls.get(id) ?? null; }\n"
               "  state(id) { const call = this.calls.get(id); return call ? call.state : null; }\n"
               "  get size() { return this.calls.size; }\n"
               "}\n\n"
               "// The direct Avatar facade: one instance per (type, entity), sending\n"
               "// envelopes the caller's transport owns and decoding typed returns off\n"
               "// the method tables above. A call is admitted to the bounded queue before\n"
               "// anything is sent, so an overflow is refused rather than dropped, and\n"
               "// the admitted entry keeps the args so a retry re-encodes the same\n"
               "// payload under a fresh correlation id instead of resending blindly.\n"
               "export class EntityClient {\n"
               "  constructor(options = {}) {\n"
               "    if (typeof options.send !== 'function') throw new RangeError('send must be a function');\n"
               "    if (entityTypes[options.type] === undefined) throw new RangeError('unknown entity type ' + options.type);\n"
               "    this.send = options.send;\n"
               "    this.type = options.type;\n"
               "    this.type_id = entityTypes[options.type];\n"
               "    this.scope = options.scope ?? 0;\n"
               "    this.entity_id = options.entity_id ?? 0;\n"
               "    this.incarnation = options.incarnation ?? 0;\n"
               "    this.deadline_ms = options.deadline_ms ?? 5000;\n"
               "    this.queue = new EntityRpcQueue(options.capacity ?? 64);\n"
               "  }\n"
               "  envelope(info, args, deadline_ms, correlation_id) {\n"
               "    return encodeEnvelope({\n"
               "      scope: this.scope, type_id: this.type_id, entity_id: this.entity_id,\n"
               "      incarnation: this.incarnation, side: 0, method_id: info.id,\n"
               "      correlation_id, hosting_generation: 0, deadline_ms,\n"
               "    }, encodeRecord(sprotoFields[info.args] ?? [], args ?? {}));\n"
               "  }\n"
               "  call(method, args, options = {}) {\n"
               "    const info = entityMethods[this.type].outbound[method];\n"
               "    if (!info || info.mode !== 'request') return { ok: false, error: 'INVALID' };\n"
               "    const deadline_ms = options.deadline_ms ?? this.deadline_ms;\n"
               "    const id = this.queue.call(deadline_ms, options.idempotent ?? false, { type: this.type, method, args });\n"
               "    if (id === null) return { ok: false, error: this.queue.error };\n"
               "    this.send(this.envelope(info, args, deadline_ms, BigInt(id)));\n"
               "    return { ok: true, id };\n"
               "  }\n"
               "  notify(method, args, options = {}) {\n"
               "    const info = entityMethods[this.type].outbound[method];\n"
               "    if (!info || info.mode !== 'notify') return { ok: false, error: 'INVALID' };\n"
               "    this.send(this.envelope(info, args, options.deadline_ms ?? this.deadline_ms, 0n));\n"
               "    return { ok: true };\n"
               "  }\n"
               "  // Malformed replies drop without touching the queue: the correlation\n"
               "  // stays outstanding rather than vanishing silently.\n"
               "  on_reply(id, bytes) {\n"
               "    const reply = decodeReply(bytes);\n"
               "    if (!reply) return 'MALFORMED';\n"
               "    if (reply.error) return this.queue.complete(id, null, reply.error);\n"
               "    const entry = this.queue.get(id);\n"
               "    if (!entry) return 'STALE';\n"
               "    const info = entityMethods[entry.tag.type].outbound[entry.tag.method];\n"
               "    const response = reply.payload.length && info.returns\n"
               "      ? decodeRecord(sprotoFields[info.returns], reply.payload) : null;\n"
               "    return this.queue.complete(id, response);\n"
               "  }\n"
               "  advance(now) { return this.queue.advance(now); }\n"
               "  cancel(id) { return this.queue.cancel(id); }\n"
               "  retry(old_id, deadline_ms = this.deadline_ms) {\n"
               "    const entry = this.queue.get(old_id);\n"
               "    if (!entry || !entry.tag) return null;\n"
               "    const id = this.queue.retry(old_id, deadline_ms);\n"
               "    if (id === null) return null;\n"
               "    const info = entityMethods[entry.tag.type].outbound[entry.tag.method];\n"
               "    this.send(this.envelope(info, entry.tag.args, deadline_ms, BigInt(id)));\n"
               "    return id;\n"
               "  }\n"
               "  take(id) { return this.queue.take(id); }\n"
               "  // Inbound notifies: side must be server, the method id must be this\n"
               "  // audience's own, and the payload must decode against its table.\n"
               "  on_notify(bytes, handler) {\n"
               "    const env = decodeEnvelope(bytes);\n"
               "    if (!env || env.side !== 1) return null;\n"
               "    const hit = Object.entries(entityMethods[this.type].inbound)\n"
               "      .find(([name, info]) => info.id === env.method_id);\n"
               "    if (!hit) return null;\n"
               "    return handler(hit[0], decodeRecord(sprotoFields[hit[1].args], env.payload));\n"
               "  }\n"
               "}\n";
        out <<
               "export class PropertySync {\n"
               "  // Client-side property replica fed by server BASELINE/DELTAS frames.\n"
               "  // Mirrors the C++ SyncReceiver: per-property gapless sequences, atomic\n"
               "  // batch apply with a full rollback on any refusal, and a stale flag that\n"
               "  // only the next baseline clears. Handlers fire after the batch commits.\n"
               "  constructor(type, handlers = {}) {\n"
               "    if (!(type in entityTypes)) throw new Error('unknown entity type ' + type);\n"
               "    this.type = type;\n"
               "    this.handlers = handlers;\n"
               "    this.replica = {};\n"
               "    this.sequence = 0;\n"
               "    this.stale = false;\n"
               "  }\n"
               "  baseline(values) {\n"
               "    const next = {};\n"
               "    for (const field of sprotoFields[this.type])\n"
               "      next[field.name] = values !== undefined\n"
               "        && Object.prototype.hasOwnProperty.call(values, field.name)\n"
               "        && values[field.name] !== null && values[field.name] !== undefined\n"
               "        ? cloneValue(values[field.name]) : defaultValue(field);\n"
               "    this.replica = next;\n"
               "    this.sequence = 0;\n"
               "    this.stale = false;\n"
               "  }\n"
               "  get(property) { return this.replica[property]; }\n"
               "  apply(batch) {\n"
               "    if (this.stale || batch.length === 0) return false;\n"
               "    let expect = this.sequence + 1;\n"
               "    for (const delta of batch) {\n"
               "      if (Number(delta.sequence) !== expect) { this.stale = true; return false; }\n"
               "      expect += 1;\n"
               "    }\n"
               "    const backup = cloneValue(this.replica);\n"
               "    const events = [];\n"
               "    for (const delta of batch) {\n"
               "      const event = performDelta(this.replica, sprotoFields[this.type], delta);\n"
               "      if (!event) { this.replica = backup; this.stale = true; return false; }\n"
               "      events.push(event);\n"
               "    }\n"
               "    this.sequence = Number(batch[batch.length - 1].sequence);\n"
               "    for (const event of events) {\n"
               "      const handler = this.handlers[event.property];\n"
               "      if (handler) handler(event);\n"
               "    }\n"
               "    return true;\n"
               "  }\n"
               "}\n"
               "function cloneValue(value) {\n"
               "  if (Array.isArray(value)) return value.map(cloneValue);\n"
               "  if (value instanceof Uint8Array) return new Uint8Array(value);\n"
               "  if (value !== null && typeof value === 'object') {\n"
               "    const out = {};\n"
               "    for (const key of Object.keys(value)) out[key] = cloneValue(value[key]);\n"
               "    return out;\n"
               "  }\n"
               "  return value;\n"
               "}\n"
               "function defaultValue(field) {\n"
               "  switch (field.kind) {\n"
               "    case 'int': return 0n;\n"
               "    case 'bool': return false;\n"
               "    case 'string': return '';\n"
               "    case 'bytes': return new Uint8Array(0);\n"
               "    case 'struct': {\n"
               "      const out = {};\n"
               "      for (const sub of sprotoFields[field.of]) out[sub.name] = defaultValue(sub);\n"
               "      return out;\n"
               "    }\n"
               "    default: return [];\n"
               "  }\n"
               "}\n"
               "function elementShape(shape) {\n"
               "  if (shape.kind === 'struct-array') return { kind: 'struct', of: shape.of };\n"
               "  if (shape.kind.endsWith('-array')) return { kind: shape.kind.slice(0, -6) };\n"
               "  return null;\n"
               "}\n"
               "function valueOk(shape, value) {\n"
               "  switch (shape.kind) {\n"
               "    case 'int': return typeof value === 'bigint' || (typeof value === 'number' && Number.isInteger(value));\n"
               "    case 'bool': return typeof value === 'boolean';\n"
               "    case 'string': return typeof value === 'string';\n"
               "    case 'bytes': return value instanceof Uint8Array;\n"
               "    case 'struct': return value !== null && typeof value === 'object' && !Array.isArray(value);\n"
               "    default: return Array.isArray(value);\n"
               "  }\n"
               "}\n"
               "function performDelta(replica, table, delta) {\n"
               "  const field = table.find((f) => f.id === Number(delta.property_id));\n"
               "  if (!field) return null;\n"
               "  const path = delta.path || [];\n"
               "  let holder = replica, key = field.name, shape = field;\n"
               "  for (const step of path) {\n"
               "    const node = holder[key];\n"
               "    if (node === undefined || node === null) return null;\n"
               "    if (step.by_index !== undefined) {\n"
               "      const elem = elementShape(shape);\n"
               "      if (!elem || !Array.isArray(node)) return null;\n"
               "      const index = Number(step.by_index);\n"
               "      if (!Number.isInteger(index) || index < 0 || index >= node.length) return null;\n"
               "      holder = node; key = index; shape = elem;\n"
               "    } else if (step.field_id !== undefined) {\n"
               "      if (shape.kind !== 'struct') return null;\n"
               "      const sub = sprotoFields[shape.of].find((f) => f.id === Number(step.field_id));\n"
               "      if (!sub || !Object.prototype.hasOwnProperty.call(node, sub.name)) return null;\n"
               "      holder = node; key = sub.name; shape = sub;\n"
               "    } else return null;\n"
               "  }\n"
               "  const position = delta.position === undefined ? 0 : Number(delta.position);\n"
               "  if (delta.kind === 'SET') {\n"
               "    if (!valueOk(shape, delta.value)) return null;\n"
               "    holder[key] = cloneValue(delta.value);\n"
               "  } else if (delta.kind === 'INSERT' || delta.kind === 'REMOVE') {\n"
               "    const elem = elementShape(shape);\n"
               "    const node = holder[key];\n"
               "    if (!elem || !Array.isArray(node)) return null;\n"
               "    if (!Number.isInteger(position) || position < 0) return null;\n"
               "    if (delta.kind === 'INSERT') {\n"
               "      if (position > node.length || !valueOk(elem, delta.value)) return null;\n"
               "      node.splice(position, 0, cloneValue(delta.value));\n"
               "    } else {\n"
               "      if (position >= node.length) return null;\n"
               "      node.splice(position, 1);\n"
               "    }\n"
               "  } else return null;\n"
               "  return { property: field.name, path, kind: delta.kind, position, value: delta.value };\n"
               "}\n";
        return out.str();
    }
    std::string csharp_bindings(bool client = false) const {
        std::ostringstream out;
        out << "// Generated by mistlane contract build; do not edit.\n";
        if (client)
            out << "// Client audience: only OWN_CLIENT/ALL_CLIENTS properties and\n"
                   "// owner-callable or client methods; SERVER_ONLY properties and\n"
                   "// server-internal (caller=\"server\") methods stay out. The wire\n"
                   "// codec body is byte-identical to the server artifact's.\n";
        // The audience filter: clients see replicated properties and the
        // methods they may invoke; everything else is server-internal.
        auto client_visible = [client](const ContractProperty& p) {
            return !client || p.flags == "OWN_CLIENT" || p.flags == "ALL_CLIENTS";
        };
        auto client_callable = [client](const ContractMethod& m) {
            return !client || m.caller == "owner";
        };
        out << "using System;\nusing System.Collections.Generic;\nusing System.IO;\nusing System.Linq;\nusing System.Text;\n"
               "namespace Mistlane.Contracts;\n"
               "public enum SprotoKind { Int, Bool, String, Bytes, Struct,\n"
               "                         IntArray, StructArray, BoolArray, StringArray, BytesArray, Unsupported }\n"
               "public readonly record struct SprotoField(ushort Id, string Name, SprotoKind Kind = SprotoKind.Int, string Sub = null);\n"
               "public static class SprotoCodec {\n"
               "  // Standard sproto codec for the generated tag tables. Struct fields are\n"
               "  // length-prefixed records, integer arrays carry an intlen byte before 4-\n"
               "  // or 8-byte elements, and arrays of structs are per-element\n"
               "  // [len][record]. Booleans ride the integer in-band word (true=4,\n"
               "  // false=2), STRING/BYTES fields are length-prefixed byte objects,\n"
               "  // boolean arrays are one raw 0/1 byte per element with no intlen byte,\n"
               "  // and string/bytes arrays are per-element [len][bytes]; STRING payloads\n"
               "  // decode through a throwing UTF-8 encoding so invalid sequences are\n"
               "  // rejected, not silently replaced. Dword reads widen each byte through\n"
               "  // uint before shifting: the low dword of a wide integer may carry its\n"
               "  // sign bit, and int arithmetic here would re-create the upstream\n"
               "  // todword sign-extension defect.\n"
               "  public static readonly UTF8Encoding StrictUtf8 = new UTF8Encoding(false, true);\n"
               "  private static void Push32(List<byte> data, uint word) { data.Add((byte)word); data.Add((byte)(word>>8)); data.Add((byte)(word>>16)); data.Add((byte)(word>>24)); }\n"
               "  private static uint Read32(byte[] bytes, int at) => (uint)bytes[at] | ((uint)bytes[at+1]<<8) | ((uint)bytes[at+2]<<16) | ((uint)bytes[at+3]<<24);\n"
               "  private static IReadOnlyList<SprotoField> Type(string sub) => ContractSchemas.Types.TryGetValue(sub ?? \"\", out var fields) ? fields : throw new InvalidDataException(\"unknown type \" + sub);\n"
               "  public static byte[] Encode(IReadOnlyList<SprotoField> fields, IReadOnlyDictionary<string, object> values) {\n"
               "    var tags = new List<ushort>(); var data = new List<byte>(); var last = -1;\n"
               "    foreach (var field in fields.OrderBy(f => f.Id)) {\n"
               "      if (!values.TryGetValue(field.Name, out var raw)) continue;\n"
               "      var gap = field.Id-last-1;\n"
               "      if (gap > 0) tags.Add((ushort)((gap-1)*2+1));\n"
               "      last = field.Id;\n"
               "      switch (field.Kind) {\n"
               "        case SprotoKind.Int: {\n"
               "          var n = raw as long? ?? throw new InvalidDataException(field.Name + \" is not an integer\");\n"
               "          if (n >= 0 && n < 0x7fff) {\n"
               "            tags.Add((ushort)((n+1)*2));\n"
               "          } else {\n"
               "            tags.Add(0);\n"
               "            var wide = n < int.MinValue || n > int.MaxValue;\n"
               "            Push32(data, wide ? 8u : 4u);\n"
               "            Push32(data, unchecked((uint)n));\n"
               "            if (wide) Push32(data, unchecked((uint)(n>>32)));\n"
               "          }\n"
               "          break;\n"
               "        }\n"
               "        case SprotoKind.Bool: {\n"
               "          var flag = raw as bool? ?? throw new InvalidDataException(field.Name + \" is not a boolean\");\n"
               "          tags.Add((ushort)(flag ? 4 : 2));\n"
               "          break;\n"
               "        }\n"
               "        case SprotoKind.String: {\n"
               "          var text = raw as string ?? throw new InvalidDataException(field.Name + \" is not a string\");\n"
               "          tags.Add(0);\n"
               "          var payload = Encoding.UTF8.GetBytes(text);\n"
               "          Push32(data, (uint)payload.Length); data.AddRange(payload);\n"
               "          break;\n"
               "        }\n"
               "        case SprotoKind.Bytes: {\n"
               "          var payload = raw as byte[] ?? throw new InvalidDataException(field.Name + \" is not bytes\");\n"
               "          tags.Add(0);\n"
               "          Push32(data, (uint)payload.Length); data.AddRange(payload);\n"
               "          break;\n"
               "        }\n"
               "        case SprotoKind.Struct: {\n"
               "          tags.Add(0);\n"
               "          var nested = raw as Dictionary<string, object> ?? throw new InvalidDataException(field.Name + \" is not a struct\");\n"
               "          var content = Encode(Type(field.Sub), nested);\n"
               "          Push32(data, (uint)content.Length); data.AddRange(content);\n"
               "          break;\n"
               "        }\n"
               "        case SprotoKind.IntArray: {\n"
               "          tags.Add(0);\n"
               "          var list = raw as List<long> ?? throw new InvalidDataException(field.Name + \" is not an integer array\");\n"
               "          var wide = list.Any(n => n < int.MinValue || n > int.MaxValue);\n"
               "          var content = new List<byte>();\n"
               "          if (list.Count > 0) {\n"
               "            content.Add((byte)(wide ? 8 : 4));\n"
               "            foreach (var n in list) {\n"
               "              if (wide) { for (int i = 0; i < 8; i++) content.Add((byte)(unchecked((ulong)n) >> (8*i))); }\n"
               "              else { for (int i = 0; i < 4; i++) content.Add((byte)(unchecked((uint)(int)n) >> (8*i))); }\n"
               "            }\n"
               "          }\n"
               "          Push32(data, (uint)content.Count); data.AddRange(content);\n"
               "          break;\n"
               "        }\n"
               "        case SprotoKind.BoolArray: {\n"
               "          tags.Add(0);\n"
               "          var list = raw as List<bool> ?? throw new InvalidDataException(field.Name + \" is not a boolean array\");\n"
               "          var content = new List<byte>();\n"
               "          foreach (var flag in list) content.Add(flag ? (byte)1 : (byte)0);\n"
               "          Push32(data, (uint)content.Count); data.AddRange(content);\n"
               "          break;\n"
               "        }\n"
               "        case SprotoKind.StringArray: {\n"
               "          tags.Add(0);\n"
               "          var list = raw as List<string> ?? throw new InvalidDataException(field.Name + \" is not a string array\");\n"
               "          var content = new List<byte>();\n"
               "          foreach (var text in list) {\n"
               "            var payload = Encoding.UTF8.GetBytes(text);\n"
               "            Push32(content, (uint)payload.Length); content.AddRange(payload);\n"
               "          }\n"
               "          Push32(data, (uint)content.Count); data.AddRange(content);\n"
               "          break;\n"
               "        }\n"
               "        case SprotoKind.BytesArray: {\n"
               "          tags.Add(0);\n"
               "          var list = raw as List<byte[]> ?? throw new InvalidDataException(field.Name + \" is not a bytes array\");\n"
               "          var content = new List<byte>();\n"
               "          foreach (var payload in list) {\n"
               "            Push32(content, (uint)payload.Length); content.AddRange(payload);\n"
               "          }\n"
               "          Push32(data, (uint)content.Count); data.AddRange(content);\n"
               "          break;\n"
               "        }\n"
               "        case SprotoKind.StructArray: {\n"
               "          tags.Add(0);\n"
               "          var list = raw as List<Dictionary<string, object>> ?? throw new InvalidDataException(field.Name + \" is not a struct array\");\n"
               "          var content = new List<byte>();\n"
               "          foreach (var item in list) {\n"
               "            var record = Encode(Type(field.Sub), item);\n"
               "            Push32(content, (uint)record.Length); content.AddRange(record);\n"
               "          }\n"
               "          Push32(data, (uint)content.Count); data.AddRange(content);\n"
               "          break;\n"
               "        }\n"
               "        default: throw new InvalidDataException(\"unsupported field type \" + field.Kind + \" for \" + field.Name);\n"
               "      }\n"
               "    }\n"
               "    var result = new List<byte> { (byte)tags.Count, (byte)(tags.Count>>8) };\n"
               "    foreach (var tag in tags) { result.Add((byte)tag); result.Add((byte)(tag>>8)); }\n"
               "    result.AddRange(data); return result.ToArray();\n"
               "  }\n"
               "  public static IReadOnlyDictionary<string, object> Decode(IReadOnlyList<SprotoField> fields, byte[] bytes) {\n"
               "    var byId = fields.ToDictionary(f => f.Id);\n"
               "    if (bytes == null || bytes.Length < 2) throw new InvalidDataException(\"short header\");\n"
               "    ushort count = (ushort)(bytes[0] | (bytes[1]<<8));\n"
               "    if (bytes.Length < 2 + 2 * count) throw new InvalidDataException(\"short tag block\");\n"
               "    var result = new Dictionary<string, object>();\n"
               "    int tagPos = 2, dataPos = 2 + 2 * count, tag = -1;\n"
               "    for (int i = 0; i < count; i++) {\n"
               "      var word = (ushort)(bytes[tagPos] | (bytes[tagPos+1]<<8));\n"
               "      tagPos += 2; tag += 1;\n"
               "      if ((word & 1) != 0) { tag += (word-1)/2; continue; }\n"
               "      var known = byId.TryGetValue((ushort)tag, out var field);\n"
               "      if (word != 0) {\n"
               "        if (known && field.Kind != SprotoKind.Int && field.Kind != SprotoKind.Bool) throw new InvalidDataException(\"in-band value for non-integer \" + field.Name);\n"
               "        if (known && field.Kind == SprotoKind.Bool) {\n"
               "          var flag = word/2 - 1;\n"
               "          if (flag != 0 && flag != 1) throw new InvalidDataException(\"bad boolean \" + field.Name);\n"
               "          result[field.Name] = flag == 1;\n"
               "        } else if (known) result[field.Name] = (long)(word/2 - 1);\n"
               "        continue;\n"
               "      }\n"
               "      if (bytes.Length - dataPos < 4) throw new InvalidDataException(\"truncated value\");\n"
               "      int length = (int)Read32(bytes, dataPos);\n"
               "      int start = dataPos + 4;\n"
               "      if (bytes.Length - start < length) throw new InvalidDataException(\"truncated value\");\n"
               "      if (known) {\n"
               "        switch (field.Kind) {\n"
               "          case SprotoKind.Int: {\n"
               "            if (length != 4 && length != 8) throw new InvalidDataException(\"bad value span\");\n"
               "            uint lo = Read32(bytes, start);\n"
               "            result[field.Name] = length == 8 ? unchecked((long)((ulong)lo | ((ulong)Read32(bytes, start+4)<<32))) : unchecked((int)lo);\n"
               "            break;\n"
               "          }\n"
               "          case SprotoKind.Struct:\n"
               "            result[field.Name] = Decode(Type(field.Sub), bytes.AsSpan(start, length).ToArray());\n"
               "            break;\n"
               "          case SprotoKind.Bool: {\n"
               "            if (length != 4 && length != 8) throw new InvalidDataException(\"bad value span\");\n"
               "            uint lo = Read32(bytes, start);\n"
               "            result[field.Name] = length == 8 ? (lo | Read32(bytes, start+4)) != 0u : lo != 0u;\n"
               "            break;\n"
               "          }\n"
               "          case SprotoKind.String:\n"
               "            try { result[field.Name] = StrictUtf8.GetString(bytes, start, length); }\n"
               "            catch (DecoderFallbackException error) { throw new InvalidDataException(\"invalid utf-8 for \" + field.Name, error); }\n"
               "            break;\n"
               "          case SprotoKind.Bytes: {\n"
               "            var payload = new byte[length];\n"
               "            Array.Copy(bytes, start, payload, 0, length);\n"
               "            result[field.Name] = payload;\n"
               "            break;\n"
               "          }\n"
               "          case SprotoKind.IntArray: {\n"
               "            var ints = new List<long>();\n"
               "            int p = start, end = start + length;\n"
               "            if (p < end) {\n"
               "              int intlen = bytes[p];\n"
               "              p += 1;\n"
               "              if ((intlen != 4 && intlen != 8) || (end - p) % intlen != 0) throw new InvalidDataException(\"bad array layout\");\n"
               "              while (p < end) {\n"
               "                uint lo = Read32(bytes, p);\n"
               "                ints.Add(intlen == 8 ? unchecked((long)((ulong)lo | ((ulong)Read32(bytes, p+4)<<32))) : unchecked((int)lo));\n"
               "                p += intlen;\n"
               "              }\n"
               "            }\n"
               "            result[field.Name] = ints;\n"
               "            break;\n"
               "          }\n"
               "          case SprotoKind.BoolArray: {\n"
               "            var flags = new List<bool>();\n"
               "            for (int p = start; p < start + length; p++) {\n"
               "              if (bytes[p] > 1) throw new InvalidDataException(\"bad boolean array\");\n"
               "              flags.Add(bytes[p] == 1);\n"
               "            }\n"
               "            result[field.Name] = flags;\n"
               "            break;\n"
               "          }\n"
               "          case SprotoKind.StringArray: {\n"
               "            var texts = new List<string>();\n"
               "            int p = start, end = start + length;\n"
               "            while (p < end) {\n"
               "              if (end - p < 4) throw new InvalidDataException(\"bad array layout\");\n"
               "              int itemLength = (int)Read32(bytes, p);\n"
               "              if (end - p - 4 < itemLength) throw new InvalidDataException(\"bad array layout\");\n"
               "              try { texts.Add(StrictUtf8.GetString(bytes, p + 4, itemLength)); }\n"
               "              catch (DecoderFallbackException error) { throw new InvalidDataException(\"invalid utf-8 for \" + field.Name, error); }\n"
               "              p += 4 + itemLength;\n"
               "            }\n"
               "            result[field.Name] = texts;\n"
               "            break;\n"
               "          }\n"
               "          case SprotoKind.BytesArray: {\n"
               "            var chunks = new List<byte[]>();\n"
               "            int p = start, end = start + length;\n"
               "            while (p < end) {\n"
               "              if (end - p < 4) throw new InvalidDataException(\"bad array layout\");\n"
               "              int itemLength = (int)Read32(bytes, p);\n"
               "              if (end - p - 4 < itemLength) throw new InvalidDataException(\"bad array layout\");\n"
               "              var chunk = new byte[itemLength];\n"
               "              Array.Copy(bytes, p + 4, chunk, 0, itemLength);\n"
               "              chunks.Add(chunk);\n"
               "              p += 4 + itemLength;\n"
               "            }\n"
               "            result[field.Name] = chunks;\n"
               "            break;\n"
               "          }\n"
               "          case SprotoKind.StructArray: {\n"
               "            var items = new List<Dictionary<string, object>>();\n"
               "            int p = start, end = start + length;\n"
               "            while (p < end) {\n"
               "              if (end - p < 4) throw new InvalidDataException(\"bad array layout\");\n"
               "              int recordLength = (int)Read32(bytes, p);\n"
               "              if (end - p - 4 < recordLength) throw new InvalidDataException(\"bad array layout\");\n"
               "              items.Add((Dictionary<string, object>)Decode(Type(field.Sub), bytes.AsSpan(p+4, recordLength).ToArray()));\n"
               "              p += 4 + recordLength;\n"
               "            }\n"
               "            result[field.Name] = items;\n"
               "            break;\n"
               "          }\n"
               "          default: throw new InvalidDataException(\"unsupported field type \" + field.Kind + \" for \" + field.Name);\n"
               "        }\n"
               "      }\n"
               "      dataPos = start + length;\n"
               "    }\n"
               "    if (dataPos != bytes.Length) throw new InvalidDataException(\"trailing bytes\");\n"
               "    return result;\n"
               "  }\n}\n";
        auto cs_type = [](const std::string& t, const std::string& of) {
            if (t == "ARRAY") return "List<" + of + ">";
            if (t == "BOOL") return std::string("bool");
            if (t == "STRING") return std::string("string");
            if (t == "BYTES") return std::string("byte[]");
            if (t == "INT64" || t == "EntityRef") return std::string("long");
            if (t == "UINT16") return std::string("ushort");
            if (t == "UINT32") return std::string("uint");
            if (t == "INT32") return std::string("int");
            return t;
        };
        auto cs_kind = [this](const ContractField& f) {
            const auto kind = field_kind(f);
            if (kind == "struct") return "SprotoKind.Struct";
            if (kind == "struct-array") return "SprotoKind.StructArray";
            if (kind == "int-array") return "SprotoKind.IntArray";
            if (kind == "bool") return "SprotoKind.Bool";
            if (kind == "string") return "SprotoKind.String";
            if (kind == "bytes") return "SprotoKind.Bytes";
            if (kind == "bool-array") return "SprotoKind.BoolArray";
            if (kind == "string-array") return "SprotoKind.StringArray";
            if (kind == "bytes-array") return "SprotoKind.BytesArray";
            if (kind == "int") return "SprotoKind.Int";
            return "SprotoKind.Unsupported";
        };
        auto cs_sub = [this](const ContractField& f) {
            const auto kind = field_kind(f);
            // Only struct kinds resolve through the type registry; everything
            // else keeps a null Sub.
            if (kind == "struct") return "\"" + f.type + "\"";
            if (kind == "struct-array") return "\"" + f.of + "\"";
            return std::string("null");
        };
        std::vector<std::string> type_names;
        auto properties_of = [&](const ContractEntity& e) {
            ContractArgs properties;
            for (const auto& p : e.properties)
                if (client_visible(p)) properties.push_back({p.name,p.type,p.of,p.default_value,p.id});
            return properties;
        };
        auto emit = [&](const std::string& name, const ContractArgs& fields) {
            auto mapped = [&](const std::string& t, const std::string& of) {
                if (t != "ARRAY") return cs_type(t, of);
                if (structs_.count(of)) return std::string("List<Dictionary<string, object>>");
                return "List<" + cs_type(of, "") + ">";
            };
            out << "public sealed record " << name << "(";
            for (std::size_t i=0;i<fields.size();++i) { if(i) out << ", "; out << mapped(fields[i].type,fields[i].of) << " " << fields[i].name; }
            out << ");\n";
        };
        for (const auto& [name, fields] : structs_) emit(name, fields);
        for (const auto& e : entities_) {
            emit(e.name, properties_of(e));
            for (const auto& m : e.server_methods) if (client_callable(m)) {
                emit(e.name+"_"+m.name+"Args", m.args);
                if (!m.returns.empty()) emit(e.name+"_"+m.name+"Returns", m.returns);
            }
            for (const auto& m : e.client_methods) emit(e.name+"_"+m.name+"Args", m.args);
        }
        // Invocation envelope and typed reply (docs/entity-contract.md): the
        // fixed big-endian header riding the existing bounded frames, and the
        // one-of-returns-or-error answer shape. Byte-for-byte the same wire
        // form the C++ server encodes (include/mistlane/entity_envelope.hpp).
        out <<
               "public readonly record struct EntityEnvelope(uint Scope, uint TypeId, long EntityId, uint Incarnation,\n"
               "  byte Side, ushort MethodId, ulong CorrelationId, uint HostingGeneration, uint DeadlineMs, byte[] Payload)\n"
               "{ public const int HeaderLength = 39; }\n"
               "public static class EntityEnvelopeCodec {\n"
               "  public static readonly string[] EntityErrors = { \"INVALID\", \"UNROUTED\", \"STALE\", \"FORBIDDEN\",\n"
               "    \"TIMEOUT\", \"CANCELLED\", \"NO_IMPLEMENTATION\", \"NOT_READY\", \"CAPACITY\", \"CONFLICT\" };\n"
               "  public readonly record struct EntityReply(byte[] Payload, string? Error, string? Reason);\n"
               "  public static byte[] Encode(EntityEnvelope e) {\n"
               "    if (e.Side > 1) throw new InvalidDataException(\"side must be 0 or 1\");\n"
               "    var payload = e.Payload ?? Array.Empty<byte>();\n"
               "    var outBytes = new byte[EntityEnvelope.HeaderLength + payload.Length];\n"
               "    var be = new SpanBigEndian(outBytes);\n"
               "    be.Put(0, e.Scope); be.Put(4, e.TypeId); be.Put(8, (ulong)e.EntityId);\n"
               "    be.Put(16, e.Incarnation); outBytes[20] = e.Side;\n"
               "    outBytes[21] = (byte)(e.MethodId >> 8); outBytes[22] = (byte)e.MethodId;\n"
               "    be.Put(23, e.CorrelationId); be.Put(31, e.HostingGeneration); be.Put(35, e.DeadlineMs);\n"
               "    Array.Copy(payload, 0, outBytes, EntityEnvelope.HeaderLength, payload.Length);\n"
               "    return outBytes;\n"
               "  }\n"
               "  public static EntityEnvelope? Decode(byte[] bytes) {\n"
               "    if (bytes == null || bytes.Length < EntityEnvelope.HeaderLength) return null;\n"
               "    if (bytes[20] > 1) return null;\n"
               "    var be = new SpanBigEndian(bytes);\n"
               "    var payload = new byte[bytes.Length - EntityEnvelope.HeaderLength];\n"
               "    Array.Copy(bytes, EntityEnvelope.HeaderLength, payload, 0, payload.Length);\n"
               "    return new EntityEnvelope(be.Get32(0), be.Get32(4), unchecked((long)be.Get(8)), be.Get32(16),\n"
               "      bytes[20], (ushort)((bytes[21] << 8) | bytes[22]), be.Get(23), be.Get32(31), be.Get32(35), payload);\n"
               "  }\n"
               "  public static byte[] EncodeReply(EntityReply reply) {\n"
               "    if (reply.Error == null) {\n"
               "      var body = reply.Payload ?? Array.Empty<byte>();\n"
               "      var ok = new byte[1 + body.Length];\n"
               "      Array.Copy(body, 0, ok, 1, body.Length);\n"
               "      return ok;\n"
               "    }\n"
               "    var code = Array.IndexOf(EntityErrors, reply.Error);\n"
               "    if (code < 0) throw new InvalidDataException(\"unknown error \" + reply.Error);\n"
               "    if (!string.IsNullOrEmpty(reply.Reason) && reply.Error != \"INVALID\") throw new InvalidDataException(\"only INVALID may carry a reason\");\n"
               "    var reason = reply.Reason == null ? Array.Empty<byte>() : Encoding.UTF8.GetBytes(reply.Reason);\n"
               "    if (reason.Length > 0xffff) throw new InvalidDataException(\"reason too long\");\n"
               "    var outBytes = new byte[reply.Error == \"INVALID\" ? 4 + reason.Length : 2];\n"
               "    outBytes[0] = 1; outBytes[1] = (byte)code;\n"
               "    if (reply.Error == \"INVALID\") { outBytes[2] = (byte)(reason.Length >> 8); outBytes[3] = (byte)reason.Length; Array.Copy(reason, 0, outBytes, 4, reason.Length); }\n"
               "    return outBytes;\n"
               "  }\n"
               "  public static EntityReply? DecodeReply(byte[] bytes) {\n"
               "    if (bytes == null || bytes.Length == 0) return null;\n"
               "    if (bytes[0] == 0) { var body = new byte[bytes.Length - 1]; Array.Copy(bytes, 1, body, 0, body.Length); return new EntityReply(body, null, \"\"); }\n"
               "    if (bytes[0] != 1 || bytes.Length < 2) return null;\n"
               "    var code = bytes[1];\n"
               "    if (code >= EntityErrors.Length) return null;\n"
               "    var error = EntityErrors[code];\n"
               "    if (error == \"INVALID\") {\n"
               "      if (bytes.Length < 4) return null;\n"
               "      int length = (bytes[2] << 8) | bytes[3];\n"
               "      if (bytes.Length - 4 != length) return null;\n"
               "      try { return new EntityReply(Array.Empty<byte>(), error, SprotoCodec.StrictUtf8.GetString(bytes, 4, length)); }\n"
               "      catch (Exception) { return null; }\n"
               "    }\n"
               "    if (bytes.Length != 2) return null;\n"
               "    return new EntityReply(Array.Empty<byte>(), error, \"\");\n"
               "  }\n"
               "  // Big-endian field helper over a fixed byte span.\n"
               "  private readonly struct SpanBigEndian {\n"
               "    private readonly byte[] bytes;\n"
               "    public SpanBigEndian(byte[] data) { bytes = data; }\n"
               "    public void Put(int offset, ulong value) {\n"
               "      for (int i = 0; i < 8; i++) bytes[offset + i] = (byte)(value >> (56 - 8 * i));\n"
               "    }\n"
               "    public void Put(int offset, uint value) {\n"
               "      for (int i = 0; i < 4; i++) bytes[offset + i] = (byte)(value >> (24 - 8 * i));\n"
               "    }\n"
               "    public ulong Get(int offset) {\n"
               "      ulong value = 0;\n"
               "      for (int i = 0; i < 8; i++) value = (value << 8) | bytes[offset + i];\n"
               "      return value;\n"
               "    }\n"
               "    public uint Get32(int offset) {\n"
               "      uint value = 0;\n"
               "      for (int i = 0; i < 4; i++) value = (value << 8) | bytes[offset + i];\n"
               "      return value;\n"
               "    }\n"
               "  }\n"
               "}\n\n"
               "// Generated per-entity method tables: outbound is what this audience may\n"
               "// send (server methods), inbound is what it receives (client notifies).\n"
               "// The client facade binds method names straight off its own Avatar — no\n"
               "// .server/.client intermediate layer.\n"
               "public sealed record EntityMethodInfo(uint Id, string Mode, string Args, string? Returns);\n"
               "public sealed record EntityNotifyInfo(uint Id, string Args);\n"
               "public static class EntityMethods {\n";
        out << "  public static readonly IReadOnlyDictionary<string, uint> TypeIds = new Dictionary<string, uint> {";
        for (const auto& e : entities_)
            out << " [\"" << e.name << "\"] = " << e.id << "U,";
        out << " };\n";
        out << "  public static readonly IReadOnlyDictionary<string, IReadOnlyDictionary<string, EntityMethodInfo>> Outbound =\n"
               "    new Dictionary<string, IReadOnlyDictionary<string, EntityMethodInfo>> {\n";
        for (const auto& e : entities_) {
            out << "      [\"" << e.name << "\"] = new Dictionary<string, EntityMethodInfo> {";
            bool first = true;
            for (const auto& m : e.server_methods) {
                if (!client_callable(m)) continue;
                if (!first) out << ",";
                first = false;
                const std::string returns = m.returns.empty() ? std::string(", null)")
                    : ", \""+e.name+"_"+m.name+"Returns\")";
                out << "\n        [\"" << m.name << "\"] = new EntityMethodInfo(" << m.id << "U, \"" << m.mode << "\", \""
                    << e.name << "_" << m.name << "Args\"" << returns;
            }
            out << "\n      },";
        }
        out << "\n    };\n";
        out << "  public static readonly IReadOnlyDictionary<string, IReadOnlyDictionary<string, EntityNotifyInfo>> Inbound =\n"
               "    new Dictionary<string, IReadOnlyDictionary<string, EntityNotifyInfo>> {\n";
        for (const auto& e : entities_) {
            out << "      [\"" << e.name << "\"] = new Dictionary<string, EntityNotifyInfo> {";
            bool first = true;
            for (const auto& m : e.client_methods) {
                if (!first) out << ",";
                first = false;
                out << "\n        [\"" << m.name << "\"] = new EntityNotifyInfo(" << m.id << "U, \""
                    << e.name << "_" << m.name << "Args\")";
            }
            out << "\n      },";
        }
        out << "\n    };\n";
        out << "}\n\n"
               "// Client-side outstanding-call discipline: the mirror of the server's bounded\n"
               "// RpcQueue. Capacity is fixed at construction, admissions are typed refusals\n"
               "// rather than silent drops, an injected clock makes deadlines testable, a\n"
               "// timed-out call stays a surfaced tombstone until Take() (its outcome is\n"
               "// unknown — it may already have been applied), and Retry() only re-arms a\n"
               "// call admitted as idempotent with a fresh correlation id, so a\n"
               "// non-idempotent timeout can never be silently retried. Replies carry no\n"
               "// correlation on the wire, so the transport pairs a reply with the call it\n"
               "// belongs to and passes that id here.\n"
               "public sealed class EntityRpcQueue {\n"
               "  public sealed class Entry {\n"
               "    public string State = \"PENDING\";\n"
               "    public bool Idempotent;\n"
               "    public (string Type, string Method, IReadOnlyDictionary<string, object> Args)? Tag;\n"
               "    public IReadOnlyDictionary<string, object>? Response;\n"
               "    public string? Error;\n"
               "    public long Deadline;\n"
               "  }\n"
               "  private readonly Dictionary<long, Entry> calls = new();\n"
               "  private long clock;\n"
               "  private long nextId;\n"
               "  public EntityRpcQueue(int capacity = 64) {\n"
               "    if (capacity < 1) throw new InvalidDataException(\"capacity must be a positive integer\");\n"
               "    Capacity = capacity;\n"
               "  }\n"
               "  public int Capacity { get; }\n"
               "  public int Size => calls.Count;\n"
               "  public string? Error { get; private set; }\n"
               "  public long Call(long deadlineMs, bool idempotent = false,\n"
               "      (string Type, string Method, IReadOnlyDictionary<string, object> Args)? tag = null, long id = 0) {\n"
               "    Error = null;\n"
               "    var cid = id != 0 ? id : ++nextId;\n"
               "    if (cid < 1 || deadlineMs < 1) { Error = \"INVALID\"; return 0; }\n"
               "    if (calls.ContainsKey(cid)) { Error = \"INVALID\"; return 0; }\n"
               "    if (calls.Count >= Capacity) { Error = \"CAPACITY\"; return 0; }\n"
               "    calls[cid] = new Entry { Deadline = clock + deadlineMs, Idempotent = idempotent, Tag = tag };\n"
               "    return cid;\n"
               "  }\n"
               "  public bool Advance(long now) {\n"
               "    if (now < 0) return false;\n"
               "    clock = Math.Max(clock, now);\n"
               "    foreach (var call in calls.Values)\n"
               "      if (call.State == \"PENDING\" && clock >= call.Deadline) call.State = \"TIMED_OUT\";\n"
               "    return true;\n"
               "  }\n"
               "  public bool Cancel(long id) {\n"
               "    Error = null;\n"
               "    if (!calls.TryGetValue(id, out var call) || call.State != \"PENDING\") { Error = \"INVALID\"; return false; }\n"
               "    call.State = \"CANCELLED\";\n"
               "    return true;\n"
               "  }\n"
               "  public string Complete(long id, IReadOnlyDictionary<string, object>? response = null, string? error = null) {\n"
               "    if (!calls.TryGetValue(id, out var call)) return \"STALE\";\n"
               "    if (call.State == \"PENDING\" && clock >= call.Deadline) call.State = \"TIMED_OUT\";\n"
               "    if (call.State == \"TIMED_OUT\") return \"TIMEOUT\";\n"
               "    if (call.State == \"CANCELLED\") return \"CANCELLED\";\n"
               "    if (call.State == \"OK\") return \"CONFLICT\";\n"
               "    call.State = \"OK\";\n"
               "    call.Response = response;\n"
               "    call.Error = error;\n"
               "    return \"OK\";\n"
               "  }\n"
               "  public long? Retry(long oldId, long newDeadlineMs) {\n"
               "    if (!calls.TryGetValue(oldId, out var call) || call.State != \"TIMED_OUT\"\n"
               "        || !call.Idempotent || newDeadlineMs < 1) return null;\n"
               "    var cid = ++nextId;\n"
               "    calls.Remove(oldId);\n"
               "    calls[cid] = new Entry { Deadline = clock + newDeadlineMs, Idempotent = true, Tag = call.Tag };\n"
               "    return cid;\n"
               "  }\n"
               "  public Entry? Take(long id) {\n"
               "    if (!calls.TryGetValue(id, out var call) || call.State == \"PENDING\") return null;\n"
               "    calls.Remove(id);\n"
               "    return call;\n"
               "  }\n"
               "  public Entry? Get(long id) => calls.TryGetValue(id, out var call) ? call : null;\n"
               "  public string? State(long id) => calls.TryGetValue(id, out var call) ? call.State : null;\n"
               "}\n\n"
               "// The direct Avatar facade: one instance per (type, entity), sending\n"
               "// envelopes the caller's transport owns and decoding typed returns off the\n"
               "// method tables above. A call is admitted to the bounded queue before\n"
               "// anything is sent, so an overflow is refused rather than dropped, and the\n"
               "// admitted entry keeps the args so a retry re-encodes the same payload under\n"
               "// a fresh correlation id instead of resending blindly.\n"
               "public sealed class EntityClient {\n"
               "  private readonly Action<byte[]> send;\n"
               "  public EntityClient(Action<byte[]> send, string type, long entityId, uint incarnation = 0,\n"
               "      uint scope = 0, uint deadlineMs = 5000, int capacity = 64) {\n"
               "    if (!EntityMethods.TypeIds.TryGetValue(type, out var typeId))\n"
               "      throw new InvalidDataException(\"unknown entity type \" + type);\n"
               "    this.send = send;\n"
               "    Type = type;\n"
               "    TypeId = typeId;\n"
               "    EntityId = entityId;\n"
               "    Incarnation = incarnation;\n"
               "    Scope = scope;\n"
               "    DefaultDeadlineMs = deadlineMs;\n"
               "    Queue = new EntityRpcQueue(capacity);\n"
               "  }\n"
               "  public string Type { get; }\n"
               "  public uint TypeId { get; }\n"
               "  public long EntityId { get; }\n"
               "  public uint Incarnation { get; }\n"
               "  public uint Scope { get; }\n"
               "  public uint DefaultDeadlineMs { get; }\n"
               "  public EntityRpcQueue Queue { get; }\n"
               "  private EntityMethodInfo? Outbound(string method) =>\n"
               "    EntityMethods.Outbound.TryGetValue(Type, out var table)\n"
               "      && table.TryGetValue(method, out var info) ? info : null;\n"
               "  private byte[] Envelope(EntityMethodInfo info, IReadOnlyDictionary<string, object>? args, uint deadlineMs, ulong correlationId) {\n"
               "    IReadOnlyList<SprotoField> fields = info.Args != null\n"
               "      && ContractSchemas.Types.TryGetValue(info.Args, out var table) ? table : Array.Empty<SprotoField>();\n"
               "    var payload = SprotoCodec.Encode(fields, args ?? new Dictionary<string, object>());\n"
               "    return EntityEnvelopeCodec.Encode(new EntityEnvelope(Scope, TypeId, EntityId, Incarnation,\n"
               "      0, (ushort)info.Id, correlationId, 0, deadlineMs, payload));\n"
               "  }\n"
               "  public (bool Ok, long Id, string? Error) Call(string method,\n"
               "      IReadOnlyDictionary<string, object>? args = null, uint? deadlineMs = null, bool idempotent = false) {\n"
               "    var info = Outbound(method);\n"
               "    if (info == null || info.Mode != \"request\") return (false, 0, \"INVALID\");\n"
               "    var deadline = deadlineMs ?? DefaultDeadlineMs;\n"
               "    var id = Queue.Call(deadline, idempotent, (Type, method, args ?? new Dictionary<string, object>()));\n"
               "    if (id == 0) return (false, 0, Queue.Error);\n"
               "    send(Envelope(info, args, deadline, (ulong)id));\n"
               "    return (true, id, null);\n"
               "  }\n"
               "  public (bool Ok, string? Error) Notify(string method, IReadOnlyDictionary<string, object>? args = null,\n"
               "      uint? deadlineMs = null) {\n"
               "    var info = Outbound(method);\n"
               "    if (info == null || info.Mode != \"notify\") return (false, \"INVALID\");\n"
               "    send(Envelope(info, args, deadlineMs ?? DefaultDeadlineMs, 0));\n"
               "    return (true, null);\n"
               "  }\n"
               "  // Malformed replies drop without touching the queue: the correlation stays\n"
               "  // outstanding rather than vanishing silently.\n"
               "  public string OnReply(long id, byte[] bytes) {\n"
               "    var reply = EntityEnvelopeCodec.DecodeReply(bytes);\n"
               "    if (reply == null) return \"MALFORMED\";\n"
               "    if (reply.Value.Error != null) return Queue.Complete(id, null, reply.Value.Error);\n"
               "    var entry = Queue.Get(id);\n"
               "    if (entry == null) return \"STALE\";\n"
               "    IReadOnlyDictionary<string, object>? response = null;\n"
               "    var info = entry.Tag != null ? Outbound(entry.Tag.Value.Method) : null;\n"
               "    if (info != null && info.Returns != null && reply.Value.Payload.Length > 0\n"
               "        && ContractSchemas.Types.TryGetValue(info.Returns, out var fields))\n"
               "      response = SprotoCodec.Decode(fields, reply.Value.Payload);\n"
               "    return Queue.Complete(id, response);\n"
               "  }\n"
               "  public bool Advance(long now) => Queue.Advance(now);\n"
               "  public bool Cancel(long id) => Queue.Cancel(id);\n"
               "  public long? Retry(long oldId, uint? deadlineMs = null) {\n"
               "    var entry = Queue.Get(oldId);\n"
               "    if (entry == null || entry.Tag == null) return null;\n"
               "    var deadline = deadlineMs ?? DefaultDeadlineMs;\n"
               "    var id = Queue.Retry(oldId, deadline);\n"
               "    if (id == null) return null;\n"
               "    var info = Outbound(entry.Tag.Value.Method);\n"
               "    if (info != null) send(Envelope(info, entry.Tag.Value.Args, deadline, (ulong)id));\n"
               "    return id;\n"
               "  }\n"
               "  public EntityRpcQueue.Entry? Take(long id) => Queue.Take(id);\n"
               "  // Inbound notifies: side must be server, the method id must be this\n"
               "  // audience's own, and the payload must decode against its table.\n"
               "  public object? OnNotify(byte[] bytes,\n"
               "      Func<string, IReadOnlyDictionary<string, object>, object> handler) {\n"
               "    var env = EntityEnvelopeCodec.Decode(bytes);\n"
               "    if (env == null || env.Value.Side != 1) return null;\n"
               "    if (!EntityMethods.Inbound.TryGetValue(Type, out var table)) return null;\n"
               "    foreach (var pair in table)\n"
               "      if (pair.Value.Id == env.Value.MethodId) {\n"
               "        IReadOnlyList<SprotoField> fields = pair.Value.Args != null\n"
               "          && ContractSchemas.Types.TryGetValue(pair.Value.Args, out var table2) ? table2 : Array.Empty<SprotoField>();\n"
               "        return handler(pair.Key, SprotoCodec.Decode(fields, env.Value.Payload));\n"
               "      }\n"
               "    return null;\n"
               "  }\n"
               "}\n";
        out <<
               "public sealed record SyncStep(long? ByIndex = null, long? FieldId = null);\n"
               "public sealed record SyncDelta(uint PropertyId, IReadOnlyList<SyncStep> Path, string Kind, long Position, object Value, ulong Sequence);\n"
               "public sealed record PropertyEvent(string Property, IReadOnlyList<SyncStep> Path, string Kind, long Position, object Value);\n"
               "public sealed class PropertySync {\n"
               "  // Client-side property replica fed by server BASELINE/DELTAS frames.\n"
               "  // Mirrors the C++ SyncReceiver: per-property gapless sequences, atomic\n"
               "  // batch apply with a full rollback on any refusal, and a stale flag that\n"
               "  // only the next baseline clears. Handlers fire after the batch commits.\n"
               "  readonly string type;\n"
               "  readonly IReadOnlyDictionary<string, Action<PropertyEvent>> handlers;\n"
               "  Dictionary<string, object> replica = new Dictionary<string, object>();\n"
               "  ulong sequence = 0;\n"
               "  public bool Stale { get; private set; }\n"
               "  public ulong Sequence { get { return sequence; } }\n"
               "  public PropertySync(string type, IReadOnlyDictionary<string, Action<PropertyEvent>> handlers = null) {\n"
               "    if (!EntityMethods.TypeIds.ContainsKey(type)) throw new InvalidDataException(\"unknown entity type \" + type);\n"
               "    this.type = type;\n"
               "    this.handlers = handlers ?? new Dictionary<string, Action<PropertyEvent>>();\n"
               "  }\n"
               "  public object Get(string property) { return replica.TryGetValue(property, out var v) ? v : null; }\n"
               "  public void Baseline(IReadOnlyDictionary<string, object> values) {\n"
               "    var next = new Dictionary<string, object>();\n"
               "    foreach (var field in ContractSchemas.Types[type])\n"
               "      next[field.Name] = values != null && values.TryGetValue(field.Name, out var v) && v != null\n"
               "        ? CloneValue(v) : DefaultValue(field);\n"
               "    replica = next;\n"
               "    sequence = 0;\n"
               "    Stale = false;\n"
               "  }\n"
               "  public bool Apply(IReadOnlyList<SyncDelta> batch) {\n"
               "    if (Stale || batch.Count == 0) return false;\n"
               "    ulong expect = sequence + 1;\n"
               "    foreach (var delta in batch) {\n"
               "      if (delta.Sequence != expect) { Stale = true; return false; }\n"
               "      expect++;\n"
               "    }\n"
               "    var backup = (Dictionary<string, object>)CloneValue(replica);\n"
               "    var events = new List<PropertyEvent>();\n"
               "    foreach (var delta in batch) {\n"
               "      var e = Perform(replica, delta);\n"
               "      if (e == null) { replica = backup; Stale = true; return false; }\n"
               "      events.Add(e);\n"
               "    }\n"
               "    sequence = batch[batch.Count - 1].Sequence;\n"
               "    foreach (var e in events)\n"
               "      if (handlers.TryGetValue(e.Property, out var handler)) handler(e);\n"
               "    return true;\n"
               "  }\n"
               "  static object CloneValue(object v) {\n"
               "    if (v is Dictionary<string, object> dict) {\n"
               "      var copy = new Dictionary<string, object>();\n"
               "      foreach (var pair in dict) copy[pair.Key] = CloneValue(pair.Value);\n"
               "      return copy;\n"
               "    }\n"
               "    if (v is byte[] bytes) return (byte[])bytes.Clone();\n"
               "    if (v is List<object> objects) return new List<object>(objects.Select(CloneValue));\n"
               "    if (v is List<byte[]> chunks) return new List<byte[]>(chunks.Select(c => (byte[])c.Clone()));\n"
               "    if (v is List<long> longs) return new List<long>(longs);\n"
               "    if (v is List<bool> bools) return new List<bool>(bools);\n"
               "    if (v is List<string> texts) return new List<string>(texts);\n"
               "    return v;\n"
               "  }\n"
               "  static object DefaultValue(SprotoField field) {\n"
               "    switch (field.Kind) {\n"
               "      case SprotoKind.Int: return 0L;\n"
               "      case SprotoKind.Bool: return false;\n"
               "      case SprotoKind.String: return \"\";\n"
               "      case SprotoKind.Bytes: return new byte[0];\n"
               "      case SprotoKind.Struct: {\n"
               "        var dict = new Dictionary<string, object>();\n"
               "        if (ContractSchemas.Types.TryGetValue(field.Sub ?? \"\", out var fields))\n"
               "          foreach (var sub in fields) dict[sub.Name] = DefaultValue(sub);\n"
               "        return dict;\n"
               "      }\n"
               "      case SprotoKind.IntArray: return new List<long>();\n"
               "      case SprotoKind.BoolArray: return new List<bool>();\n"
               "      case SprotoKind.StringArray: return new List<string>();\n"
               "      case SprotoKind.BytesArray: return new List<byte[]>();\n"
               "      default: return new List<object>();\n"
               "    }\n"
               "  }\n"
               "  static SprotoField? ElementShape(SprotoField shape) {\n"
               "    switch (shape.Kind) {\n"
               "      case SprotoKind.StructArray: return new SprotoField(0, \"\", SprotoKind.Struct, shape.Sub);\n"
               "      case SprotoKind.IntArray: return new SprotoField(0, \"\", SprotoKind.Int, null);\n"
               "      case SprotoKind.BoolArray: return new SprotoField(0, \"\", SprotoKind.Bool, null);\n"
               "      case SprotoKind.StringArray: return new SprotoField(0, \"\", SprotoKind.String, null);\n"
               "      case SprotoKind.BytesArray: return new SprotoField(0, \"\", SprotoKind.Bytes, null);\n"
               "      default: return null;\n"
               "    }\n"
               "  }\n"
               "  static bool ValueOk(SprotoField shape, object value) {\n"
               "    switch (shape.Kind) {\n"
               "      case SprotoKind.Int: return value is long || value is int;\n"
               "      case SprotoKind.Bool: return value is bool;\n"
               "      case SprotoKind.String: return value is string;\n"
               "      case SprotoKind.Bytes: return value is byte[];\n"
               "      case SprotoKind.Struct: return value is Dictionary<string, object>;\n"
               "      default: return value is System.Collections.IList;\n"
               "    }\n"
               "  }\n"
               "  static object ReadSlot(object container, string key, int index) {\n"
               "    if (key != null && container is Dictionary<string, object> dict) return dict.TryGetValue(key, out var v) ? v : null;\n"
               "    if (key == null && container is System.Collections.IList list && index >= 0 && index < list.Count) return list[index];\n"
               "    return null;\n"
               "  }\n"
               "  static bool WriteSlot(object container, string key, int index, object value) {\n"
               "    if (key != null && container is Dictionary<string, object> dict) { dict[key] = value; return true; }\n"
               "    if (key == null && container is System.Collections.IList list && index >= 0 && index < list.Count) { list[index] = value; return true; }\n"
               "    return false;\n"
               "  }\n"
               "  PropertyEvent Perform(Dictionary<string, object> replica, SyncDelta delta) {\n"
               "    SprotoField field = default; bool found = false;\n"
               "    foreach (var f in ContractSchemas.Types[type]) if (f.Id == delta.PropertyId) { field = f; found = true; break; }\n"
               "    if (!found) return null;\n"
               "    object container = replica; string key = field.Name; int index = -1; var shape = field;\n"
               "    foreach (var step in delta.Path) {\n"
               "      var node = ReadSlot(container, key, index);\n"
               "      if (node == null) return null;\n"
               "      if (step.ByIndex.HasValue) {\n"
               "        var elem = ElementShape(shape);\n"
               "        if (elem == null || !(node is System.Collections.IList list)) return null;\n"
               "        long i = step.ByIndex.Value;\n"
               "        if (i < 0 || i >= list.Count) return null;\n"
               "        shape = elem.Value; container = node; key = null; index = (int)i;\n"
               "      } else if (step.FieldId.HasValue) {\n"
               "        if (shape.Kind != SprotoKind.Struct) return null;\n"
               "        long fid = step.FieldId.Value;\n"
               "        if (fid < 0 || fid > 65535) return null;\n"
               "        SprotoField sub = default; bool hit = false;\n"
               "        if (ContractSchemas.Types.TryGetValue(shape.Sub ?? \"\", out var fields))\n"
               "          foreach (var candidate in fields) if (candidate.Id == (ushort)fid) { sub = candidate; hit = true; break; }\n"
               "        if (!hit || !(node is Dictionary<string, object>)) return null;\n"
               "        shape = sub; container = node; key = sub.Name; index = -1;\n"
               "      } else return null;\n"
               "    }\n"
               "    long position = delta.Position;\n"
               "    if (delta.Kind == \"SET\") {\n"
               "      if (!ValueOk(shape, delta.Value)) return null;\n"
               "      if (!WriteSlot(container, key, index, CloneValue(delta.Value))) return null;\n"
               "    } else if (delta.Kind == \"INSERT\" || delta.Kind == \"REMOVE\") {\n"
               "      var elem = ElementShape(shape);\n"
               "      var node = ReadSlot(container, key, index);\n"
               "      if (elem == null || !(node is System.Collections.IList list)) return null;\n"
               "      if (position < 0) return null;\n"
               "      if (delta.Kind == \"INSERT\") {\n"
               "        if (position > list.Count || !ValueOk(elem.Value, delta.Value)) return null;\n"
               "        list.Insert((int)position, CloneValue(delta.Value));\n"
               "      } else {\n"
               "        if (position >= list.Count) return null;\n"
               "        list.RemoveAt((int)position);\n"
               "      }\n"
               "    } else return null;\n"
               "    return new PropertyEvent(field.Name, delta.Path, delta.Kind, position, delta.Value);\n"
               "  }\n"
               "}\n";
        auto emit_fields = [&](const std::string& name, const ContractArgs& fields) {
            out << "public static class " << name << "Schema { public static readonly SprotoField[] Fields = new SprotoField[] {";
            for (const auto& f : fields) out << " new(" << f.id << ", \"" << f.name << "\", " << cs_kind(f) << ", " << cs_sub(f) << "),";
            out << " }; }\n";
            type_names.push_back(name);
        };
        for (const auto& [name, fields] : structs_) emit_fields(name, fields);
        for (const auto& e : entities_) {
            emit_fields(e.name, properties_of(e));
            for (const auto& m : e.server_methods) if (client_callable(m)) {
                emit_fields(e.name+"_"+m.name+"Args", m.args);
                if (!m.returns.empty()) emit_fields(e.name+"_"+m.name+"Returns", m.returns);
            }
            for (const auto& m : e.client_methods) emit_fields(e.name+"_"+m.name+"Args", m.args);
        }
        out << "public static class ContractSchemas { public static readonly IReadOnlyDictionary<string, IReadOnlyList<SprotoField>> Types = new Dictionary<string, IReadOnlyList<SprotoField>> {";
        for (const auto& name : type_names) out << " [\"" << name << "\"] = " << name << "Schema.Fields,";
        out << " }; }\n";
        return out.str();
    }
    // Well-formedness of any XML document in this reader's dialect, without
    // any entity-grammar judgement; the generator feeds its own output back
    // through this so a malformed schema cannot ship.
    static bool well_formed(std::string_view xml) {
        try { (void)Reader(xml).document(); }
        catch (const ContractError&) { return false; }
        return true;
    }
private:
    // Kind tag emitted into every field table so the SDK codecs can drive
    // struct and array wiring: int covers every integer-like type
    // (UINT16/UINT32/INT32/INT64/EntityRef), struct and struct-array carry the
    // declared struct name in "of", int-array marks fixed-size integer
    // elements, and BOOL/STRING/BYTES shapes (scalar and array) are tagged as
    // their own kinds so each codec emits its upstream wire form.
    std::string field_kind(const ContractField& f) const {
        if (f.type == "ARRAY") {
            if (structs_.count(f.of)) return "struct-array";
            if (f.of == "BOOL") return "bool-array";
            if (f.of == "STRING") return "string-array";
            if (f.of == "BYTES") return "bytes-array";
            return "int-array";
        }
        if (structs_.count(f.type)) return "struct";
        if (f.type == "BOOL") return "bool";
        if (f.type == "STRING") return "string";
        if (f.type == "BYTES") return "bytes";
        return "int";
    }
    // ---- schema generator pieces: every emitted element pins its id and its
    // declared type, so a renumbered or re-typed member fails the schema.
    static void enum_block(std::ostringstream& out, const std::string& name,
                           const std::vector<std::string>& values) {
        out << "  <xs:simpleType name=\"" << name << "\"><xs:restriction base=\"xs:string\">";
        for (const auto& value : values) out << "<xs:enumeration value=\"" << value << "\"/>";
        out << "</xs:restriction></xs:simpleType>\n";
    }
    static void pinned(std::ostringstream& out, const char* key, const std::string& value) {
        out << "<xs:attribute name=\"" << key << "\" use=\"required\"><xs:simpleType>"
            << "<xs:restriction base=\"xs:string\"><xs:enumeration value=\"" << value
            << "\"/></xs:restriction></xs:simpleType></xs:attribute>";
    }
    static void group(std::ostringstream& out, const std::string& group, const std::string& body) {
        if (body.empty()) return;
        out << "<xs:element name=\"" << group << "\" minOccurs=\"0\" maxOccurs=\"1\"><xs:complexType>"
            << "<xs:choice>" << body << "</xs:choice></xs:complexType></xs:element>";
    }
    static void member_element(std::ostringstream& out, const ContractField& field, bool property) {
        out << "<xs:element name=\"" << field.name << "\" minOccurs=\"1\" maxOccurs=\"1\"><xs:complexType>";
        pinned(out, "id", std::to_string(field.id));
        pinned(out, "type", field.type);
        if (!field.of.empty()) pinned(out, "of", field.of);
        if (property)
            out << "<xs:attribute name=\"flags\" use=\"optional\" type=\"ReplicationFlags\"/>"
                << "<xs:attribute name=\"persistent\" use=\"optional\" type=\"Persistent\"/>";
        out << "<xs:attribute name=\"default\" use=\"optional\"/></xs:complexType></xs:element>";
    }
    static void arg_element(std::ostringstream& out, const char* tag, const ContractField& field) {
        out << "<xs:element name=\"" << tag << "\" minOccurs=\"0\" maxOccurs=\"1\"><xs:complexType>";
        pinned(out, "id", std::to_string(field.id));
        pinned(out, "type", field.type);
        if (!field.of.empty()) pinned(out, "of", field.of);
        out << "</xs:complexType></xs:element>";
    }
    static void entity_element(std::ostringstream& out, const ContractEntity& e) {
        out << "<xs:element name=\"Entity\" minOccurs=\"0\" maxOccurs=\"1\">"
            << "<xs:complexType><xs:choice>";
        out << "<xs:element name=\"" << e.name << "\" minOccurs=\"1\" maxOccurs=\"1\">"
            << "<xs:complexType><xs:sequence>";
        std::ostringstream props, server, client, reserved;
        for (const auto& p : e.properties)
            member_element(props, ContractField{p.name, p.type, p.of, p.default_value, p.id}, true);
        for (const auto& m : e.server_methods) method_element(server, m, true);
        for (const auto& m : e.client_methods) method_element(client, m, false);
        const auto pin_reserved = [&](const char* kind, unsigned r) {
            reserved << "<xs:element name=\"" << kind << "\" minOccurs=\"0\" maxOccurs=\"1\"><xs:complexType>";
            pinned(reserved, "id", std::to_string(r));
            reserved << "</xs:complexType></xs:element>";
        };
        for (unsigned r : e.reserved_properties) pin_reserved("Property", r);
        for (unsigned r : e.reserved_server_methods) pin_reserved("ServerMethod", r);
        for (unsigned r : e.reserved_client_methods) pin_reserved("ClientMethod", r);
        // Element content comes before attributes in a complexType.
        group(out, "Properties", props.str());
        group(out, "ServerMethods", server.str());
        group(out, "ClientMethods", client.str());
        group(out, "Reserved", reserved.str());
        out << "</xs:sequence>";
        pinned(out, "name", e.name);
        pinned(out, "id", std::to_string(e.id));
        out << "</xs:complexType></xs:element>";
        out << "</xs:choice></xs:complexType></xs:element>";
    }
    static void method_element(std::ostringstream& out, const ContractMethod& m, bool server_side) {
        out << "<xs:element name=\"" << m.name << "\" minOccurs=\"1\" maxOccurs=\"1\"><xs:complexType>";
        std::ostringstream body;
        for (const auto& a : m.args) arg_element(body, "Arg", a);
        for (const auto& r : m.returns) arg_element(body, "Field", r);
        out << "<xs:choice>" << body.str() << "</xs:choice>";
        pinned(out, "name", m.name);
        pinned(out, "id", std::to_string(m.id));
        pinned(out, "mode", m.mode);
        pinned(out, server_side ? "caller" : "audience", m.caller);
        out << "</xs:complexType></xs:element>";
    }
private:
    // ---- tiny XML subset reader: elements, double-quoted attributes,
    // comments and declarations; anything else (DOCTYPE, CDATA, entities,
    // namespaces, mixed content) is refused rather than guessed.
    struct Node {
        std::string name;
        std::vector<std::pair<std::string, std::string>> attrs;
        std::vector<Node> children;
        const std::string* attr(const char* key) const {
            for (const auto& [k, v] : attrs) if (k == key) return &v;
            return nullptr;
        }
    };
    class Reader {
    public:
        explicit Reader(std::string_view text) : text_(text) {}
        Node document() {
            skip_misc();
            Node root = element();
            skip_misc();
            if (at_ != text_.size()) fail("trailing content after the root element");
            return root;
        }
    private:
        std::string_view text_;
        std::size_t at_ = 0;
        [[noreturn]] void fail(const std::string& reason) const {
            throw ContractError("xml: "+reason);
        }
        bool ws(char ch) const { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'; }
        void skip_ws() { while (at_ < text_.size() && ws(text_[at_])) ++at_; }
        void skip_misc() {
            for (;;) {
                skip_ws();
                if (text_.compare(at_, 2, "<?") == 0) {
                    const auto end = text_.find("?>", at_);
                    if (end == std::string_view::npos) fail("unterminated declaration");
                    at_ = end+2;
                } else if (text_.compare(at_, 4, "<!--") == 0) {
                    const auto end = text_.find("-->", at_);
                    if (end == std::string_view::npos) fail("unterminated comment");
                    at_ = end+3;
                } else return;
            }
        }
        bool name_char(char ch) const {
            // ':' admits the xs: namespace prefix so the generated schema can
            // be re-parsed by this same reader; entity identifiers are
            // restricted separately by check_identifier.
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
                || ch == '_' || ch == '-' || ch == '.' || ch == ':';
        }
        std::string name() {
            if (at_ >= text_.size() || !name_char(text_[at_]) || (text_[at_] >= '0' && text_[at_] <= '9'))
                fail("expected a name");
            std::string out;
            while (at_ < text_.size() && name_char(text_[at_])) out += text_[at_++];
            return out;
        }
        std::string quoted() {
            if (at_ >= text_.size() || text_[at_] != '"') fail("attribute values must be double-quoted");
            std::string out;
            for (++at_; at_ < text_.size() && text_[at_] != '"'; ++at_) {
                if (text_[at_] == '<' || text_[at_] == '&') fail("raw '<' or '&' in an attribute value");
                out += text_[at_];
            }
            if (at_++ >= text_.size()) fail("unterminated attribute value");
            return out;
        }
        Node element() {
            if (at_ >= text_.size() || text_[at_++] != '<') fail("expected an element");
            if (at_ < text_.size() && text_[at_] == '!')
                fail("DOCTYPE, ENTITY and CDATA are refused: XML here is data, not code");
            Node node;
            node.name = name();
            for (;;) {
                skip_ws();
                if (at_ >= text_.size()) fail("unterminated tag");
                if (text_[at_] == '>') { ++at_; break; }
                if (text_.compare(at_, 2, "/>") == 0) { at_ += 2; return node; }
                const auto key = name();
                skip_ws();
                if (at_ >= text_.size() || text_[at_++] != '=') fail("expected '=' after an attribute name");
                skip_ws();
                for (const auto& [seen, value] : node.attrs)
                    if (seen == key) fail("attribute '"+key+"' declared twice on <"+node.name+">");
                node.attrs.emplace_back(std::move(key), quoted());
            }
            for (;;) {
                skip_misc();
                if (text_.compare(at_, 2, "</") == 0) {
                    at_ += 2;
                    const auto close = name();
                    skip_ws();
                    if (at_ >= text_.size() || text_[at_++] != '>') fail("unterminated close tag");
                    if (close != node.name) fail("close tag </"+close+"> does not match <"+node.name+">");
                    return node;
                }
                if (at_ >= text_.size()) fail("missing close tag for <"+node.name+">");
                node.children.push_back(element());
            }
        }
    };
    // ---- resolved model checks
    static void field_line(std::ostringstream& out, const ContractField& f) {
        out << '{' << f.id << ' ' << f.name << ' ' << f.type;
        if (!f.of.empty()) out << " of " << f.of;
        if (!f.default_value.empty()) out << " default " << f.default_value;
        out << '}';
    }
    static void method_line(std::ostringstream& out, const char* side, const ContractMethod& m) {
        out << " method " << side << ' ' << m.id << ' ' << m.name << ' ' << m.mode << ' ' << m.caller;
        for (const auto& a : m.args) field_line(out, a);
        for (const auto& r : m.returns) field_line(out, r);
        out << '\n';
    }
    static unsigned decimal(const std::string& text, const std::string& what) {
        if (text.empty() || text.size() > 9)
            throw ContractError(what+": id must be an explicit decimal, got '"+text+"'");
        unsigned value = 0;
        for (unsigned char ch : text) {
            if (ch < '0' || ch > '9') throw ContractError(what+": id must be an explicit decimal, got '"+text+"'");
            value = value*10 + ch-'0';
        }
        return value;
    }
    static std::string required(const Node& node, const char* key, const std::string& what) {
        const auto* value = node.attr(key);
        if (!value || value->empty()) throw ContractError(what+": missing required attribute '"+key+"'");
        return *value;
    }
    static void reserve(unsigned id, std::vector<unsigned>& reserved, const std::string& what) {
        for (unsigned seen : reserved)
            if (seen == id) throw ContractError(what+": reserved id "+std::to_string(id)+" is declared twice");
        reserved.push_back(id);
    }
    static void check_identifier(const std::string& name, const std::string& what) {
        if (name.empty() || name.size() > 64) throw ContractError(what+": name must be 1..64 chars");
        for (unsigned char ch : name)
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_'))
                throw ContractError(what+": name '"+name+"' is not [A-Za-z0-9_]");
        static const std::set<std::string_view> reserved{
            "Types", "TypesSource", "Entity", "Properties", "ServerMethods",
            "ClientMethods", "Reserved", "Method", "Returns"};
        if (reserved.count(name))
            throw ContractError(what+": name '"+name+"' is a reserved XML element name");
    }
    static bool basic_type(const std::string& type) {
        return type == "BOOL" || type == "INT32" || type == "UINT16" || type == "UINT32"
            || type == "INT64" || type == "STRING" || type == "BYTES" || type == "EntityRef";
    }
    void check_type(const std::string& type, const std::string& of, const std::string& what) const {
        if (type == "ARRAY") {
            if (of.empty()) throw ContractError(what+": ARRAY needs 'of'");
            if (of == "ARRAY" || (!basic_type(of) && !structs_.count(of)))
                throw ContractError(what+": ARRAY of '"+of+"' does not resolve");
            return;
        }
        if (!of.empty()) throw ContractError(what+": 'of' only applies to ARRAY");
        if (!basic_type(type) && !structs_.count(type))
            throw ContractError(what+": type '"+type+"' is neither built-in nor a declared struct");
    }
    static void check_default(const ContractField& f, const std::string& what) {
        const auto& value = f.default_value;
        if (value.empty()) return;
        const auto digits = [&](bool signed_ok) {
            if (value.empty()) throw ContractError(what+": default '"+value+"' is not a number");
            std::size_t at = value.front() == '-' ? 1 : 0;
            if (!signed_ok && at) throw ContractError(what+": default '"+value+"' must be unsigned");
            if (at == value.size() || value.size()-at > 20)
                throw ContractError(what+": default '"+value+"' is not a number");
            for (std::size_t i = at; i < value.size(); ++i)
                if (value[i] < '0' || value[i] > '9') throw ContractError(what+": default '"+value+"' is not a number");
            const unsigned long long magnitude = std::stoull(value.substr(at));
            unsigned long long limit = 0;
            if (f.type == "UINT16") limit = 65535;
            else if (f.type == "UINT32") limit = 4294967295u;
            else if (f.type == "INT32") limit = 2147483647;
            else if (f.type == "INT64") limit = 9223372036854775807ull;
            if (magnitude > limit) throw ContractError(what+": default '"+value+"' exceeds "+f.type);
        };
        if (f.type == "BOOL") {
            if (value != "true" && value != "false")
                throw ContractError(what+": BOOL default '"+value+"' must be true or false");
        } else if (f.type == "INT32" || f.type == "INT64") digits(true);
        else if (f.type == "UINT16" || f.type == "UINT32") digits(false);
        else if (f.type == "STRING") return;
        else throw ContractError(what+": explicit defaults for type '"+f.type+"' are not defined");
    }
    static ContractField read_field(const Node& node, const std::string& what) {
        ContractField f;
        f.name = node.name;
        f.type = required(node, "type", what);
        if (const auto* of = node.attr("of")) f.of = *of;
        if (const auto* def = node.attr("default")) f.default_value = *def;
        f.id = decimal(required(node, "id", what), what);
        check_identifier(f.name, what);
        return f;
    }
    static ContractMethod read_method(const Node& node, bool server_side, const std::string& what) {
        ContractMethod m;
        m.name = required(node, "name", what);
        m.id = decimal(required(node, "id", what), what);
        m.mode = required(node, "mode", what);
        if (m.mode != "notify" && m.mode != "request")
            throw ContractError(what+": mode '"+m.mode+"' is neither notify nor request");
        if (!server_side && m.mode == "request")
            throw ContractError(what+": client methods are notify-only in the first version");
        m.caller = required(node, server_side ? "caller" : "audience", what);
        if (server_side && m.caller != "owner" && m.caller != "server")
            throw ContractError(what+": caller '"+m.caller+"' is not owner or server; other policies default to denial");
        if (!server_side && m.caller != "owner")
            throw ContractError(what+": audience '"+m.caller+"' is not owner");
        std::map<unsigned, std::string> arg_ids;
        bool returns_seen = false;
        for (const auto& child : node.children) {
            if (child.name == "Arg" || child.name == "Field") {
                auto arg = read_field(child, what+" arg");
                arg.name = required(child, "name", what+" arg");
                check_identifier(arg.name, what+" arg");
                if (const auto seen = arg_ids.find(arg.id); seen != arg_ids.end())
                    throw ContractError(what+": argument id "+std::to_string(arg.id)+" declared twice");
                arg_ids.emplace(arg.id, arg.name);
                (child.name == "Arg" ? m.args : m.returns).push_back(std::move(arg));
            } else if (child.name == "Returns" && server_side) {
                returns_seen = true;
                for (const auto& field : child.children) {
                    auto ret = read_field(field, what+" return");
                    ret.name = required(field, "name", what+" return");
                    check_identifier(ret.name, what+" return");
                    if (const auto seen = arg_ids.find(ret.id); seen != arg_ids.end())
                        throw ContractError(what+": return field id "+std::to_string(ret.id)+" collides with an argument id");
                    arg_ids.emplace(ret.id, ret.name);
                    m.returns.push_back(std::move(ret));
                }
            } else throw ContractError(what+": unexpected element <"+child.name+"> inside a Method");
        }
        if (m.mode == "request" && m.returns.empty())
            throw ContractError(what+": request methods declare typed returns");
        if (m.mode == "notify" && (returns_seen || !m.returns.empty()))
            throw ContractError(what+": notify methods have no business response");
        return m;
    }
    ContractProperty read_property(const Node& node, const std::string& what) const {
        ContractProperty p;
        p.name = node.name;
        p.type = required(node, "type", what);
        if (const auto* of = node.attr("of")) p.of = *of;
        if (const auto* def = node.attr("default")) p.default_value = *def;
        p.id = decimal(required(node, "id", what), what);
        check_identifier(p.name, what);
        if (const auto* flags = node.attr("flags")) {
            if (*flags != "OWN_CLIENT" && *flags != "ALL_CLIENTS" && *flags != "SERVER_ONLY")
                throw ContractError(what+": unknown flags '"+*flags+"' are errors, not defaults");
            p.flags = *flags;
        } else p.flags = "SERVER_ONLY";
        if (const auto* persistent = node.attr("persistent")) {
            if (*persistent != "true" && *persistent != "false")
                throw ContractError(what+": persistent must be true or false");
            p.persistent = *persistent == "true";
        }
        return p;
    }
    void read_types(const Node& types) {
        for (const auto& struct_node : types.children) {
            const auto what = "struct "+struct_node.name;
            check_identifier(struct_node.name, what);
            if (structs_.count(struct_node.name))
                throw ContractError(what+": a type is defined once");
            if (struct_node.name == "ARRAY" || basic_type(struct_node.name))
                throw ContractError(what+": struct names may not shadow built-in type names");
            ContractArgs fields;
            std::map<unsigned, std::string> ids;
            for (const auto& field_node : struct_node.children) {
                auto field = read_field(field_node, what+" field "+field_node.name);
                if (const auto seen = ids.find(field.id); seen != ids.end())
                    throw ContractError(what+": field id "+std::to_string(field.id)+" declared twice");
                ids.emplace(field.id, field.name);
                fields.push_back(std::move(field));
            }
            structs_.emplace(struct_node.name, std::move(fields));
        }
    }
    // <TypesSource path="..."> pulls a type document into the contract. The
    // XML reader never opens files, so only a resolver a caller supplied may
    // reach the filesystem; type documents merge through read_types, which
    // already refuses a type defined twice.
    void include_type_source(const Node& node, const std::function<std::string(const std::string&)>& type_resolver) {
        if (!type_resolver) throw ContractError("TypesSource needs a type document resolver");
        const std::string what = "TypesSource";
        const std::string path = required(node, "path", what);
        if (!node.children.empty())
            throw ContractError(what+": <"+node.children.front().name+"> is not allowed inside a TypesSource");
        const auto text = type_resolver(path);
        const auto root = Reader(text).document();
        if (root.name != "Types")
            throw ContractError("type source '"+path+"' must be a <Types> document, not <"+root.name+">");
        read_types(root);
    }
    void finalize() {
        // Document order is irrelevant: entity groups may reference a type
        // from a later TypesSource, so type/default checks run only after the
        // whole document (and its sources) have been merged.
        check_recursion();
        const auto check_field_tags = [&](const ContractArgs& fields, const std::string& where) {
            for (const auto& field : fields)
                if (field.id > 32766)
                    throw ContractError(where+" field id "+std::to_string(field.id)
                                        +" exceeds the sproto 16-bit tag limit 32766");
        };
        for (auto& [name, fields] : structs_)
            for (auto& f : fields) {
                const auto what = "struct "+name+" field "+f.name;
                check_field_tags(fields, "struct "+name);
                check_type(f.type, f.of, what);
                check_default(f, what);
            }
        for (const auto& e : entities_) {
            const auto what = "entity "+e.name;
            for (const auto& p : e.properties) {
                const auto pwhat = what+" property "+p.name;
                if (p.id > 32766)
                    throw ContractError(pwhat+" id "+std::to_string(p.id)
                                        +" exceeds the sproto 16-bit tag limit 32766");
                check_type(p.type, p.of, pwhat);
                check_default(ContractField{p.name, p.type, p.of, p.default_value, p.id}, pwhat);
            }
            const auto check_methods = [&](const std::vector<ContractMethod>& list, const char* side) {
                for (const auto& m : list) {
                    const auto mwhat = what+" method "+m.name+" ["+side+"]";
                    check_field_tags(m.args, mwhat+" args");
                    check_field_tags(m.returns, mwhat+" returns");
                    for (auto& a : m.args) check_type(a.type, a.of, mwhat+" arg "+a.name);
                    for (auto& r : m.returns) check_type(r.type, r.of, mwhat+" return "+r.name);
                }
            };
            check_methods(e.server_methods, "server");
            check_methods(e.client_methods, "client");
        }
    }
    void check_recursion() const {
        // By-value structs must terminate: a struct reaching itself through
        // struct references or ARRAY-of chains is a contract error.
        std::map<std::string, int> state; // 0 unseen, 1 in flight, 2 done
        std::function<void(const std::string&)> visit = [&](const std::string& name) {
            auto& mark = state[name];
            if (mark == 1) throw ContractError("struct "+name+": recursive types are refused");
            if (mark == 2) return;
            mark = 1;
            if (const auto found = structs_.find(name); found != structs_.end())
                for (const auto& field : found->second) {
                    if (field.type == "ARRAY" ? structs_.count(field.of) : structs_.count(field.type))
                        visit(field.type == "ARRAY" ? field.of : field.type);
                }
            mark = 2;
        };
        for (const auto& [name, fields] : structs_) visit(name);
    }
    void read_entity(const Node& node) {
        const std::string what = "entity "+node.name;
        ContractEntity e;
        e.name = required(node, "name", what);
        check_identifier(e.name, what);
        e.id = decimal(required(node, "id", what), what);
        for (const auto& found : entities_)
            if (found.name == e.name) throw ContractError(what+": entity names are unique");
            else if (found.id == e.id) throw ContractError(what+": entity type id "+std::to_string(e.id)+" is already taken");
        std::map<unsigned, std::string> property_ids, server_ids, client_ids;
        std::map<std::string, std::string> member_names;
        const auto claim_name = [&](const std::string& name) {
            if (member_names.count(name))
                throw ContractError(what+": member name '"+name+"' is duplicated across groups");
            member_names.emplace(name, name);
        };
        bool properties_seen = false, server_seen = false, client_seen = false, reserved_seen = false;
        for (const auto& child : node.children) {
            if (child.name == "Properties") {
                if (properties_seen) throw ContractError(what+": one Properties group per entity");
                properties_seen = true;
                for (const auto& prop_node : child.children) {
                    auto p = read_property(prop_node, what+" property "+prop_node.name);
                    if (property_ids.count(p.id))
                        throw ContractError(what+": property id "+std::to_string(p.id)+" declared twice");
                    property_ids.emplace(p.id, p.name);
                    claim_name(p.name);
                    e.properties.push_back(std::move(p));
                }
            } else if (child.name == "ServerMethods" || child.name == "ClientMethods") {
                const bool server_side = child.name == "ServerMethods";
                auto& seen_ids = server_side ? server_ids : client_ids;
                if (server_side ? server_seen : client_seen)
                    throw ContractError(what+": one "+child.name+" group per entity");
                (server_side ? server_seen : client_seen) = true;
                for (const auto& method_node : child.children) {
                    if (method_node.name != "Method")
                        throw ContractError(what+": unexpected element <"+method_node.name+"> in "+child.name);
                    auto m = read_method(method_node, server_side, what+" method "+method_node.name);
                    if (seen_ids.count(m.id))
                        throw ContractError(what+": "+child.name+" id "+std::to_string(m.id)+" declared twice");
                    seen_ids.emplace(m.id, m.name);
                    claim_name(m.name);
                    (server_side ? e.server_methods : e.client_methods).push_back(std::move(m));
                }
            } else if (child.name == "Reserved") {
                if (reserved_seen) throw ContractError(what+": one Reserved group per entity");
                reserved_seen = true;
                // Retired ids: collected here, checked against the live ids
                // after the groups are read so the order of the groups does
                // not matter.
                const auto what2 = what+" Reserved";
                for (const auto& entry : child.children) {
                    if (entry.attrs.size() != 1)
                        throw ContractError(what2+": a reserved entry carries only 'id'");
                    const auto value = decimal(required(entry, "id", what2), what2);
                    if (entry.name == "Property") reserve(value, e.reserved_properties, what2+" Property");
                    else if (entry.name == "ServerMethod")
                        reserve(value, e.reserved_server_methods, what2+" ServerMethod");
                    else if (entry.name == "ClientMethod")
                        reserve(value, e.reserved_client_methods, what2+" ClientMethod");
                    else throw ContractError(what2+": unexpected element <"+entry.name+">");
                }
            } else throw ContractError(what+": unexpected element <"+child.name+"> inside an Entity");
        }
        // A retired id may never be live in the same contract: that is what
        // distinguishes "this tag is free" from "this tag is retired".
        const auto collides = [&](const std::vector<unsigned>& reserved,
                                  const std::map<unsigned, std::string>& live, const char* kind) {
            for (unsigned r : reserved)
                if (const auto taken = live.find(r); taken != live.end())
                    throw ContractError(what+": reserved "+kind+" id "+std::to_string(r)
                                        +" is still declared as '"+taken->second+"'");
        };
        collides(e.reserved_properties, property_ids, "property");
        collides(e.reserved_server_methods, server_ids, "server method");
        collides(e.reserved_client_methods, client_ids, "client method");
        entities_.push_back(std::move(e));
    }
    void load(std::string_view xml, std::function<std::string(const std::string&)> type_resolver) {
        const auto root = Reader(xml).document();
        if (root.name == "Contracts") {
            bool types_seen = false;
            for (const auto& child : root.children) {
                if (child.name == "Types") {
                    if (types_seen) throw ContractError("one Types block per document");
                    types_seen = true;
                    read_types(child);
                } else if (child.name == "TypesSource") include_type_source(child, type_resolver);
                else if (child.name == "Entity") read_entity(child);
                else throw ContractError("unexpected element <"+child.name+"> in Contracts");
            }
        } else if (root.name == "Types") read_types(root);
        else if (root.name == "Entity") read_entity(root);
        else throw ContractError("root element must be Contracts, Types or Entity, not <"+root.name+">");
        finalize();
    }
    std::map<std::string, ContractArgs> structs_;
    std::vector<ContractEntity> entities_;
};
// Compact entry rendering for reports: "prop Avatar gold 2",
// "method Avatar attack 1 [server]", "reserved-property Avatar 5".
inline std::string entry_text(const ContractEntry& e) {
    std::string text = e.kind;
    if (!e.owner.empty()) text += " " + e.owner;
    if (!e.name.empty()) text += " " + e.name;
    text += " " + std::to_string(e.id);
    if (!e.sub.empty()) text += " [" + e.sub + "]";
    return text;
}
// Read a release lock back into its entries. Comment lines are skipped; every
// other line must be a known shape and each assignment may appear exactly
// once, or the lock is refused outright — a gate that silently skips a line
// it does not understand, or resolves a duplicated assignment by position,
// would let that assignment drift. The result is sorted like
// EntityContract::entries(), so a model and its lock text compare equal
// regardless of file line order.
inline std::vector<ContractEntry> parse_lock(std::string_view text) {
    std::vector<ContractEntry> entries;
    const auto fail = [](std::string_view line) {
        throw ContractError("unknown release lock line: "+std::string(line));
    };
    const auto has_trailing = [](std::istringstream& row) {
        std::string extra;
        return static_cast<bool>(row >> extra);
    };
    // The lock's two element-type spellings: struct fields write " of X" inside
    // their braces, prop/arg/return lines carry the element as a bare trailing
    // token. Both normalize to "TYPE of ELEM" so the model and lock compare
    // equal; anything beyond that one token is refused.
    const auto read_of = [&](std::istringstream& row, std::string& spec, std::string_view line) {
        std::string of;
        if (!(row >> of)) return;
        if (of != "of" || !(row >> of)) fail(line);
        spec += " of " + of;
        if (has_trailing(row)) fail(line);
    };
    const auto read_bare_of = [&](std::istringstream& row, std::string& spec, std::string_view line) {
        std::string of;
        if (!(row >> of)) return;
        spec += " of " + of;
        if (has_trailing(row)) fail(line);
    };
    std::size_t at = 0;
    while (at < text.size()) {
        auto end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        const auto line = text.substr(at, end-at);
        at = end+1;
        if (line.empty() || line.front() == '#') continue;
        std::istringstream in{std::string(line)};
        std::string kind;
        in >> kind;
        if (kind == "entity") {
            ContractEntry e{"entity", "", "", "", "", 0};
            if (!(in >> e.name >> e.id)) fail(line);
            e.spec = std::to_string(e.id);
            if (has_trailing(in)) fail(line);
            entries.push_back(e);
        } else if (kind == "prop") {
            ContractEntry e{"prop", "", "", "", "", 0};
            if (!(in >> e.owner >> e.id >> e.name >> e.spec)) fail(line);
            read_bare_of(in, e.spec, line);
            entries.push_back(e);
        } else if (kind == "method") {
            ContractEntry e{"method", "", "", "", "", 0};
            std::string mode, caller;
            if (!(in >> e.owner >> e.sub >> e.id >> e.name >> mode >> caller)) fail(line);
            e.spec = mode + " " + caller;
            if (has_trailing(in)) fail(line);
            entries.push_back(e);
        } else if (kind == "arg" || kind == "return") {
            ContractEntry e{kind, "", "", "", "", 0};
            std::string side;
            unsigned method_id = 0;
            if (!(in >> e.owner >> side >> method_id >> e.id >> e.name >> e.spec)) fail(line);
            e.sub = side + " " + std::to_string(method_id);
            read_bare_of(in, e.spec, line);
            entries.push_back(e);
        } else if (kind == "struct") {
            std::string owner;
            if (!(in >> owner)) fail(line);
            const auto rest = std::string(std::istreambuf_iterator<char>(in), {});
            std::size_t pos = 0;
            bool any_field = false;
            while (true) {
                const auto open = rest.find('{', pos);
                if (open == std::string::npos) break;
                const auto close = rest.find('}', open);
                if (close == std::string::npos) fail(line);
                std::istringstream field{rest.substr(open+1, close-open-1)};
                ContractEntry e{"struct", owner, "", "", "", 0};
                if (!(field >> e.id >> e.name >> e.spec)) fail(line);
                read_of(field, e.spec, line);
                entries.push_back(e);
                any_field = true;
                pos = close+1;
            }
            if (!any_field) fail(line);
            if (rest.substr(pos).find_first_not_of(" \t\r\n") != std::string::npos) fail(line);
        } else if (kind == "reserved") {
            ContractEntry e{"", "", "", "", "", 0};
            std::string family;
            if (!(in >> e.owner >> family >> e.id)) fail(line);
            if (family == "property") e.kind = "reserved-property";
            else if (family == "server") e.kind = "reserved-server";
            else if (family == "client") e.kind = "reserved-client";
            else fail(line);
            if (has_trailing(in)) fail(line);
            entries.push_back(e);
        } else fail(line);
    }
    std::sort(entries.begin(), entries.end(),
              [](const ContractEntry& a, const ContractEntry& b) { return a.key() < b.key(); });
    for (std::size_t i = 1; i < entries.size(); ++i)
        if (entries[i-1].key() == entries[i].key())
            throw ContractError("duplicate release lock entry: "+entry_text(entries[i]));
    return entries;
}
// Publication gate: compare two entry lists (a live model's entries() or a
// parsed lock) and classify every move. Breaking is anything a published
// client could depend on disappearing, renumbering or changing meaning —
// including a retired id coming back as a live meaning and a reservation
// being dropped, which would silently re-open the id for future reuse.
// Additions are compatible. Reports are deterministic: maps iterate in key
// order, so the same pair of locks always produces the same text.
inline CompatDiff compat_diff(const std::vector<ContractEntry>& prev, const std::vector<ContractEntry>& curr) {
    using Key = std::tuple<std::string, std::string, std::string, std::string, unsigned>;
    using Name = std::tuple<std::string, std::string, std::string, std::string>;
    const auto reserved = [](const std::string& kind) {
        return kind.rfind("reserved-", 0) == 0;
    };
    std::map<Key, const ContractEntry*> old, now;
    for (const auto& e : prev) old[e.key()] = &e;
    for (const auto& e : curr) now[e.key()] = &e;
    std::map<Name, const ContractEntry*> old_by_name, now_by_name;
    for (const auto& slot : old)
        if (!reserved(slot.second->kind)) old_by_name[{slot.second->kind, slot.second->owner, slot.second->name, slot.second->sub}] = slot.second;
    for (const auto& slot : now)
        if (!reserved(slot.second->kind)) now_by_name[{slot.second->kind, slot.second->owner, slot.second->name, slot.second->sub}] = slot.second;
    const auto render = [](const ContractEntry& e) { return entry_text(e); };
    CompatDiff diff;
    for (const auto& slot : old) {
        const auto it = now.find(slot.first);
        if (it == now.end()) continue;
        if (it->second->spec != slot.second->spec)
            diff.breaking.push_back("changed: "+render(*slot.second)+" "+slot.second->spec+" -> "+it->second->spec);
    }
    for (const auto& slot : old) {
        if (reserved(slot.second->kind) || now.contains(slot.first)) continue;
        const auto renamed = now_by_name.find({slot.second->kind, slot.second->owner, slot.second->name, slot.second->sub});
        if (renamed != now_by_name.end())
            diff.breaking.push_back("renumbered: "+render(*slot.second)+" -> "+std::to_string(renamed->second->id));
        else
            diff.breaking.push_back("removed: "+render(*slot.second));
    }
    for (const auto& slot : now) {
        if (reserved(slot.second->kind) || old.contains(slot.first)) continue;
        if (old_by_name.contains({slot.second->kind, slot.second->owner, slot.second->name, slot.second->sub}))
            continue; // same meaning at a new id: reported as a renumber above
        diff.additive.push_back("added: "+render(*slot.second)+" = "+slot.second->spec);
    }
    // Retired ids: any live use against a listed reservation is reuse.
    std::set<std::tuple<std::string, std::string, unsigned>> retired;
    for (const auto* table : {&old, &now})
        for (const auto& slot : *table)
            if (reserved(slot.second->kind))
                retired.insert({slot.second->kind.substr(9), slot.second->owner, slot.second->id});
    const auto family = [](const ContractEntry& e) -> std::string {
        if (e.kind == "prop") return "property";
        if (e.kind == "method" && (e.sub == "server" || e.sub == "client")) return e.sub;
        return "";
    };
    for (const auto& slot : now) {
        const auto live = family(*slot.second);
        if (!live.empty() && retired.contains({live, slot.second->owner, slot.second->id}))
            diff.breaking.push_back("reserved id reused by "+render(*slot.second));
    }
    for (const auto& slot : old)
        if (reserved(slot.second->kind) && !now.contains(slot.first))
            diff.breaking.push_back("dropped reservation: "+render(*slot.second));
    return diff;
}
}
