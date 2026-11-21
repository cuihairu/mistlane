#!/usr/bin/env python3
"""Avatar spawn/destroy design-contract regression fixture.

Pins contracts/avatar-spawn-destroy.json (the PL-2 deliverable) to its published
invariants so the design cannot silently drift: the destroy obligation order, the
required fields with defaults, the four acceptance paths and the failure
compensation coverage. Self-contained: reads only committed repo artifacts.

Run from anywhere:
    python3 tests/test_avatar_contract.py
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONTRACT = os.path.join(ROOT, "contracts", "avatar-spawn-destroy.json")
RECONNECT = os.path.join(ROOT, "contracts", "avatar-reconnect-cutover.json")

PIN = {
    "identity": ["origin_realm", "character_id", "hosting_generation"],
    "destroy_obligations": ["flush", "leave_broadcast", "ref_release", "recycle_bin", "tombstone"],
    "four_paths": ["create_success", "failure_rollback", "reclaim_complete", "reclaim_interrupt_recovery"],
    "compensation_interruptions": [
        "save fails during reclaiming",
        "disconnect during reclaiming",
        "host crash mid-reclaim",
        "reference-release interrupted",
    ],
    "load_source": ["home", "transfer-in"],
}


def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def check(cond, msg):
    if not cond:
        fail(msg)


def main():
    c = json.load(open(CONTRACT, encoding="utf-8"))

    check(c["contract"] == "avatar-spawn-destroy", f"contract={c['contract']}")
    check(c["oracle"] == "docs/avatar-lifecycle.md (PL-1): loading/active/suspended/visiting/transferring/reclaiming/closed",
          "oracle reference drifted (PL-1 state set changed?)")

    # Identity tuple.
    ident = [f["field"] for f in c["identity"]]
    check(ident == PIN["identity"], f"identity={ident} != {PIN['identity']}")

    # Destroy obligations: exact names, exact order, steps 1..N.
    obl = c["destroy"]["obligations_in_order"]
    check([o["name"] for o in obl] == PIN["destroy_obligations"],
          f"obligations={[o['name'] for o in obl]}")
    check([o["step"] for o in obl] == list(range(1, len(obl) + 1)), "obligation steps not 1..N in order")

    # Required spawn fields have declared types and defaults; required ones have no default.
    by_field = {f["field"]: f for f in c["spawn"]["required_fields"]}
    for f in PIN["identity"]:
        check(f in by_field, f"spawn missing identity field {f}")
    check(by_field["load_source"]["default"] == "home", "load_source default != home")
    check(by_field["declared_timers"]["default"] == [], "declared_timers default != []")
    check(c["spawn"]["load_source"]["values"] == PIN["load_source"], "load_source values drifted")

    # Four acceptance paths, named exactly.
    paths = [p["name"] for p in c["four_paths"]]
    check(paths == PIN["four_paths"], f"four_paths={paths}")
    for p in c["four_paths"]:
        check(p["diagram"] and p["steps"], f"path {p['name']} missing diagram reference or steps")

    # Failure compensation covers every interruption class.
    interruptions = [x["interruption"] for x in c["failure_compensation"]]
    for want in PIN["compensation_interruptions"]:
        check(want in interruptions, f"compensation missing: {want}")
    for x in c["failure_compensation"]:
        check(x["compensation"], f"compensation for {x['interruption']} is empty")

    # Mid-life reclaim never-rules present (single-writer invariants).
    never = c["destroy"]["mid_life_reclaim"]["never"]
    for rule in ("half-publish", "tombstone an unacknowledged change", "free the object mid-obligation"):
        check(any(rule in n for n in never), f"mid_life_reclaim.never missing rule: {rule}")

    # --- Reconnect / daily-cutover contract ---------------------------------
    rc = json.load(open(RECONNECT, encoding="utf-8"))
    check(rc["contract"] == "avatar-reconnect-cutover", f"rc contract={rc['contract']}")

    # Suspended sub-states, exact names.
    subs = [s["state"] for s in rc["suspended_sub_states"]]
    check(subs == ["suspended-waiting", "suspended-expiring"], f"sub_states={subs}")

    # Double-lane verdicts carry the four fast-lane conditions and the slow-lane reload.
    fast = rc["reconnect_double_lane"]["fast_lane"]["condition"]
    for cond in ("grace", "generation still current", "no intervening writer", "in-memory state retained"):
        check(cond in fast, f"fast-lane condition missing: {cond}")
    slow = rc["reconnect_double_lane"]["slow_lane"]["result"]
    check("loading -> active" in slow, "slow lane does not reload through loading")

    # Three reconnect paths, named exactly, each with steps.
    rpaths = [p["name"] for p in rc["three_reconnect_paths"]]
    check(rpaths == ["fast_reattach", "grace_expired_reload", "kick_on_relogin"], f"reconnect paths={rpaths}")
    for p in rc["three_reconnect_paths"]:
        check(p["diagram"] and p["steps"], f"path {p['name']} missing diagram reference or steps")

    # Daily cutover: trigger surface non-empty, three layers in bottom-up order.
    check(rc["daily_cutover"]["trigger_surface"], "daily cutover has no trigger surface")
    layers = [l["layer"] for l in rc["daily_cutover"]["three_layer_touch_order"]]
    check(layers == [1, 2, 3], f"layer order={layers}")
    names = [l["name"] for l in rc["daily_cutover"]["three_layer_touch_order"]]
    check(names == ["avatar", "scene", "node"], f"layer names={names}")

    # Hook signatures present and failure compensation covers the three classes.
    for hook in ("onAvatarDailyReset", "onDailyReset"):
        check(hook in rc["daily_cutover"]["hook_signatures"], f"missing hook {hook}")
    for want in ("save fails", "hook fails", "barrier not released"):
        check(any(want in x["interruption"] for x in rc["daily_cutover"]["failure_compensation"]),
              f"cutover compensation missing: {want}")

    print(f"OK: avatar contracts pinned: "
          f"{len(PIN['destroy_obligations'])} obligations / {len(PIN['four_paths'])} paths, "
          f"{len(subs)} suspended sub-states / {len(rpaths)} reconnect paths, "
          f"{len(names)} cutover layers / {len(rc['daily_cutover']['hook_signatures'])} cutover hooks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
