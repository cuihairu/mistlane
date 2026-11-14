import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { ContractParseError, contractRows, parseContractDef } from "../src/contract.js";

// The shipped example, not a synthetic copy: the panel must render exactly
// what the engine loads.
const BUSINESS_DEF = readFileSync(
  new URL("../../../examples/contracts/business.def", import.meta.url),
  "utf8",
);

const SHAPE_DEF = [
  "<Contracts>",
  "  <TypesSource path=\"types.xml\"/>",
  "  <Entity name=\"Avatar\" id=\"1\">",
  "    <Properties>",
  "      <level id=\"1\" type=\"UINT16\" flags=\"OWN_CLIENT\"/>",
  "      <inventory id=\"2\" type=\"Inventory\"/>",
  "      <tags id=\"5\" type=\"ARRAY\" of=\"UINT32\"/>",
  "    </Properties>",
  "    <ServerMethods>",
  "      <Method name=\"attack\" id=\"1\" mode=\"notify\" caller=\"owner\">",
  "        <Arg name=\"target\" id=\"0\" type=\"EntityRef\"/>",
  "      </Method>",
  "      <Method name=\"getProfile\" id=\"2\" mode=\"request\" caller=\"server\">",
  "        <Arg name=\"account\" id=\"0\" type=\"UINT32\"/>",
  "        <Returns><Field name=\"level\" id=\"1\" type=\"UINT16\"/></Returns>",
  "      </Method>",
  "    </ServerMethods>",
  "    <ClientMethods>",
  "      <Method name=\"onReward\" id=\"1\" mode=\"notify\" audience=\"owner\">",
  "        <Arg name=\"itemId\" id=\"0\" type=\"UINT32\"/>",
  "      </Method>",
  "    </ClientMethods>",
  "  </Entity>",
  "</Contracts>",
].join("\n");

describe("contract .def parser", () => {
  it("parses the shipped business.def example", () => {
    const doc = parseContractDef(BUSINESS_DEF);
    expect(doc.entities).toHaveLength(1);
    const player = doc.entities[0]!;
    expect(player.name).toBe("Player");
    expect(player.id).toBe(2);
    expect(player.serverMethods.map((method) => method.name)).toEqual([
      "login", "joinRoom", "move", "heartbeat", "leaveRoom",
    ]);
    expect(player.serverMethods[0]!.args[0]!.type).toBe("STRING");
    expect(player.serverMethods[0]!.returns[0]!.name).toBe("ok");
    expect(doc.typesSource).toBeUndefined();
  });

  it("records source lines for click-to-open anchors", () => {
    const doc = parseContractDef(BUSINESS_DEF);
    // Entity tag is on line 11 of examples/contracts/business.def.
    expect(doc.entities[0]!.line).toBe(11);
    expect(doc.entities[0]!.serverMethods[0]!.line).toBe(13);
  });

  it("treats the property tag as the property name inside <Properties>", () => {
    const doc = parseContractDef(SHAPE_DEF);
    const avatar = doc.entities[0]!;
    expect(avatar.properties.map((property) => property.name)).toEqual(["level", "inventory", "tags"]);
    expect(avatar.properties[0]!.flags).toBe("OWN_CLIENT");
    expect(avatar.properties[2]!.of).toBe("UINT32");
  });

  it("separates server and client methods with their vocabularies", () => {
    const doc = parseContractDef(SHAPE_DEF);
    const avatar = doc.entities[0]!;
    expect(avatar.serverMethods.map((method) => `${method.name}/${method.mode}/${method.caller}`))
      .toEqual(["attack/notify/owner", "getProfile/request/server"]);
    expect(avatar.clientMethods[0]!.audience).toBe("owner");
    expect(avatar.clientMethods[0]!.args[0]!.type).toBe("UINT32");
    expect(avatar.serverMethods[1]!.returns[0]!.type).toBe("UINT16");
  });

  it("keeps the TypesSource reference with its line", () => {
    const doc = parseContractDef(SHAPE_DEF);
    expect(doc.typesSource!.path).toBe("types.xml");
    expect(doc.typesSource!.line).toBe(2);
  });

  it("flattens into IDE table rows", () => {
    const rows = contractRows(parseContractDef(SHAPE_DEF));
    expect(rows).toHaveLength(3);
    expect(rows[0]).toMatchObject({
      entity: "Avatar", side: "server", method: "attack", mode: "notify", caller: "owner",
      args: "target:EntityRef", returns: "-",
    });
    expect(rows[2]).toMatchObject({ side: "client", method: "onReward", caller: "owner" });
  });

  it("rejects misplaced elements with the offending line", () => {
    expect(() => parseContractDef("<Method name=\"x\" id=\"1\"/>")).toThrow(ContractParseError);
    expect(() => parseContractDef("<Contracts><Method name=\"x\" id=\"1\"/></Contracts>"))
      .toThrow(/line 1/);
    expect(() => parseContractDef("<Contracts><Entity name=\"E\" id=\"1\"><Method name=\"m\" id=\"1\"/></Entity></Contracts>"))
      .toThrow(/Method belongs inside/);
    expect(() => parseContractDef("<Contracts><Entity name=\"E\"/></Contracts>")).toThrow(/integer id/);
  });
});
