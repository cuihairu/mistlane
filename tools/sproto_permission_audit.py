#!/usr/bin/env python3
"""Per-message authorization audit for the recovered game-fogs client protocol.

Resolves every client-request handler (PacketHandler[Protocol.X] = Module.func) to its
defining Lua function, records its signature and the authorization-related checks in the
function body, and emits contracts/fogs-permission-audit.json.

The recovered protocol has TWO different client dispatch conventions, both observed in
game-fogs (commit 37b640a; no drift to HEAD in the handler / error-code files):

  Hegel  game/Common/CService/Server/HegelService.lua:242 handleClientMsg
         in-dispatch gate: CharactorManager.getObjByAid(h.aid) must return a character,
         else the handler is not called; then the handler is invoked (h, p, c).
         => the handler receives the character object c.
  World  game/Common/CService/Server/WorldService.lua:370 onGwMsg, `if not h.cid` branch
         ph[h.type](gid, h.aid, p) with NO in-dispatch character-existence gate;
         => the handler receives the character id aid and must gate on it itself.
  CrossWorld / Pay / Cache register no client dispatch path (CrossWorld has one orphan
  registration; its service never reads PacketHandler).

Each resolved handler is classified by the character context its signature can receive
(c / aid / h / none) and by self-check patterns found in its body. The classification
records what is present; it does not decide whether a handler is sufficiently authorized —
that judgment is left to the reader, with the guards recorded as evidence.

The audit is evidence for the protocol contract (todo item 73); it does not change the
recovered contract.

Usage:
  python3 tools/sproto_permission_audit.py <inventory.json> <game-fogs-root>
"""
import json
import os
import re
import sys


# Handler registration RHS stored by sproto_inventory.py: 'Module.func'
REGEX_REG = re.compile(r"^([A-Za-z_]\w*)\.([A-Za-z_]\w*)$")

# Definition styles: 'function Module.func(args)' and 'Module.func = function(args)'
REGEX_DEF = re.compile(r"^[\s]*function\s+([A-Za-z_]\w*)\.([A-Za-z_]\w*)\s*\((.*?)\)")
REGEX_DEF_ASSIGN = re.compile(
    r"^[\s]*([A-Za-z_]\w*)\.([A-Za-z_]\w*)\s*=\s*function\s*\((.*?)\)"
)

# Per-world dispatch convention (see module docstring).
CONVENTION = {
    "Hegel": {"gate": "aid-existence in dispatch (CharactorManager.getObjByAid)",
              "call": "ph[h.type](h, p, c)"},
    "World": {"gate": "none in dispatch; handler must gate on aid itself",
              "call": "ph[h.type](gid, h.aid, p)"},
    "CrossWorld": {"gate": "none: no client dispatch path in CrossWorldService",
                   "call": "n/a (orphan registration)"},
    "Pay": {"gate": "none: no client dispatch path in PayService", "call": "n/a"},
    "Cache": {"gate": "none: no client dispatch path in CacheService", "call": "n/a"},
}

# Authorization-relevant patterns (best-effort; evidence, not verdict).
GUARD_PATTERNS = [
    ("role", r"\brole\b|isAdmin|isGM|isManager|checkPower|hasPower|privilege|gm\b"),
    ("ownership", r"h\.aid\s*[!=]=?\s*|owner|getObjByAid"),
    ("state", r"\.status\s*[!=]=?|\.state\s*[!=]=?|if\s+not\s+c\b|assert\s*\(\s*c\b"),
    ("amount", r"p\.(?:num|count|amount|gold|diamond|money)\b|checkGold|checkMoney|costItem"),
    ("level", r"c\.level|\.lv\b\s*[<>=]"),
]

# A payload-provided operation subject: the client controls which character the
# operation targets. Safe only when the handler compares it against the dispatch
# identity (h.aid / aid / c); flagged for review otherwise.
PAYLOAD_SUBJECT = re.compile(r"\bp\.(?:aid|targetAid|tarAid|otherAid|friendAid)\b")

# The API layer is a thin wrapper; the real body is the first Logic-layer call.
REGEX_LOGIC_CALL = re.compile(r"\b([A-Za-z_]\w*Logic)\.([A-Za-z_]\w*)\s*\(")


def function_body(lines, start):
    """Body text from the def line to the matching top-level 'end' (0-indent).

    Functions in this codebase are top-level; a line 'end' at column 0 closes the
    def. Capped at 120 lines as a safety net."""
    for j in range(start, min(start + 120, len(lines))):
        if lines[j].rstrip("\n") == "end" and j > start:
            return "".join(lines[start:j + 1])
    return "".join(lines[start:start + 120])


def index_definitions(root):
    """Map (Module, func) -> [{file, line, params, body}]."""
    index = {}
    for dirpath, _, fns in os.walk(root + "/game"):
        for fn in fns:
            if not fn.endswith(".lua"):
                continue
            path = os.path.join(dirpath, fn)
            with open(path, encoding="utf-8", errors="replace") as f:
                lines = f.readlines()
            for i, ln in enumerate(lines):
                s = ln.lstrip()
                if s.startswith("--"):
                    continue
                m = REGEX_DEF.match(ln) or REGEX_DEF_ASSIGN.match(ln)
                if not m:
                    continue
                mod, func, params = m.group(1), m.group(2), m.group(3)
                index.setdefault((mod, func), []).append({
                    "file": path, "line": i + 1, "params": params,
                    "body": function_body(lines, i),
                })
    return index


def world_of(path):
    segs = path.split(os.sep)
    try:
        return segs[segs.index("game") + 1]
    except (ValueError, IndexError):
        return "?"


def char_context(params):
    """Which character context the handler signature can receive."""
    names = [n.strip() for n in params.split(",") if n.strip()]
    if "c" in names:
        return "c"
    if "aid" in names:
        return "aid"
    if "h" in names:
        return "h"
    return "none"


def main():
    inv_path, root = sys.argv[1], sys.argv[2]
    inventory = json.load(open(inv_path, encoding="utf-8"))
    index = index_definitions(root)

    rows, unresolved = [], []
    for p in inventory["protocols"]:
        for world, expr in p.get("client_handlers", {}).items():
            name = p["name"]
            m = REGEX_REG.match(expr)
            base = {"message": name, "world": world, "expr": expr,
                    "gate": CONVENTION.get(world, {}).get("gate", "unknown")}
            if not m:
                rows.append({**base, "handler_module": None, "handler_func": None,
                             "file": None, "line": None, "params": None,
                             "char_context": None, "guards": [], "logic": None,
                             "payload_subject": False, "resolved": False,
                             "anomaly": "unparseable-registration"})
                continue
            mod, func = m.group(1), m.group(2)
            cands = index.get((mod, func), [])
            if not cands:
                unresolved.append({"message": name, "world": world, "ref": f"{mod}.{func}"})
                rows.append({**base, "handler_module": mod, "handler_func": func,
                             "file": None, "line": None, "params": None,
                             "char_context": None, "guards": [], "logic": None,
                             "payload_subject": False, "resolved": False,
                             "anomaly": "unresolved-definition"})
                continue
            same = [c for c in cands if world_of(c["file"]) == world]
            choice = (same or cands)[0]
            params = choice["params"].strip()
            guards = [label for label, pat in GUARD_PATTERNS
                      if re.search(pat, choice["body"], re.I)]
            ctx = char_context(params)

            # Follow the first Logic-layer call (the API wrapper is thin); the real
            # body and any authorization live there.
            logic = None
            lm = REGEX_LOGIC_CALL.search(choice["body"])
            if lm:
                lcands = index.get((lm.group(1), lm.group(2)), [])
                lsame = [c for c in lcands if world_of(c["file"]) == world]
                lchoice = (lsame or lcands)[0] if lcands else None
                if lchoice:
                    logic = {
                        "file": lchoice["file"], "line": lchoice["line"],
                        "call": f"{lm.group(1)}.{lm.group(2)}",
                        "params": lchoice["params"].strip(),
                        "guards": [label for label, pat in GUARD_PATTERNS
                                   if re.search(pat, lchoice["body"], re.I)],
                        "payload_subject": bool(PAYLOAD_SUBJECT.search(lchoice["body"])),
                    }
                    for g in logic["guards"]:
                        if g not in guards:
                            guards.append(g)

            payload_subject = bool(PAYLOAD_SUBJECT.search(choice["body"])) or (
                logic and logic["payload_subject"]
            )
            anomaly = None
            if not same:
                anomaly = "cross-world-definition"
            elif len(same) > 1:
                anomaly = "multiple-definitions-in-world"
            elif ctx == "none":
                anomaly = "no-character-context"
            elif payload_subject:
                anomaly = "payload-scoped-subject"
            rows.append({**base, "handler_module": mod, "handler_func": func,
                         "file": choice["file"], "line": choice["line"],
                         "params": params, "char_context": ctx, "guards": guards,
                         "logic": logic, "payload_subject": payload_subject,
                         "resolved": True, "anomaly": anomaly})

    summary = {
        "client_request_messages": len({r["message"] for r in rows}),
        "registrations": len(rows),
        "resolved": sum(1 for r in rows if r["resolved"]),
        "unresolved": sum(1 for r in rows if not r["resolved"]),
        "with_guard": sum(1 for r in rows if r["resolved"] and r["guards"]),
        "no_guard": sum(1 for r in rows if r["resolved"] and not r["guards"]),
        "logic_resolved": sum(1 for r in rows if r["logic"]),
        "payload_subject": sum(1 for r in rows if r["payload_subject"]),
        "by_char_context": {c: sum(1 for r in rows if r["char_context"] == c)
                           for c in ("c", "aid", "h", "none", None)},
        "by_world": {},
        "anomalies": {},
    }
    for r in rows:
        summary["by_world"][r["world"]] = summary["by_world"].get(r["world"], 0) + 1
    for r in rows:
        if r["anomaly"]:
            summary["anomalies"][r["anomaly"]] = \
                summary["anomalies"].get(r["anomaly"], 0) + 1

    out = {"summary": summary, "handlers": rows, "unresolved_refs": unresolved}
    json.dump(out, open("contracts/fogs-permission-audit.json", "w"), indent=1)
    print(json.dumps(summary, indent=1))
    if unresolved:
        print("unresolved refs:")
        for u in unresolved[:20]:
            print(f"  {u['message']} [{u['world']}] {u['ref']}")


if __name__ == "__main__":
    main()
