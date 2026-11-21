#!/usr/bin/env python3
"""Build a machine-readable inventory of the recovered game-fogs sproto protocol.

Reads the recovered schema (contracts/fogs-sproto-schema.txt) and the game-fogs
handler registrations + error-code table, and emits contracts/fogs-proto-inventory.json.

The schema is the authoritative wire contract (names/ids/request-response types/fields).
Handlers come from PacketHandler.lua (client requests) and StatePacketHandler.lua (internal
state messages) across the Hegel/World/CrossWorld/Cache/Pay worlds; error codes from
game/Common/EC.lua. Per-message authorization is the observed dispatch model: handleClientMsg
keys on h.aid (the character must exist) and each handler applies its own checks (the
entry-by-entry audit the ledger defers). Version-compatibility rules are absent from the
legacy source and recorded as such.

Usage:
  python3 tools/sproto_inventory.py <schema.txt> <game-fogs-root>
  (schema.txt = contracts/fogs-sproto-schema.txt; game-fogs-root = game-fogs repo root)
"""
import json
import os
import re
import sys


def parse_schema(path):
    """Parse the sproto_dump output into protocols and types."""
    protos, types, section, cur = {}, {}, None, None
    for ln in open(path, encoding="utf-8", errors="replace"):
        if ln.startswith("=== "):
            section = "protocols" if "protocol" in ln else "types"
            continue
        s = ln.strip()
        if section == "protocols":
            m = re.match(r"^(\S+) \((\d+)\) request:(\S+)(?: response:(\S+))?$", s)
            if m:
                protos[m.group(1)] = {
                    "id": int(m.group(2)),
                    "req": m.group(3),
                    "resp": m.group(4),
                }
        elif s and not ln.startswith("\t"):
            cur, types[cur] = s, []
        elif s and cur:
            fm = re.match(r"^(\S+) \((\d+)\) (\S+)$", s)
            if fm:
                types[cur].append(
                    {"name": fm.group(1), "tag": int(fm.group(2)), "type": fm.group(3)}
                )
    return protos, types


def parse_handlers(root, pattern, field):
    """Parse 'PacketHandler[Protocol.NAME] = expr' / 'StatePacketHandler[...]' across worlds."""
    out = {}
    for dirpath, _, files in os.walk(root + "/game"):
        for fn in files:
            if fn != pattern:
                continue
            # world = the dir above Rpc, e.g. game/Hegel/Rpc -> Hegel
            segs = dirpath.split("/")
            world = segs[-2] if len(segs) >= 2 else "?"
            for ln in open(dirpath + "/" + fn, encoding="utf-8", errors="replace"):
                m = re.match(r"\s*" + field + r"\[Protocol\.(\w+)\]\s*=\s*(.+?)\s*$", ln)
                if m:
                    out.setdefault(m.group(1), {})[world] = m.group(2).strip().rstrip(",")
    return out


def parse_error_codes(path):
    codes = {}
    for ln in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"\s*(\w+)\s*=\s*(-?\d+)\s*,", ln)
        if m:
            codes[m.group(1)] = int(m.group(2))
    return codes


def main():
    schema_path, root = sys.argv[1], sys.argv[2]
    protos, types = parse_schema(schema_path)
    client_handlers = parse_handlers(root, "PacketHandler.lua", "PacketHandler")
    state_handlers = parse_handlers(root, "StatePacketHandler.lua", "StatePacketHandler")
    ec = parse_error_codes(root + "/game/Common/EC.lua")

    # Resolve every handler name to a schema protocol; report unresolved.
    unresolved = sorted(
        n for n in set(client_handlers) | set(state_handlers) if n not in protos
    )

    inventory = {"protocols": [], "error_codes": ec, "unresolved_handlers": unresolved}
    for name, p in sorted(protos.items(), key=lambda kv: kv[1]["id"]):
        direction = []
        if name in client_handlers:
            direction.append("client-request")
        if name in state_handlers:
            direction.append("internal-state")
        if not p["req"] or p["req"] == "(null)":
            direction.append("server-push")
        if not direction:
            direction.append("server-response" if p["resp"] else "undefined")
        inventory["protocols"].append(
            {
                "name": name,
                "id": p["id"],
                "request_type": p["req"],
                "response_type": p["resp"],
                "request_fields": types.get(p["req"], []),
                "response_fields": types.get(p["resp"], []),
                "direction": direction,
                "client_handlers": client_handlers.get(name, {}),
                "state_handlers": state_handlers.get(name, {}),
            }
        )

    counts = {}
    for p in inventory["protocols"]:
        for d in p["direction"]:
            counts[d] = counts.get(d, 0) + 1
    inventory["summary"] = {
        "protocols": len(inventory["protocols"]),
        "error_codes": len(ec),
        "direction_counts": counts,
        "handler_protocols": len(set(client_handlers) | set(state_handlers)),
    }
    json.dump(inventory, open("contracts/fogs-proto-inventory.json", "w"), indent=1)
    print(
        f"protocols={len(inventory['protocols'])} error_codes={len(ec)} "
        f"directions={counts} handler_protocols={inventory['summary']['handler_protocols']} "
        f"unresolved={len(unresolved)}"
    )


if __name__ == "__main__":
    main()
