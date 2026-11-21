#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate contracts/hooks.xml, the single hook registry.

The registry is the one place the lifecycle hooks are named, so it closes its
vocabulary: unknown elements or attribute values are errors, not defaults. It
also refuses an orphan hook -- one with no home in the avatar lifecycle oracle
- and checks the mapping to the avatar-runtime Lua hook surface is complete in
both directions, so no Lua hook goes unregistered and no registry entry claims
a name the Lua surface does not have.

A hook has a home in exactly one of two ways: an object-level hook names at
least one oracle state, or a process-level hook names a declared trigger.

Usage: tools/check_hooks.py [path]      (default contracts/hooks.xml)
"""
import sys
import xml.etree.ElementTree as ET

# Closed vocabularies. Anything outside these sets is refused.
ORACLE_STATES = {
    "loading", "active", "suspended", "reconnecting", "transferring",
    "visiting", "closing", "closed", "purged",
}
LEVELS = {"process", "object"}
THREADS = {"asio"}
FAILURES = {"refuse", "logged", "abort"}
RETURNS = {"none", "verdict", "avatar-table", "state-table", "snapshot"}
TRIGGERS = {"day-boundary"}
REENTRANT = {"no"}

# The avatar-runtime Lua hook surface: the closed list a registry entry may map
# to, and which must all be covered by exactly one registry entry.
LUA_SURFACE = {
    "avatar_load", "avatar_enter", "avatar_leave", "avatar_tick", "avatar_save",
    "avatar_transfer_out", "avatar_transfer_in", "avatar_detach", "avatar_fail",
}


def fail(msg):
    print("hooks: " + msg)
    sys.exit(1)


def attribute(el, name, hook):
    value = el.get(name)
    if value is None:
        fail("%s: missing %s" % (hook, name))
    return value


def check_state(value, hook):
    states = value.split()
    if not states:
        return []
    seen = set()
    for state in states:
        if state not in ORACLE_STATES:
            fail("%s: state %r is not an avatar lifecycle state" % (hook, state))
        if state in seen:
            fail("%s: state %r repeated" % (hook, state))
        seen.add(state)
    return states


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "contracts/hooks.xml"
    try:
        root = ET.parse(path).getroot()
    except (ET.ParseError, OSError) as exc:
        fail("cannot parse %s: %s" % (path, exc))
    if root.tag != "HookRegistry":
        fail("root must be HookRegistry, found %s" % root.tag)
    if not root.get("version") or not root.get("scope"):
        fail("HookRegistry needs version and scope")

    hooks = root.findall("Hook")
    if not hooks:
        fail("no hooks registered")

    names, lua_targets = set(), {}
    for hook in hooks:
        name = attribute(hook, "name", "hook")
        if name in names:
            fail("%s: hook declared twice" % name)
        names.add(name)

        # Closed-vocabulary attributes.
        level = attribute(hook, "level", name)
        if level not in LEVELS:
            fail("%s: level %r is not closed" % (name, level))
        thread = attribute(hook, "thread", name)
        if thread not in THREADS:
            fail("%s: thread %r is not closed" % (name, thread))
        failure = attribute(hook, "failure", name)
        if failure not in FAILURES:
            fail("%s: failure %r is not closed" % (name, failure))
        returns = attribute(hook, "returns", name)
        if returns not in RETURNS:
            fail("%s: returns %r is not closed" % (name, returns))
        reentrant = attribute(hook, "reentrant", name)
        if reentrant not in REENTRANT:
            fail("%s: reentrant %r is not closed" % (name, reentrant))
        check_state(attribute(hook, "states", name), name)

        # A home, in exactly one form.
        trigger = hook.get("trigger", "")
        states = hook.get("states", "").split()
        triggers = trigger.split()
        if states and triggers:
            fail("%s: names both oracle states and a trigger" % name)
        if not states and not triggers:
            fail("%s: orphan hook, no oracle state and no declared trigger" % name)
        for value in triggers:
            if value not in TRIGGERS:
                fail("%s: trigger %r is not closed" % (name, value))
        if triggers and level != "process":
            fail("%s: a trigger belongs to a process-level hook" % name)
        if states and level != "object":
            fail("%s: oracle states belong to an object-level hook" % name)

        # The contract fields: timing, signature, error.
        for child in ("Timing", "Error"):
            node = hook.find(child)
            if node is None or not (node.text or "").strip():
                fail("%s: needs a %s" % (name, child))
        signature = hook.find("Signature")
        if signature is None or not signature.findall("Arg"):
            fail("%s: needs a Signature with at least one Arg" % name)
        if returns != "none" and hook.find("Returns") is None:
            fail("%s: returns %s needs a Returns" % (name, returns))

        # Lua mapping: unique and drawn from the closed Lua surface.
        lua = hook.get("lua")
        if lua:
            if lua not in LUA_SURFACE:
                fail("%s: lua %r is not an avatar-runtime hook" % (name, lua))
            if lua in lua_targets:
                fail("%s: lua %r claimed by both %s and %s"
                     % (name, lua, lua_targets[lua], name))
            lua_targets[lua] = name

    uncovered = sorted(LUA_SURFACE - set(lua_targets))
    if uncovered:
        fail("no registry entry maps the Lua hook(s) %s" % ", ".join(uncovered))

    print("%d hooks registered, %d mapped onto the Lua surface, vocabulary closed, "
          "no orphan hooks" % (len(hooks), len(lua_targets)))


if __name__ == "__main__":
    main()
