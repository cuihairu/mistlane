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
    explicit EntityContract(std::string_view xml) { load(xml); }
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
    std::string javascript_bindings() const {
        std::ostringstream out;
        out << "// Generated by mistlane contract build; do not edit.\n";
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
        for (const auto& [name, fields] : structs_) emit(name, fields);
        for (const auto& e : entities_) {
            ContractArgs properties;
            for (const auto& p : e.properties) properties.push_back({p.name,p.type,p.of,p.default_value,p.id});
            emit(e.name, properties);
            for (const auto& m : e.server_methods) emit(e.name+"_"+m.name+"_Args", m.args);
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
            ContractArgs properties; for (const auto& p : e.properties) properties.push_back({p.name,p.type,p.of,p.default_value,p.id});
            emit_fields(e.name, properties);
            for (const auto& m : e.server_methods) emit_fields(e.name+"_"+m.name+"_Args", m.args);
            for (const auto& m : e.client_methods) emit_fields(e.name+"_"+m.name+"_Args", m.args);
        }
        out << "};\n\n"
               "// Standard sproto codec for the generated tag tables. Integers use the\n"
               "// canonical wire forms: an in-band short word for 0..0x7ffe, a 4-byte little-\n"
               "// endian int32, and an 8-byte two's-complement int64. Struct fields are\n"
               "// length-prefixed records, integer arrays carry an intlen byte before 4- or\n"
               "// 8-byte elements, and arrays of structs are per-element [len][record].\n"
               "// Values cross as bigint when they do not fit a JS Number. Encoding matches\n"
               "// upstream cloudwu/sproto byte for byte; decode skips tags absent from the\n"
               "// table, rejects malformed buffers, and widens every dword from unsigned\n"
               "// bytes instead of upstream's todword int arithmetic, so negative wide\n"
               "// integers survive. BOOL/STRING/BYTES fields are emitted into the tables\n"
               "// but rejected by the codec until their wire forms land.\n"
               "const INT64_MIN = -9223372036854775808n;\n"
               "const INT64_MAX = 9223372036854775807n;\n"
               "const INT32_MIN = -2147483648n;\n"
               "const INT32_MAX = 2147483647n;\n\n"
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
               "      if (field && field.kind !== 'int') throw new RangeError(`in-band value for non-integer ${field.name}`);\n"
               "      if (field) out[field.name] = BigInt(word / 2 - 1);\n"
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
               "    } else if (field) {\n"
               "      throw new RangeError(`unsupported field type ${field.kind}`);\n"
               "    }\n"
               "    dataPos = start + length;\n"
               "  }\n"
               "  if (dataPos !== bytes.length) throw new RangeError('trailing bytes');\n"
               "  return out;\n"
               "}\n";
        return out.str();
    }
    std::string csharp_bindings() const {
        std::ostringstream out;
        out << "// Generated by mistlane contract build; do not edit.\n"
               "using System;\nusing System.Collections.Generic;\nusing System.IO;\nusing System.Linq;\n"
               "namespace Mistlane.Contracts;\n"
               "public enum SprotoKind { Int, Struct, IntArray, StructArray, Unsupported }\n"
               "public readonly record struct SprotoField(ushort Id, string Name, SprotoKind Kind = SprotoKind.Int, string Sub = null);\n"
               "public static class SprotoCodec {\n"
               "  // Standard sproto codec for the generated tag tables. Struct fields are\n"
               "  // length-prefixed records, integer arrays carry an intlen byte before 4-\n"
               "  // or 8-byte elements, and arrays of structs are per-element\n"
               "  // [len][record]. Dword reads widen each byte through uint before\n"
               "  // shifting: the low dword of a wide integer may carry its sign bit,\n"
               "  // and int arithmetic here would re-create the upstream todword\n"
               "  // sign-extension defect. BOOL/STRING/BYTES fields are emitted into the\n"
               "  // tables but rejected here until their wire forms land.\n"
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
               "        if (known && field.Kind != SprotoKind.Int) throw new InvalidDataException(\"in-band value for non-integer \" + field.Name);\n"
               "        if (known) result[field.Name] = (long)(word/2 - 1);\n"
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
            ContractArgs properties; for (const auto& p : e.properties) properties.push_back({p.name,p.type,p.of,p.default_value,p.id});
            emit(e.name, properties);
            for (const auto& m : e.server_methods) emit(e.name+"_"+m.name+"Args", m.args);
            for (const auto& m : e.client_methods) emit(e.name+"_"+m.name+"Args", m.args);
        }
        auto emit_fields = [&](const std::string& name, const ContractArgs& fields) {
            out << "public static class " << name << "Schema { public static readonly SprotoField[] Fields = new SprotoField[] {";
            for (const auto& f : fields) out << " new(" << f.id << ", \"" << f.name << "\", " << cs_kind(f) << ", " << cs_sub(f) << "),";
            out << " }; }\n";
            type_names.push_back(name);
        };
        for (const auto& [name, fields] : structs_) emit_fields(name, fields);
        for (const auto& e : entities_) {
            ContractArgs properties; for (const auto& p : e.properties) properties.push_back({p.name,p.type,p.of,p.default_value,p.id});
            emit_fields(e.name, properties);
            for (const auto& m : e.server_methods) emit_fields(e.name+"_"+m.name+"Args", m.args);
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
    // elements, and BOOL/STRING/BYTES shapes are tagged as their own kinds the
    // codecs reject until those wire forms land.
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
        for (const auto& value : values) out << "<xs:enumeration xs:value=\"" << value << "\"/>";
        out << "</xs:restriction></xs:simpleType>\n";
    }
    static void pinned(std::ostringstream& out, const char* key, const std::string& value) {
        out << "<xs:attribute name=\"" << key << "\" use=\"required\"><xs:simpleType>"
            << "<xs:restriction base=\"xs:string\"><xs:enumeration xs:value=\"" << value
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
        out << "<xs:element name=\"Entity\" minOccurs=\"0\" maxOccurs=\"1\"><xs:complexType><xs:choice>";
        out << "<xs:element name=\"" << e.name << "\" minOccurs=\"1\" maxOccurs=\"1\"><xs:complexType>";
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
        check_type(p.type, p.of, what);
        check_default(ContractField{p.name, p.type, p.of, p.default_value, p.id}, what);
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
                    for (auto& a : m.args) check_type(a.type, a.of, what+" method "+m.name+" arg "+a.name);
                    for (auto& r : m.returns) check_type(r.type, r.of, what+" method "+m.name+" return "+r.name);
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
    void load(std::string_view xml) {
        const auto root = Reader(xml).document();
        if (root.name == "Contracts") {
            bool types_seen = false;
            for (const auto& child : root.children) {
                if (child.name == "Types") {
                    if (types_seen) throw ContractError("one Types block per document");
                    types_seen = true;
                    read_types(child);
                } else if (child.name == "Entity") read_entity(child);
                else throw ContractError("unexpected element <"+child.name+"> in Contracts");
            }
        } else if (root.name == "Types") read_types(root);
        else if (root.name == "Entity") read_entity(root);
        else throw ContractError("root element must be Contracts, Types or Entity, not <"+root.name+">");
        check_recursion();
        for (auto& [name, fields] : structs_)
            for (auto& f : fields) {
                const auto what = "struct "+name+" field "+f.name;
                check_type(f.type, f.of, what);
                check_default(f, what);
            }
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
