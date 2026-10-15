# src/core/observability — logging infrastructure

`port_log.{c,h}` — the process-wide logger. Locked across platforms
(Interlocked/libnx Mutex/pthread), JSONL mirror via `SSB64_LOG_JSON`,
sinks for UI capture. Leaf module: nothing here may include product
code — decomp sources call `port_log()` directly.

Crash bundle capture (`crash_bundle`), offline symbolization
(`port_symbolize`) and RenderDoc integration (`renderdoc_*`). Output
artifacts go under `out/` — never the source tree.

## Allowed dependencies

`platform`, `observability`, LUS. Must not import `features`,
`netplay`, `distribution`.

## Tests

`tests/unit/diagnostics/` (planned); crash path exercised by
`tests/e2e/` rig failure captures.
