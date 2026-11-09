// SPDX-License-Identifier: Apache-2.0
#include "test_common.hpp"
namespace mistlane::test {
void core_contracts() {

    TokenBucket bucket(2, 0.000001);
    require(bucket.consume() && bucket.consume() && !bucket.consume(), "token budget is not enforced");
    must_fail([] { TokenBucket invalid(0, 1); });
    auto context = cross_context("MLC1 1 realm group room");
    equal(context.room, "room");
    require(context.character.empty() && !context.generation && context.return_room.empty(),
        "anonymous join gained participant fields");
    must_fail([] { (void)cross_context("MLC1 0 realm group room"); });
    must_fail([] { (void)cross_context("MLC1 1 realm group room extra"); });
    // A participant declares character, hosting generation and return route
    // together; every partial declaration is refused.
    auto participant = cross_context("MLC1 1 realm group room ch hero-7 gen 12 ret home arena r1");
    equal(participant.character, "hero-7");
    require(participant.generation == 12, "hosting generation lost");
    equal(participant.return_realm+"/"+participant.return_group+"/"+participant.return_room, "home/arena/r1");
    require(!participant.ticket, "a join without a ticket gained one");
    // The optional entry ticket rides the last trailing group as hex MLT1.
    ApiSecret ticket_secret{};
    for (std::size_t i = 0; i < ticket_secret.size(); ++i) ticket_secret[i] = static_cast<unsigned char>(i*7+1);
    EntryTicket ticket;
    ticket.character = {"home", "hero-7"};
    ticket.instance = "gs-a";
    ticket.generation = 12;
    ticket.expires = 1'760'000'000;
    ticket.nonce = "0123456789abcdef0123456789abcdef";
    ticket.signature = entry_ticket_sign(ticket, ticket_secret);
    const auto hex = entry_ticket_hex(ticket);
    const auto with_ticket = "MLC1 1 realm group room ch hero-7 gen 12 ret home arena r1 tk " + hex;
    const auto entered = cross_context(with_ticket);
    require(entered.ticket && *entered.ticket == ticket, "the ticket did not round-trip the join");
    // Decoding refuses anything off the wire, so a forged tail is an INVALID
    // join instead of a partly-valid participant.
    for (const auto& forged : std::vector<std::string>{
            hex.substr(0, hex.size()-2),
            hex.substr(1),
            "0"+hex,
            hex.substr(0, hex.size()-4)+"ZZZZ",
            std::string(512, '0'),
            "DEADBEEF"})
        must_fail([&] { (void)cross_context("MLC1 1 realm group room ch hero-7 gen 12 ret home arena r1 tk "+forged); });
    // The ticket is bound to the same identity and generation it names.
    for (const auto& mismatch : std::vector<std::string>{
            "MLC1 1 realm group room ch other gen 12 ret home arena r1",
            "MLC1 1 realm group room ch hero-7 gen 13 ret home arena r1"})
        must_fail([&] { (void)cross_context(mismatch+" tk "+hex); });
    must_fail([&] { (void)cross_context("MLC1 1 realm group room tk "+hex); });
    must_fail([&] { (void)cross_context("MLC1 1 realm group room ch hero-7 gen 12 ret home arena r1 tke "+hex); });
    must_fail([&] { (void)cross_context("MLC1 1 realm group room ch hero-7 gen 12 ret home arena r1 tk "+std::string(420, 'a')); });
    // Oversized tails are refused at the caps, not just by the decoder.
    must_fail([&] { (void)cross_context("MLC1 1 realm group room ch hero-7 gen 12 ret home arena r1 tk "+std::string(1200, 'a')); });
    must_fail([&] { (void)cross_context("MLC1 1 realm group room "+std::string(2000, 'x')); });
    for (const auto& partial : std::vector<std::string>{
        "MLC1 1 realm group room ch hero",
        "MLC1 1 realm group room ch hero gen 12",
        "MLC1 1 realm group room ch hero gen 12 ret home arena",
        "MLC1 1 realm group room gen 12",
        "MLC1 1 realm group room ret home arena r1",
        "MLC1 1 realm group room ch hero gen 0 ret home arena r1",
        "MLC1 1 realm group room ch hero gen 12 ret home arena r1 extra",
        "MLC1 1 realm group room ch her!o gen 12 ret home arena r1"})
        must_fail([&] { (void)cross_context(partial); });
    // MLD1 record grammar: load and submit both parse, and every partial or
    // out-of-shape declaration is refused.
    auto load = parse_record("MLD1 LOAD home hero-7");
    equal(load.realm+"/"+load.character, "home/hero-7");
    require(load.type == RecordRequest::Type::Load, "record load mislabeled");
    auto submit = parse_record("MLD1 SUBMIT home hero-7 gen 12 seq 3 {\"hp\":5}");
    require(submit.type == RecordRequest::Type::Submit && submit.generation == 12
            && submit.sequence == 3, "record submit header lost");
    equal(submit.payload, "{\"hp\":5}");
    equal(parse_record("MLD1 SUBMIT home hero-7 gen 12 seq 3").payload, "");
    for (const auto& broken : std::vector<std::string>{
        "MLD1", "MLD1 LOAD home", "MLD1 LOAD home hero extra",
        "MLD1 SUBMIT home hero gen 12", "MLD1 SUBMIT home hero gen 12 seq",
        "MLD1 SUBMIT home hero gen 0 seq 1", "MLD1 SUBMIT home hero gen 12 seq 0",
        "MLD1 SUBMIT home hero gen x seq 1", "MLD1 SUBMIT home hero gen 12 seq 99999999999",
        "MLD1 SUBMIT ho/me hero gen 12 seq 1", "MLD1 SUBMIT home her!o gen 12 seq 1",
        std::string("MLD1 SUBMIT home hero gen 12 seq 1 ")+std::string(1024*1024+1, 'x')})
        must_fail([&] { (void)parse_record(broken); });
    // The home side owns the fence and the sequence discipline.
    RecordStore records;
    std::string_view result;
    equal(records.handle("MLD1 LOAD home hero", result), "MLD1 EMPTY");
    require(result == "empty", "record load mislabeled as " + std::string(result));
    equal(records.handle("MLD1 SUBMIT home hero gen 5 seq 1 hp=10", result), "MLD1 RECEIVED gen 5 seq 1");
    require(result == "accepted", "first submit was not accepted");
    equal(records.handle("MLD1 LOAD home hero", result), "MLD1 STATE gen 5 seq 1 hp=10");
    require(result == "loaded", "record load after submit mislabeled");
    // A resend dedups to a second receipt and never overwrites the record.
    equal(records.handle("MLD1 SUBMIT home hero gen 5 seq 1 hp=99", result), "MLD1 RECEIVED gen 5 seq 1");
    require(result == "duplicate", "resend was not deduplicated");
    equal(records.handle("MLD1 LOAD home hero", result), "MLD1 STATE gen 5 seq 1 hp=10");
    equal(records.handle("MLD1 SUBMIT home hero gen 5 seq 3 gap", result), "MLD1 ERR SEQ seq 2");
    require(result == "seq", "sequence gap mislabeled");
    equal(records.handle("MLD1 SUBMIT home hero gen 4 seq 2 late", result), "MLD1 ERR STALE gen 5");
    require(result == "stale", "generation fence mislabeled");
    equal(records.handle("MLD1 SUBMIT home hero gen 6 seq 2 taken", result), "MLD1 RECEIVED gen 6 seq 2");
    equal(records.handle("MLD1 SUBMIT home hero gen 7 seq 9 jump", result), "MLD1 ERR SEQ seq 3");
    equal(records.handle("MLD1 SUBMIT home hero gen 7 seq 3", result), "MLD1 RECEIVED gen 7 seq 3");
    equal(records.handle("MLD1 LOAD home hero", result), "MLD1 STATE gen 7 seq 3");
    // Records are independent per character, and a fresh record starts at one.
    equal(records.handle("MLD1 SUBMIT home other gen 1 seq 2", result), "MLD1 ERR SEQ seq 1");
    // Realm is part of both the fence and sequence key: the same character
    // id in another home domain starts independently at sequence one.
    equal(records.handle("MLD1 SUBMIT away hero gen 1 seq 1 hp=3", result), "MLD1 RECEIVED gen 1 seq 1");
    equal(records.handle("MLD1 LOAD home hero", result), "MLD1 STATE gen 7 seq 3");
    equal(records.handle("MLD1 LOAD away hero", result), "MLD1 STATE gen 1 seq 1 hp=3");
    equal(records.handle("MLD1 NOPE", result), "MLD1 ERR INVALID");
    require(result == "invalid", "malformed frame mislabeled");
    // Durable mode: acceptance echoes the committed ACK, save failures refuse
    // the write, and a fresh store rebuilt from the backend sees the same state.
    struct FakeRecordBackend : RecordBackend {
        std::vector<RecordSnapshot> rows;
        bool fail = false;
        void save_record(const RecordSnapshot& row) override {
            if (fail) throw std::runtime_error("storage down");
            std::vector<RecordSnapshot> kept;
            for (auto& old : rows)
                if (old.realm != row.realm || old.character != row.character) kept.push_back(std::move(old));
            rows = std::move(kept);
            rows.push_back(row);
        }
        std::vector<RecordSnapshot> restore_records() const override { return rows; }
    };
    FakeRecordBackend backend;
    RecordStore durable;
    durable.attach_backend(&backend);
    require(durable.durable() && !records.durable(), "durability switch is wrong");
    equal(durable.handle("MLD1 SUBMIT home hero gen 5 seq 1 hp=10", result),
        "MLD1 ACK durable home hero gen 5 seq 1");
    require(result == "accepted", "durable submit was not accepted");
    equal(durable.handle("MLD1 SUBMIT home hero gen 5 seq 1 hp=99", result),
        "MLD1 ACK durable home hero gen 5 seq 1");
    require(result == "duplicate", "durable resend was not deduplicated");
    equal(durable.handle("MLD1 LOAD home hero", result), "MLD1 STATE gen 5 seq 1 hp=10");
    RecordStore reloaded;
    reloaded.attach_backend(&backend);
    equal(reloaded.handle("MLD1 LOAD home hero", result), "MLD1 STATE gen 5 seq 1 hp=10");
    equal(reloaded.handle("MLD1 SUBMIT home hero gen 6 seq 2 hp=20", result),
        "MLD1 ACK durable home hero gen 6 seq 2");
    backend.fail = true;
    equal(reloaded.handle("MLD1 SUBMIT home hero gen 6 seq 3 fail", result), "MLD1 ERR STORAGE");
    require(result == "storage", "storage failure mislabeled");
    equal(reloaded.handle("MLD1 LOAD home hero", result), "MLD1 STATE gen 6 seq 2 hp=20");
    backend.fail = false;
    equal(reloaded.handle("MLD1 SUBMIT home hero gen 6 seq 3 committed", result),
        "MLD1 ACK durable home hero gen 6 seq 3");
    // Entity contract prototype: the design page's example parses, resolves
    // and digests; every listed semantic rule refuses its violation.
    const auto document = R"(<Contracts>
  <Types>
    <Item><itemId id="0" type="UINT32"/><count id="1" type="UINT32" default="1"/></Item>
    <Inventory><capacity id="0" type="UINT16" default="20"/><items id="1" type="ARRAY" of="Item"/></Inventory>
  </Types>
  <Entity name="Avatar" id="1">
    <Properties>
      <level id="1" type="UINT16" default="1" flags="OWN_CLIENT" persistent="true"/>
      <hp id="2" type="UINT32" default="100" flags="ALL_CLIENTS"/>
      <gold id="3" type="INT64" default="-5" flags="OWN_CLIENT" persistent="true"/>
      <inventory id="4" type="Inventory" flags="OWN_CLIENT" persistent="true"/>
    </Properties>
    <ServerMethods>
      <Method name="attack" id="1" mode="notify" caller="owner">
        <Arg name="target" id="0" type="EntityRef"/>
      </Method>
      <Method name="getProfile" id="2" mode="request" caller="server">
        <Returns><Field name="level" id="0" type="UINT16"/></Returns>
      </Method>
    </ServerMethods>
    <ClientMethods>
      <Method name="onReward" id="1" mode="notify" audience="owner">
        <Arg name="itemId" id="0" type="UINT32"/><Arg name="count" id="1" type="UINT32"/>
      </Method>
    </ClientMethods>
  </Entity>
</Contracts>)";
    const auto document_with = [&](std::string_view from, std::string_view to) {
        auto text = std::string(document);
        const auto at = text.find(from);
        require(at != std::string::npos, std::string("test edit missed its anchor: ")+std::string(from));
        text.replace(at, from.size(), to);
        return text;
    };
    EntityContract contract(document);
    require(contract.structs().size() == 2 && contract.entities().size() == 1,
            "contract model lost a declaration");
    const auto& avatar = contract.entities().front();
    equal(avatar.properties[3].type, "Inventory");
    require(avatar.properties[3].persistent, "persistent attribute lost");
    require(avatar.properties[1].flags == "ALL_CLIENTS" && !avatar.properties[1].persistent,
            "explicit flags lost");
    require(avatar.properties[2].default_value == "-5", "negative INT64 default lost");
    require(avatar.server_methods[1].returns.size() == 1, "request returns lost");
    require(contract.structs().at("Item").front().id == 0, "struct field id lost");
    // The dump is the digest input and keeps declaration order and explicit ids.
    require(contract.describe().find(" prop {1 level UINT16 default 1} OWN_CLIENT persistent") != std::string::npos &&
            contract.describe().find(" method server 2 getProfile request server") != std::string::npos,
            "canonical dump lost the resolved model: "+contract.describe());
    require(contract.digest() != 0, "digest is computed");
    require(EntityContract(document).digest() == contract.digest(), "digest is not reproducible");
    for (const auto& broken : std::vector<std::string>{
        // ids: explicit, unique, never recomputed
        document_with("<hp id=\"2\" type=\"UINT32\" default=\"100\" flags=\"ALL_CLIENTS\"/>",
                      "<hp id=\"1\" type=\"UINT32\" default=\"100\" flags=\"ALL_CLIENTS\"/>"),
        // closed vocabularies
        document_with("flags=\"ALL_CLIENTS\"", "flags=\"EVERYONE\""),
        document_with("type=\"UINT32\" default=\"100\" flags", "type=\"UINT8\" default=\"100\" flags"),
        document_with("caller=\"server\"", "caller=\"anyone\""),
        document_with("mode=\"request\" caller=\"server\"",
                      "mode=\"fire-and-forget\" caller=\"server\""),
        // typed defaults
        document_with("default=\"100\" flags", "default=\"4294967296\" flags"),
        document_with("default=\"-5\"", "default=\"false\""),
        // arrays and struct references
        document_with("type=\"ARRAY\" of=\"Item\"", "type=\"ARRAY\""),
        document_with("type=\"ARRAY\" of=\"Item\"", "type=\"ARRAY\" of=\"Warehouse\""),
        // methods
        document_with("<Method name=\"getProfile\" id=\"2\" mode=\"request\" caller=\"server\">\n"
                      "        <Returns><Field name=\"level\" id=\"0\" type=\"UINT16\"/></Returns>\n"
                      "      </Method>",
                      "<Method name=\"getProfile\" id=\"2\" mode=\"request\" caller=\"server\"/>"),
        document_with("mode=\"notify\" caller=\"owner\">\n"
                      "        <Arg name=\"target\" id=\"0\" type=\"EntityRef\"/>\n"
                      "      </Method>",
                      "mode=\"notify\" caller=\"owner\">\n"
                      "        <Returns><Field name=\"x\" id=\"0\" type=\"UINT16\"/></Returns>\n"
                      "      </Method>"),
        document_with("audience=\"owner\"", "audience=\"server\""),
        "<Entity name=\"Avatar\" id=\"1\"><ServerMethods>"
        "<Method name=\"attack\" id=\"1\" mode=\"request\" caller=\"owner\"/></ServerMethods></Entity>",
        // xml subset: external entities and CDATA are refused outright
        std::string("<!DOCTYPE Contracts [<!ENTITY xxe \"evil\">]><Contracts></Contracts>"),
        std::string("<Contracts><Entity name=\"A\" id=\"1\"/><![CDATA[raw]]></Contracts>")})
        must_fail([&] { (void)EntityContract(broken); });
    // Duplicate member names across groups are refused, even across method
    // sides; the same method id on both sides is fine.
    EntityContract mirrored(std::string(
        "<Contracts><Entity name=\"A\" id=\"1\">"
        "<Properties><x id=\"1\" type=\"UINT32\"/></Properties>"
        "<ServerMethods><Method name=\"ping\" id=\"1\" mode=\"notify\" caller=\"server\"/></ServerMethods>"
        "<ClientMethods><Method name=\"pong\" id=\"1\" mode=\"notify\" audience=\"owner\"/></ClientMethods>"
        "</Entity></Contracts>"));
    require(mirrored.entities().front().client_methods.front().id == 1, "side-local method id lost");
    must_fail([&] { (void)EntityContract(std::string(
        "<Contracts><Entity name=\"A\" id=\"1\">"
        "<ServerMethods><Method name=\"ping\" id=\"1\" mode=\"notify\" caller=\"server\"/></ServerMethods>"
        "<ClientMethods><Method name=\"ping\" id=\"1\" mode=\"notify\" audience=\"owner\"/></ClientMethods>"
        "</Entity></Contracts>")); });
    must_fail([&] { (void)EntityContract(std::string(
        "<Contracts><Entity name=\"A\" id=\"1\">"
        "<Properties><x id=\"1\" type=\"UINT32\"/></Properties>"
        "<ServerMethods><Method name=\"x\" id=\"1\" mode=\"notify\" caller=\"server\"/></ServerMethods>"
        "</Entity></Contracts>")); });
    // Recursive struct chains are refused; acyclic chains resolve.
    must_fail([&] { (void)EntityContract(std::string(
        "<Contracts><Types>"
        "<A><next id=\"0\" type=\"ARRAY\" of=\"B\"/></A>"
        "<B><prev id=\"0\" type=\"A\"/></B>"
        "</Types></Contracts>")); });
    EntityContract chain(std::string(
        "<Contracts><Types>"
        "<A><b id=\"0\" type=\"B\"/></A>"
        "<B><leaf id=\"0\" type=\"UINT16\"/></B>"
        "</Types><Entity name=\"E\" id=\"1\">"
        "<Properties><root id=\"1\" type=\"A\"/></Properties>"
        "</Entity></Contracts>"));
    require(chain.digest() != 0, "digest is computed");
    require(chain.describe().find("struct A") != std::string::npos &&
            chain.describe().find(" prop {1 root A} SERVER_ONLY") != std::string::npos,
            "canonical dump lost the resolved model: "+chain.describe());
    // Reserved ids keep retired tags forever, and the schema/lock emit them.
    EntityContract locked(document_with("</Entity>", "<Reserved>"
            "<Property id=\"5\"/><ServerMethod id=\"9\"/><ClientMethod id=\"7\"/></Reserved></Entity>"));
    const auto& retained = locked.entities().front();
    require(retained.reserved_properties.size() == 1 && retained.reserved_properties[0] == 5 &&
            retained.reserved_server_methods.size() == 1 && retained.reserved_server_methods[0] == 9 &&
            retained.reserved_client_methods.size() == 1 && retained.reserved_client_methods[0] == 7,
            "reserved ids lost: "+locked.describe());
    require(locked.digest() != contract.digest(), "reserving an id does not change the model");
    for (const auto& broken : std::vector<std::string>{
        document_with("</Entity>", "<Reserved><Property id=\"1\"/></Reserved></Entity>"),
        document_with("</Entity>", "<Reserved><Property id=\"5\"/><Property id=\"5\"/></Reserved></Entity>"),
        document_with("</Entity>", "<Reserved><Property id=\"5\" note=\"retired\"/></Reserved></Entity>"),
        document_with("</Entity>", "<Reserved><Property/></Reserved></Entity>"),
        document_with("</Entity>", "<Reserved><Thing id=\"5\"/></Reserved></Entity>"),
        document_with("</Entity>", "<Reserved><Property id=\"7\"/></Reserved></Entity>"
        "<Reserved><Property id=\"5\"/></Reserved></Entity>")})
        must_fail([&] { (void)EntityContract(broken); });
    // The generated schema is well-formed XML and pins each member's id and type.
    const auto schema = locked.xsd();
    require(EntityContract::well_formed(schema), "generated schema is not well-formed: "+schema);
    require(EntityContract(document_with("</Entity>", "<Reserved><Property id=\"5\"/>"
            "<ServerMethod id=\"9\"/><ClientMethod id=\"7\"/></Reserved></Entity>")).xsd() == schema,
            "schema is not reproducible");
    const auto block = [&](const std::string& needle) {
        const auto at = schema.find(needle);
        require(at != std::string::npos, std::string("schema is missing ")+needle);
        const auto end = schema.find("</xs:element>", at);
        require(end != std::string::npos, "schema block is unterminated");
        return schema.substr(at, end-at);
    };
    const auto hp = block("<xs:element name=\"hp\" ");
    require(hp.find("<xs:enumeration value=\"2\"/>") != std::string::npos &&
            hp.find("<xs:enumeration value=\"UINT32\"/>") != std::string::npos &&
            hp.find("type=\"ReplicationFlags\"") != std::string::npos,
            "the schema does not pin a property's id, type and flags: "+hp);
    require(schema.find("<xs:element name=\"TypesSource\"") < schema.find("<xs:element name=\"Types\"") &&
            schema.find("<xs:element name=\"Types\"") < schema.find("<xs:element name=\"Entity\"") &&
            schema.find("<xs:complexType><xs:sequence>") != std::string::npos,
            "the schema does not restate the shared document skeleton");
    require(schema.find("<xs:attribute name=\"path\" use=\"required\"/>") != std::string::npos &&
            schema.find("<xs:element name=\"Types\"") != std::string::npos,
            "the schema lost the shared type-source rules");
    require(block("<xs:element name=\"Property\" ").find("<xs:enumeration value=\"5\"/>") !=
            std::string::npos, "the schema does not pin a reserved property id");
    require(block("<xs:element name=\"ClientMethod\" ").find("<xs:enumeration value=\"7\"/>") !=
            std::string::npos, "the schema does not pin a reserved client method id");
    require(block("<xs:element name=\"Field\" ").find("<xs:enumeration value=\"UINT16\"/>") !=
            std::string::npos, "the schema lost the request's typed return");
    const auto attr = [&](const char* key, const std::string& value) {
        return std::string("<xs:attribute name=\"")+key+"\" use=\"required\"><xs:simpleType>"
            "<xs:restriction base=\"xs:string\"><xs:enumeration value=\""+value+
            "\"/></xs:restriction></xs:simpleType></xs:attribute>";
    };
    require(schema.find(attr("id", "2")) != std::string::npos &&
            schema.find(attr("type", "UINT32")) != std::string::npos,
            "the generated schema pins a member id and declared type");
    require(schema.find(attr("mode", "request")) != std::string::npos &&
            schema.find(attr("mode", "notify")) != std::string::npos,
            "the schema does not pin the declared modes");
    require(schema.find(attr("caller", "owner")) != std::string::npos &&
            schema.find(attr("caller", "server")) != std::string::npos &&
            schema.find(attr("audience", "owner")) != std::string::npos,
            "the schema mixed up caller and audience");
    require(schema.find(attr("name", "attack")) != std::string::npos &&
            schema.find(attr("id", "1")) != std::string::npos,
            "the schema does not pin member names and ids");
    require(schema.find("<xs:attribute name=\"id\"") != std::string::npos &&
            schema.find("<xs:attribute name=\"type\"") != std::string::npos,
            "the schema attribute order is malformed");
    for (const auto* tag : {"TypesSource", "Types", "Entity", "Properties",
                            "ServerMethods", "ClientMethods", "Reserved"})
        require(schema.find("<xs:element name=\""+std::string(tag)+"\"") !=
                std::string::npos &&
                schema.find("<xs:element name=\""+std::string(tag)+"\"") ==
                schema.rfind("<xs:element name=\""+std::string(tag)+"\""),
                "the schema emitted duplicate shared element "+std::string(tag));
    require(schema.find("<xs:enumeration value=\"Inventory\"/>") != std::string::npos &&
            schema.find("<xs:element name=\"Inventory\"") != std::string::npos &&
            schema.find("name=\"capacity\"") != std::string::npos,
            "the schema lost the declared structs");
    for (const auto* reserved_name : {"Types", "Entity", "Properties",
                                      "ServerMethods", "ClientMethods", "Reserved",
                                      "Method", "Returns"}) {
        const auto text = std::string("<Contracts><Types><")+reserved_name+"></"+reserved_name+"></Types></Contracts>";
        must_fail([&] { (void)EntityContract(text); });
    }
    require(contract.xsd().find("name=\"Reserved\"") == std::string::npos,
            "an empty Reserved group must not appear in the schema");
    require(!EntityContract::well_formed("<Contracts><A><B/></Contracts>"),
            "well-formedness accepted a mismatched close tag");
    require(!EntityContract::well_formed("<Contracts/></Contracts>"),
            "well-formedness accepted trailing content");
    // The release lock keeps every assignment a published version made.
    const auto lock_text = locked.lock();
    require(lock_text.find("# mistlane entity contract lock\n# digest ") != std::string::npos &&
            lock_text.find("struct Item") != std::string::npos &&
            lock_text.find("entity Avatar 1") != std::string::npos &&
            lock_text.find(" prop Avatar 3 gold INT64") != std::string::npos &&
            lock_text.find(" method Avatar server 2 getProfile") != std::string::npos &&
            lock_text.find(" method Avatar client 1 onReward") != std::string::npos &&
            lock_text.find(" reserved Avatar property 5") != std::string::npos &&
            lock_text.find(" reserved Avatar server 9") != std::string::npos &&
            lock_text.find(" reserved Avatar client 7") != std::string::npos,
            "release lock lost assignments: "+lock_text);
    require(EntityContract(document_with("</Entity>", "<Reserved><Property id=\"5\"/>"
            "<ServerMethod id=\"9\"/><ClientMethod id=\"7\"/></Reserved></Entity>")).lock() == lock_text,
            "release lock is not reproducible");
    // Compatibility model: the lock text parses back into exactly the entries
    // the live model flattens to, so the publication gate compares one model
    // shape on both sides instead of a second ad-hoc lock reader.
    const auto baseline = locked.entries();
    require(parse_lock(lock_text) == baseline, "lock text and live model disagree on entries");
    // A lock line the parser cannot place is refused outright, never skipped.
    must_fail([&] { (void)parse_lock(lock_text+" widget Avatar x\n"); });
    must_fail([&] { (void)parse_lock("struct Item {0 itemId UINT32 extra}\n"); });
    must_fail([&] { (void)parse_lock(lock_text+" prop Avatar 2 hp UINT32\n"); });
    require(compat_diff(baseline, baseline).compatible(), "a lock is incompatible with itself");
    const auto compat_source = document_with("</Entity>", "<Reserved><Property id=\"5\"/>"
            "<ServerMethod id=\"9\"/><ClientMethod id=\"7\"/></Reserved></Entity>");
    const auto compat_edit = [&](std::string_view from, std::string_view to) {
        auto text = compat_source;
        const auto at = text.find(from);
        require(at != std::string::npos,
                std::string("compat edit missed its anchor: ")+std::string(from));
        text.replace(at, from.size(), to);
        return EntityContract(text).entries();
    };
    const auto joined_lines = [](const std::vector<std::string>& lines) {
        std::string text;
        for (const auto& line : lines) text += line + "; ";
        return text;
    };
    const auto breaks_with = [&](const std::vector<ContractEntry>& prev, const std::vector<ContractEntry>& curr,
                                 std::string_view needle) {
        const auto diff = compat_diff(prev, curr);
        const auto reports = joined_lines(diff.breaking);
        require(!diff.compatible() && reports.find(needle) != std::string::npos,
                std::string("compat gate missed ")+std::string(needle)+": "+reports);
    };
    breaks_with(baseline, compat_edit("<gold id=\"3\" type=\"INT64\"", "<gold id=\"6\" type=\"INT64\""), "3 -> 6");
    breaks_with(baseline, compat_edit("<gold id=\"3\" type=\"INT64\"", "<gold id=\"3\" type=\"STRING\""), "INT64");
    breaks_with(baseline, compat_edit("caller=\"owner\"", "caller=\"server\""), "notify owner -> notify server");
    breaks_with(baseline, compat_edit("<gold id=\"3\" type=\"INT64\" default=\"-5\" flags=\"OWN_CLIENT\" persistent=\"true\"/>\n      ",
            ""), "gold");
    breaks_with(baseline, EntityContract(document).entries(), "dropped");
    // Retired ids never come back as live meanings on any id space. The XML
    // parser refuses the collision outright, so the attempt lives in lock text
    // exactly where a published lock could be hand-edited into it.
    const auto lock_edit = [&](std::string_view from, std::string_view to) {
        auto text = lock_text;
        const auto at = text.find(from);
        require(at != std::string::npos,
                std::string("lock edit missed its anchor: ")+std::string(from));
        text.replace(at, from.size(), to);
        return parse_lock(text);
    };
    breaks_with(baseline, lock_edit(" reserved Avatar property 5\n",
            " reserved Avatar property 5\n prop Avatar 5 magic UINT32\n"), "reused");
    breaks_with(baseline, lock_edit(" reserved Avatar server 9\n",
            " reserved Avatar server 9\n method Avatar server 9 zap notify server\n"), "reused");
    breaks_with(baseline, lock_edit(" reserved Avatar client 7\n",
            " reserved Avatar client 7\n method Avatar client 7 zap notify owner\n"), "reused");
    breaks_with(baseline, lock_edit(" arg Avatar server 1 0 target EntityRef\n",
            " arg Avatar server 1 0 target ARRAY Item\n"), "EntityRef");
    const auto added = compat_diff(baseline, compat_edit(
            "<gold id=\"3\" type=\"INT64\" default=\"-5\" flags=\"OWN_CLIENT\" persistent=\"true\"/>",
            "<gold id=\"3\" type=\"INT64\" default=\"-5\" flags=\"OWN_CLIENT\" persistent=\"true\"/>"
            "<nickname id=\"6\" type=\"STRING\"/>"));
    require(added.compatible() && !added.additive.empty() &&
            joined_lines(added.additive).find("nickname") != std::string::npos,
            "a new property did not land as an additive change");
    CrossAdmission admission(1, 1, 1, 1);
    int a = 0, b = 0;
    require(admission.acquire(&a, context, 0).empty(), "first admission failed");
    equal(admission.acquire(&b, context, 0), "ROOM_FULL");
    admission.release(&a);
    require(admission.acquire(&b, context, 0).empty(), "admission did not recover on release");
    Context c;
    for (auto args : std::vector<std::vector<std::string>>{
        {"--role", "invalid"}, {"--listen", "127.0.0.1:65536"}, {"--role", "gameServer"},
        {"--role", "gateway"}, {"--max-connections", "0"}, {"--role", "gameServer", "--transport", "wss"}}) {
        args.insert(args.begin(), self().string());
        require(c.command(args) != 0, "invalid CLI options accepted");
    }
}
void role_names() {
    Context c;
    write(c.path("echo.lua"), correlate_lua);
    for (const auto& old : {"worker", "router", "cross", "shard"}) {
        auto& child = c.spawn({self().string(), "--role", old, "--script", c.path("echo.lua").string(),
                              "--upstream", endpoint(port())});
        require(child.wait() != 0, "removed role accepted");
        require(child.output().find("unknown role") != std::string::npos, "removed role was not rejected by name");
    }
    // login and data run as script services: a script-less launch is refused,
    // and with a script they serve framed sessions under their own role name.
    for (const auto& role : {"login", "data"}) {
        auto& child = c.spawn({self().string(), "--role", role});
        require(child.wait() != 0 && child.output().find("--script") != std::string::npos,
                std::string(role) + " started without a script");
        write(c.path(std::string(role)+".lua"), "return {message=function(p)return p end}");
        const auto listen = port(), metrics = port();
        c.node(role, listen, {"--script", c.path(std::string(role)+".lua").string(),
                              "--metrics-listen", endpoint(metrics)});
        equal(tcp(listen)->request("ok"), "ok");
        const auto samples = http("http://"+endpoint(metrics)+"/metrics");
        require(samples.status == 200 && samples.body.find(std::string("mistlane_ready{role=\"")+role+"\"} 1") != std::string::npos,
            std::string(role) + " readiness gauge missing");
    }
    write(c.path("api.lua"), http_lua);
    const auto api_port = port();
    auto& api = c.node("apiServer", api_port, {"--script", c.path("api.lua").string()});
    require(api.output().find("ready apiServer ") != std::string::npos, "apiServer role missing");
    // Default apiServer transport is HTTP. Missing HTTP callback has a controlled response.
    require(http("http://"+endpoint(api_port)+"/missing").status != 0, "apiServer HTTP default failed");
}

// Full transport binding: a real gameServer loads a contract (.def) plus a
// script with an entity implementation table, and MLE1 frames travel the
// u32-BE framed socket exactly like any other message. The trusted connection
// is the only caller identity: an MLS1-identified peer is the authorized
// internal service the contract's caller="server" permits, while a plain
// client leg is FORBIDDEN a server method — owner attribution is not built
// yet, so no client can invoke anything, by design not by omission.
}
