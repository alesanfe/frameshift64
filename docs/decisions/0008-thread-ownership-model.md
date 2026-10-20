# ADR-0008: Thread ownership model — explicit queues, no arbitrary cross-thread callbacks

## Status

Accepted (2026-10) — ownership model adopted for new code; existing
transports migrate as they are touched (websocket backends already
follow it).

## Context

The runtime combines at least five distinct execution contexts:

- the **main thread** — game simulation, scene lifecycle, ImGui,
  presentation coordination;
- **audio callbacks** — real-time-safe mixer work only;
- **network transport workers** — curl/WinHTTP/fallback receive loops
  in `src/networking/netplay/transport/`;
- **N64 virtual threads** — libultra scheduler semantics replayed on
  top (`osSendMesg`/`osRecvMesg` message queues, virtual priorities);
- **asset/update workers** — decode, install, hash jobs.

Historically these communicated through whichever mechanism was local:
direct calls into shared state, ad-hoc flags, callbacks fired from
worker threads into code that assumed the main thread. That class of
bug is the hardest to reproduce: use-after-free on shutdown, callbacks
fired after owner destruction, ImGui touched off-main, races visible
only under TSan or on slower machines.

## Decision

1. **Every thread has a named owner.** A worker's lifetime belongs to
   one object; when the owner dies the worker is joined or detached
   *before* the owner's state is torn down. "Fire and forget" threads
   that outlive their creator are not allowed.

2. **Cross-thread communication is a queue, not a call.** The only
   legal way to reach another context is posting an immutable event to
   its queue:

   - `MainThreadDispatcher` — run-on-main work items (the sink for
     transport events, async load completions, deferred UI);
   - transport-internal queues — ingress frames, uplink state
     transitions (already the shape used by `port_ws_uplink_*`);
   - the N64 message-queue emulation — decomp `OSMesgQueue` semantics,
     unchanged, but always consumed on the main thread's scheduler;
   - shutdown drain — a queue processed during teardown so late events
     are dropped deterministically rather than racing a dying owner.

3. **Callbacks never cross threads.** A worker thread may only write
   to its own state plus its outbound queue. Invoking a main-thread
   function pointer from a worker is a review-blocker bug. If an API
   needs async completion, it takes a queue/dispatcher reference, not a
   `void(*)(void*)`.

4. **Real-time contexts stay real-time.** The audio callback may not
   lock mutexes the main thread holds for unbounded time, allocate, or
   log; it consumes pre-produced command buffers only.

5. **Shutdown is ordered.** Producer stops → queue drains → worker
   joins → owner frees. Any event arriving after drain is discarded
   with a counted diagnostic, never dispatched.

## Consequences

- New transport or worker code must name its owner thread and its
  queues at the top of the file; reviewers reject implicit contexts.
- The netplay websocket backends already match this shape (uplink
  state machine + ingress queue consumed on main); they are the
  reference implementation.
- TSan runs (CI `sanitizers.yml`) become the executable check: a data
  race under the churn tests means this ADR was violated somewhere.
- The N64 virtual-thread semantics are preserved — libultra message
  queues are the compatibility contract — but their *physical*
  execution stays pinned to the owning context.
- Deferred, not solved: a unified `GameTaskQueue` for asset workers
  and the frame-pipeline refactor (ADR candidate once the phases in
  `application/gameloop.cpp` stabilize).
