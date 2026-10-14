# src/application — process composition and lifecycle

Owns the top of the port: bootstrap (`port.cpp`), the main loop
(`gameloop`), first-run asset extraction (`first_run`), window focus
handling (`focus`), the watchdog (`port_watchdog`) and path resolution
(`app_paths`).

## Allowed dependencies

Every module — application is the composition root, it may import
`presentation`, `features`, `netplay`, `integration`, `resources`,
`platform`, `observability`, `diagnostics`, `distribution`.

## Forbidden dependencies

Nothing below the port boundary is *forbidden* here, but this module
must not contain feature logic — it wires modules, it does not
implement them.

## Tests

`tests/e2e/first_run/`, `tests/harness/headless/` — the binary itself
is the test rig (`AUTOVS`/`MAX_FRAMES`/`RIG_EXIT` env hooks).
