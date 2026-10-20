# ADR-0007: Determinism classes for systems, CVars, and mods

## Status

Accepted (2026-10) — taxonomy adopted; enforcement is phased in
incrementally, not retrofitted in one pass.

## Context

Rollback netplay and golden replays depend on a property the codebase
previously carried only as convention: some state is *deterministic
simulation* (must be identical on every peer and every replay) and the
rest is *local presentation* (may diverge freely). The decomp's original
globals mix both — and the port has been adding systems
(interpolation, toasts, telemetry, cosmetics, CVars, mods) on both sides
of that invisible line. The result is an audit-by-memory model: every
sync-hash, replay tape, and netplay feature has to "just know" which
reads and writes are safe.

Concretely dangerous examples:

- a purely visual system that touches a decomp global contaminates the
  sync hash;
- a simulation system that reads wall-clock or local config diverges
  peers silently;
- a CVar that is cosmetic on one page and gameplay-affecting on another
  cannot be reasoned about from its name;
- a mod has no way to declare whether it is legal to enable in netplay.

## Decision

Every system, CVar, mod capability, and hash input is classified into
one of four determinism classes:

| Class | Meaning | Rules |
|---|---|---|
| `Simulation` | Inputs to game state that affect outcomes | No wall clock, no sockets, no filesystem, no unsynchronized RNG, no pointer-derived values. All reads/writes are replay-recorded or hash-covered. |
| `DerivedPresentation` | State computed *from* simulation state | May read `Simulation` state freely; must never write it. Not hash-covered — two peers may compute different frames. |
| `LocalOnly` | Per-machine preference/cosmetic state | Never read by `Simulation`. May use wall clock, RNG, filesystem, OS APIs freely. |
| `ExternalSideEffect` | I/O boundary (network send, disk write, updater, telemetry) | Runs outside the simulation step; must be idempotent w.r.t. replay — re-simulating a frame must not re-emit the effect. |

Application rules:

1. **CVars** — `src/config/schema.h` gains a determinism field per
   entry (default `LocalOnly`). A `Simulation`-class CVar must be
   negotiated in the netplay handshake; a `LocalOnly` CVar must never
   be read by simulation code — that is the enforceable half of the
   taxonomy.
2. **Mods** — capabilities are declared: a mod that hooks simulation
   requires the same mod set on all peers; presentation-only mods are
   unrestricted. The manifest schema records this.
3. **Sync hash** — the rollback hash covers exactly `Simulation` state.
   New systems declare their class before they may write game globals.
4. **Wall clock** — `DerivedPresentation` and below may read real time;
   `Simulation` reads the frame counter only.
5. **Tests** — golden replay traces remain the executable check: a
   `LocalOnly` change must produce byte-identical `.trace` output;
   a `Simulation` change is expected to differ and needs a regen note.

## Consequences

- The four classes are a *vocabulary* first: code review, CVar schema,
  mod manifest, and the sync-hash boundary all name the same thing.
- Enforcement lands incrementally: schema field → handshake
  negotiation → mod manifest flag → lizard/lint rules for direct I/O in
  simulation files. No big-bang refactor.
- Replays and crash bundles already stamp `PORT_ABI_CURRENT`; a future
  MINOR bump may add a determinism-manifest hash so peers can refuse
  divergent configuration sets instead of desyncing.
- This ADR does **not** reclassify existing globals en masse — each
  subsystem adopts its class when touched, starting with the netplay
  and interpolation paths where the boundary matters most.
