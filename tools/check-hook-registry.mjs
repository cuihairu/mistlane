// Hook registry validation (ledger PL-5).
//
// Acceptance = no orphan hooks: every hook named in the lifecycle oracle's
// refining pages/contracts resolves into contracts/hook-registry.json, and
// every registry hook carries signature/timing/thread/failure/reentrancy/Lua
// surface plus a landing point inside the oracle's seven-state machine.
//
// Run: node tools/check-hook-registry.mjs
import { readFileSync } from 'node:fs'
import { resolve, dirname } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const read = (p) => readFileSync(resolve(root, p), 'utf8')

const problems = []
const bad = (msg) => problems.push(msg)

const registry = JSON.parse(read('contracts/hook-registry.json'))
const oracleStates = new Set(registry.oracle_states)

// The scan set: the oracle's refining pages and contracts — the only places
// hook names are defined. The fogs contracts pin legacy sproto protocol
// names (onLogin, onBetCross, ...), not engine hooks; they are out of scope.
const scanSet = [
  'docs/avatar-lifecycle.md',
  'docs/avatar-spawn-destroy.md',
  'docs/avatar-reconnect-cutover.md',
  'docs/cross-server-lifecycle.md',
  'contracts/avatar-spawn-destroy.json',
  'contracts/avatar-reconnect-cutover.json',
  'contracts/cross-server-lifecycle.json',
]

const hooks = registry.hooks
const names = hooks.map((h) => h.name)
if (new Set(names).size !== names.length) bad('duplicate hook names in the registry')

const requiredFields = ['name', 'family', 'signature', 'timing', 'thread', 'failure', 'reentrancy', 'lua_surface', 'landing', 'source']
for (const h of hooks) {
  for (const f of requiredFields) {
    if (!h[f] || (typeof h[f] === 'object' && !Object.keys(h[f]).length)) bad(`hook ${h.name}: empty field ${f}`)
  }
  if (!['framework', 'event'].includes(h.family)) bad(`hook ${h.name}: unknown family ${h.family}`)
  const { states, edges } = h.landing
  if (!states.length && !edges.length && !h.landing.barrier && !h.landing.phase && !h.landing.layer) {
    bad(`hook ${h.name}: no landing point at all`)
  }
  for (const s of states) if (!oracleStates.has(s)) bad(`hook ${h.name}: landing state ${s} is not an oracle state`)
  for (const e of edges) {
    const parts = e.split(' -> ').map((s) => s.trim())
    if (parts.length !== 2 || !parts.every((p) => oracleStates.has(p))) {
      bad(`hook ${h.name}: landing edge "${e}" is not an oracle edge`)
    }
  }
}

// Forward: every contract-declared hook must be registered.
for (const c of ['avatar-reconnect-cutover', 'cross-server-lifecycle']) {
  const contract = JSON.parse(read(`contracts/${c}.json`))
  const declared = contract.event_hooks ? Object.keys(contract.event_hooks) : []
  for (const name of declared) {
    if (!names.includes(name)) bad(`${c} declares hook ${name}: not in the registry`)
  }
}

// Reverse: every hook-shaped token in the scan set must resolve into the
// registry (no unregistered hook anywhere the surface is defined).
const hookToken = /\b(?:avatar_[a-z_]+|on[A-Z][A-Za-z0-9]*)/g
const registered = new Set(names)
for (const f of scanSet) {
  const text = read(f)
  for (const match of text.matchAll(hookToken)) {
    if (!registered.has(match[0])) {
      bad(`${f}: hook-shaped token ${match[0]} is not in the registry (orphan)`)
    }
  }
}

// The nine framework hooks the oracle's hook-surface table fixes must all be present.
const frameworkCount = hooks.filter((h) => h.family === 'framework').length
for (const n of ['avatar_load', 'avatar_enter', 'avatar_leave', 'avatar_tick', 'avatar_save',
  'avatar_transfer_out', 'avatar_transfer_in', 'avatar_detach', 'avatar_fail']) {
  if (!hooks.some((h) => h.name === n && h.family === 'framework')) bad(`framework hook ${n} missing from the registry`)
}

if (problems.length) {
  for (const p of problems) console.error(`FAIL hook-registry: ${p}`)
  console.error(`FAIL hook-registry: ${problems.length} problem(s)`)
  process.exit(1)
}
console.log(`OK: hook registry consistent: ${hooks.length} hooks (${frameworkCount} framework, ${hooks.length - frameworkCount} event), no orphans, all landings inside the oracle's ${oracleStates.size} states`)
