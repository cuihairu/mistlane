#!/usr/bin/env python3
"""Protocol-contract recovery regression fixture.

Pins the recovered game-fogs sproto contract to its published invariants so the
recovery (contracts/fogs-sproto-schema.txt, contracts/fogs-proto-inventory.json)
cannot silently drift. Self-contained: reads only committed repo artifacts; no
game-fogs checkout, Lua interpreter, or network is required.

Run from anywhere:
    python3 tests/test_protocol_recovery.py

The pinned numbers are the ones published in docs/protocol-contract-recovery.md
(Verification) and docs/protocol-inventory.md, re-derived here from the committed
artifacts so an accidental edit that breaks an invariant fails this fixture.
"""
import json
import os
import re
import sys

# Repo root: tests/ -> repo root
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from sproto_inventory import parse_schema  # noqa: E402

SCHEMA = os.path.join(ROOT, "contracts", "fogs-sproto-schema.txt")
INVENTORY = os.path.join(ROOT, "contracts", "fogs-proto-inventory.json")
AUDIT = os.path.join(ROOT, "contracts", "fogs-permission-audit.json")

# Published invariants (docs/protocol-contract-recovery.md, docs/protocol-inventory.md).
PIN = {
    "protocols": 646,
    "types": 1421,
    "fields": 4245,
    "array_fields": 638,
    "map_fields": 152,
    "no_req": 69,  # pushes (no request type)
    "no_resp": 151,
    "error_codes": 197,
    "direction": {
        "client-request": 361,
        "internal-state": 190,
        "server-push": 69,
        "server-response": 61,
        "undefined": 8,
    },
    "handler_protocols": 526,
    "unresolved_handlers": [
        "ANA_DoBattleFromHegel",
        "ANA_GetNextInfoFromHegel",
        "ANA_SycPlayerList",
        "ANA_UploadPlayer",
    ],
}

PRIMITIVES = {"integer", "boolean", "string", "double"}


def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def check(cond, msg):
    if not cond:
        fail(msg)


def main():
    protos, types = parse_schema(SCHEMA)
    inv = json.load(open(INVENTORY, encoding="utf-8"))

    # --- Schema shape -------------------------------------------------------
    check(len(protos) == PIN["protocols"], f"protocols={len(protos)} != {PIN['protocols']}")
    check(len(types) == PIN["types"], f"types={len(types)} != {PIN['types']}")

    fields = sum(len(v) for v in types.values())
    check(fields == PIN["fields"], f"fields={fields} != {PIN['fields']}")
    array_fields = sum(1 for v in types.values() for f in v if f["type"].startswith("*"))
    map_fields = sum(1 for v in types.values() for f in v if "[" in f["type"])
    check(array_fields == PIN["array_fields"], f"array_fields={array_fields}")
    check(map_fields == PIN["map_fields"], f"map_fields={map_fields}")

    no_req = sum(1 for p in protos.values() if not p["req"] or p["req"] == "(null)")
    no_resp = sum(1 for p in protos.values() if not p["resp"] or p["resp"] == "(null)")
    check(no_req == PIN["no_req"], f"no_req={no_req} != {PIN['no_req']}")
    check(no_resp == PIN["no_resp"], f"no_resp={no_resp} != {PIN['no_resp']}")

    # Protocol ids unique.
    ids = [p["id"] for p in protos.values()]
    check(len(set(ids)) == len(ids), "duplicate protocol ids")

    # --- Inventory summary --------------------------------------------------
    s = inv["summary"]
    check(s["protocols"] == PIN["protocols"], f"summary.protocols={s['protocols']}")
    check(s["error_codes"] == PIN["error_codes"], f"error_codes={s['error_codes']}")
    check(s["handler_protocols"] == PIN["handler_protocols"],
          f"handler_protocols={s['handler_protocols']}")
    for d, n in PIN["direction"].items():
        check(s["direction_counts"].get(d) == n, f"direction {d}={s['direction_counts'].get(d)} != {n}")
    check(sorted(inv["unresolved_handlers"]) == sorted(PIN["unresolved_handlers"]),
          f"unresolved={inv['unresolved_handlers']}")

    # --- Schema <-> inventory cross-consistency ------------------------------
    by_name = {p["name"]: p for p in inv["protocols"]}
    check(len(by_name) == len(protos), "inventory protocol count != schema protocol count")
    for name, p in protos.items():
        entry = by_name.get(name)
        check(entry is not None, f"inventory missing protocol {name}")
        check(entry["id"] == p["id"], f"id mismatch for {name}")
        check(entry["request_type"] == p["req"], f"req type mismatch for {name}")
        check(entry["response_type"] == p["resp"], f"resp type mismatch for {name}")

    # Every request/response body and field type resolves to a declared type or primitive.
    for name, entry in by_name.items():
        for tn in (entry["request_type"], entry["response_type"]):
            if tn and tn != "(null)" and tn not in types:
                fail(f"{name}: body type {tn} undeclared")
        for tn, fields_list in (("req", entry["request_fields"]), ("resp", entry["response_fields"])):
            for f in fields_list:
                base = f["type"].lstrip("*").split("[")[0]
                if base not in PRIMITIVES and base not in types:
                    fail(f"{name}.{tn}: field {f['name']} type {base} undeclared")

    # Direction classes consistent with handler registration / request presence.
    for name, entry in by_name.items():
        d = entry["direction"]
        if entry["client_handlers"]:
            check("client-request" in d, f"{name}: has client handler but no client-request dir")
        if entry["state_handlers"]:
            check("internal-state" in d, f"{name}: has state handler but no internal-state dir")
        if not entry["request_type"] or entry["request_type"] == "(null)":
            check("server-push" in d, f"{name}: no request type but no server-push dir")

    # --- Permission-audit registry -------------------------------------------
    audit = json.load(open(AUDIT, encoding="utf-8"))
    asum = audit["summary"]
    check(asum["client_request_messages"] == 361, f"audit client_request={asum['client_request_messages']}")
    check(asum["resolved"] == 361, f"audit resolved={asum['resolved']}")
    check(asum["unresolved"] == 0, f"audit unresolved={asum['unresolved']}")
    check(asum["payload_subject"] == 26, f"audit payload_subject={asum['payload_subject']}")
    ctx = asum["by_char_context"]
    check(ctx.get("c") == 252 and ctx.get("aid") == 107 and ctx.get("h") == 2,
          f"audit char_context={ctx}")
    # Every client-request message has exactly one audit row; rows reference known messages.
    audited = {}
    for r in audit["handlers"]:
        audited.setdefault(r["message"], []).append(r["world"])
        if r["resolved"]:
            check(r["file"] and r["char_context"] is not None,
                  f"audit row {r['message']} missing resolution")
    client_msgs = [p["name"] for p in inv["protocols"] if p["client_handlers"]]
    check(set(client_msgs) == set(audited), "audit messages != inventory client-request set")

    print(f"OK: {PIN['protocols']} protocols / {PIN['types']} types / {PIN['fields']} fields; "
          f"{PIN['handler_protocols']} handled / {PIN['error_codes']} error codes; "
          f"{len(PIN['unresolved_handlers'])} unresolved; "
          f"audit {asum['resolved']}/{asum['client_request_messages']} resolved, "
          f"{asum['payload_subject']} payload-scoped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
