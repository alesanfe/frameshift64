# src/diagnostics — crash and debug instrumentation

Crash bundle capture (`crash_bundle`), offline symbolization
(`port_symbolize`) and RenderDoc integration (`renderdoc_*`). Output
artifacts go under `out/` — never the source tree.

## Allowed dependencies

`platform`, `observability`, LUS. Must not import `features`,
`netplay`, `distribution`.

## Tests

`tests/unit/diagnostics/` (planned); crash path exercised by
`tests/e2e/` rig failure captures.
