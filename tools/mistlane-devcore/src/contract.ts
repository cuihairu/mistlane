// Entity-contract .def reader for the IDE contract face. The document is XML
// with one twist that rules out a generic tag-name schema: inside
// <Properties> the property name IS the tag name
// (include/mistlane/entity_contract.hpp; examples/contracts/avatar.def), so
// the parser is a small line-aware XML tokenizer that walks the element stack
// and records the source line of every entity, property and method — the IDE
// panel turns those lines into click-to-open anchors. Read-only listing only:
// validation lives in `mistlane contract check`, this parser just must not
// lie about what is in the file.

export interface ContractArg {
  name: string;
  id: number;
  type: string;
  line: number;
}

export interface ContractMethod {
  name: string;
  id: number;
  mode: string;
  caller?: string;
  audience?: string;
  args: ContractArg[];
  returns: ContractArg[];
  line: number;
}

export interface ContractProperty {
  name: string;
  id: number;
  type: string;
  flags?: string;
  of?: string;
  default?: string;
  line: number;
}

export interface ContractEntity {
  name: string;
  id: number;
  properties: ContractProperty[];
  serverMethods: ContractMethod[];
  clientMethods: ContractMethod[];
  line: number;
}

export interface ContractDoc {
  typesSource?: { path: string; line: number };
  entities: ContractEntity[];
}

export class ContractParseError extends Error {
  constructor(message: string, readonly line: number) {
    super(`contract parse error at line ${line}: ${message}`);
    this.name = "ContractParseError";
  }
}

interface Tag {
  name: string;
  attrs: Record<string, string>;
  selfClosing: boolean;
  closing: boolean;
  line: number;
}

function parseAttrs(raw: string, line: number): Record<string, string> {
  const attrs: Record<string, string> = {};
  const pattern = /([\w:.-]+)\s*=\s*"([^"]*)"/g;
  let match: RegExpExecArray | null;
  while ((match = pattern.exec(raw)) !== null) {
    const name = match[1]!;
    if (name in attrs) throw new ContractParseError(`duplicate attribute ${name}`, line);
    attrs[name] = match[2]!;
  }
  return attrs;
}

/** Tokenizes every tag with its source line; text content is ignored. */
function scanTags(text: string): Tag[] {
  const tags: Tag[] = [];
  const pattern = /<\s*(\/?)([\w:.-]+)((?:\s[^<>]*?)?)\s*(\/?)\s*>/g;
  // Comments are blanked out positionally (newlines kept) so line numbers
  // survive and `-->` is never misread as a tag; the same for a doctype.
  const stripped = text
    .replace(/<!--[\s\S]*?-->/g, (comment) => comment.replace(/[^\n]/g, " "))
    .replace(/<![^>]*>/g, (decl) => decl.replace(/[^\n]/g, " "));
  const linesUpTo = (index: number, source: string = stripped): number => {
    let line = 1;
    for (let i = 0; i < index; i += 1) if (source[i] === "\n") line += 1;
    return line;
  };
  let match: RegExpExecArray | null;
  while ((match = pattern.exec(stripped)) !== null) {
    tags.push({
      name: match[2]!,
      attrs: match[1] === "/" ? {} : parseAttrs(match[3] ?? "", linesUpTo(match.index)),
      selfClosing: match[4] === "/",
      closing: match[1] === "/",
      line: linesUpTo(match.index),
    });
  }
  return tags;
}

export function parseContractDef(text: string): ContractDoc {
  const doc: ContractDoc = { entities: [] };
  const stack: string[] = [];
  let entity: ContractEntity | null = null;
  let method: ContractMethod | null = null;

  for (const tag of scanTags(text)) {
    if (tag.closing) {
      // Pop to the matching opener; a well-formed document pops exactly one.
      const at = stack.lastIndexOf(tag.name);
      if (at >= 0) stack.length = at;
      if (tag.name === "Method") method = null;
      if (tag.name === "Entity") entity = null;
      continue;
    }
    const depth = stack.length;
    switch (tag.name) {
      case "Contracts":
        if (tag.selfClosing || depth !== 0) throw new ContractParseError("Contracts must be the root element", tag.line);
        stack.push(tag.name);
        continue;
      case "TypesSource":
        if (depth !== 1) throw new ContractParseError("TypesSource belongs directly under Contracts", tag.line);
        if (!tag.attrs.path) throw new ContractParseError("TypesSource needs a path attribute", tag.line);
        doc.typesSource = { path: tag.attrs.path, line: tag.line };
        continue;
      case "Entity": {
        if (depth !== 1) throw new ContractParseError("Entity belongs directly under Contracts", tag.line);
        const id = Number(tag.attrs.id);
        if (!tag.attrs.name || !Number.isInteger(id)) {
          throw new ContractParseError("Entity needs name and integer id", tag.line);
        }
        entity = { name: tag.attrs.name, id, properties: [], serverMethods: [], clientMethods: [], line: tag.line };
        doc.entities.push(entity);
        stack.push(tag.name);
        continue;
      }
      case "Properties":
      case "ServerMethods":
      case "ClientMethods":
        if (!entity || depth !== 2) throw new ContractParseError(`${tag.name} belongs inside an Entity`, tag.line);
        stack.push(tag.name);
        continue;
      case "Method": {
        if (!entity || depth !== 3 || (stack[2] !== "ServerMethods" && stack[2] !== "ClientMethods")) {
          throw new ContractParseError("Method belongs inside ServerMethods/ClientMethods", tag.line);
        }
        const id = Number(tag.attrs.id);
        if (!tag.attrs.name || !Number.isInteger(id)) {
          throw new ContractParseError("Method needs name and integer id", tag.line);
        }
        method = {
          name: tag.attrs.name,
          id,
          mode: tag.attrs.mode ?? "request",
          caller: tag.attrs.caller,
          audience: tag.attrs.audience,
          args: [],
          returns: [],
          line: tag.line,
        };
        if (stack[2] === "ServerMethods") entity.serverMethods.push(method);
        else entity.clientMethods.push(method);
        stack.push(tag.name);
        continue;
      }
      case "Arg":
      case "Field": {
        const inside = tag.name === "Arg" ? "Method" : "Returns";
        if (!method || depth < 3 || stack[depth - 1] !== inside) {
          throw new ContractParseError(`${tag.name} belongs inside ${inside}`, tag.line);
        }
        const id = Number(tag.attrs.id);
        if (!tag.attrs.name || !tag.attrs.type || !Number.isInteger(id)) {
          throw new ContractParseError(`${tag.name} needs name, integer id and type`, tag.line);
        }
        const field = { name: tag.attrs.name, id, type: tag.attrs.type, line: tag.line };
        if (tag.name === "Arg") method.args.push(field);
        else method.returns.push(field);
        continue;
      }
      case "Returns":
        if (!method) throw new ContractParseError("Returns belongs inside a Method", tag.line);
        stack.push(tag.name);
        continue;
      default: {
        // Inside <Properties> an unknown tag name IS the property name; the
        // id attribute carries the stable property number.
        if (entity && stack[stack.length - 1] === "Properties" && depth === 3) {
          const id = Number(tag.attrs.id);
          if (!Number.isInteger(id)) throw new ContractParseError(`property ${tag.name} needs an integer id`, tag.line);
          entity.properties.push({
            name: tag.name,
            id,
            type: tag.attrs.type ?? "",
            flags: tag.attrs.flags,
            of: tag.attrs.of,
            default: tag.attrs.default,
            line: tag.line,
          });
          continue;
        }
        if (tag.name === "Types") { stack.push(tag.name); continue; }
        // Everything else (types document bodies, comments, item fields) is
        // out of scope for the listing face; keep walking.
        if (!tag.selfClosing) stack.push(tag.name);
        continue;
      }
    }
  }
  return doc;
}

/** A row for the IDE contract table: one method with its owning entity. */
export interface ContractMethodRow {
  entity: string;
  entityId: number;
  side: "server" | "client";
  method: string;
  methodId: number;
  mode: string;
  caller: string;
  args: string;
  returns: string;
  line: number;
}

export function contractRows(doc: ContractDoc): ContractMethodRow[] {
  const rows: ContractMethodRow[] = [];
  for (const entity of doc.entities) {
    for (const side of ["serverMethods", "clientMethods"] as const) {
      for (const method of entity[side]) {
        rows.push({
          entity: entity.name,
          entityId: entity.id,
          side: side === "serverMethods" ? "server" : "client",
          method: method.name,
          methodId: method.id,
          mode: method.mode,
          caller: method.caller ?? method.audience ?? "-",
          args: method.args.map((arg) => `${arg.name}:${arg.type}`).join(", ") || "-",
          returns: method.returns.map((field) => `${field.name}:${field.type}`).join(", ") || "-",
          line: method.line,
        });
      }
    }
  }
  return rows;
}
