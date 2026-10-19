// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace mistlane {
// Entity contract prototype: the first phase of the entity-contract chain
// (docs/entity-contract.md) — parse the handwritten XML subset and enforce
// its semantic rules. It is a definition/build-time library only: no runtime
// wiring, no code generation, no sproto emission, no persistence. Wire
// messages stay binary; this header never sees one.
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
        }
        return out.str();
    }
    std::uint64_t digest() const {
        const auto text = describe();
        std::uint64_t hash = 14695981039346656037ull;
        for (unsigned char ch : text) { hash ^= ch; hash *= 1099511628211ull; }
        return hash;
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
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
                || ch == '_' || ch == '-' || ch == '.';
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
            if (of == "ARRAY" || !basic_type(of) && !structs_.count(of))
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
                const auto arg = read_field(child, what+" arg");
                if (const auto seen = arg_ids.find(arg.id); seen != arg_ids.end())
                    throw ContractError(what+": argument id "+std::to_string(arg.id)+" declared twice");
                arg_ids.emplace(arg.id, arg.name);
                (child.name == "Arg" ? m.args : m.returns).push_back(std::move(arg));
            } else if (child.name == "Returns" && server_side) {
                returns_seen = true;
                for (const auto& field : child.children) {
                    const auto ret = read_field(field, what+" return");
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
        bool properties_seen = false, server_seen = false, client_seen = false;
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
            } else throw ContractError(what+": unexpected element <"+child.name+"> inside an Entity");
        }
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
}
