# Error model

One taxonomy, three channels. Before this, each module reported failure
its own way (`bool`, `nullptr`, errno ints, log-and-continue, `abort`,
`assert`) — the categories in `src/shared/result/error_code.h` are the
vocabulary every module now converges on.

## The contract

| Channel | Where | Mechanism |
|---|---|---|
| C ABI (`port_*`) | decomp ↔ port boundary | `int` 0/negative + `PortErrorCategory` where the signature allows |
| C++ module APIs | inside `src/` | `ssb64::Result<T>` / `ssb64::Status` |
| User surface | presentation | Toasts + menu text built from `Error.category` + `message` |

Rules:

1. **No exceptions across the `port_*` ABI or module boundaries.**
   `Result` is the error channel; exceptions stay inside a TU.
2. **Decomp-facing C keeps `int`/`NULL` conventions.** Translate at the
   edge — don't push `Result` into `.c` files or the decomp.
3. **`message` is for humans and logs.** Programmatic decisions branch on
   `category` (+ `code`), never on the message text.
4. **`PORT_ERR_INTERNAL` means bug.** It logs + bundles a crash report —
   it is never toasted as "user error" and never retried.

## Disposition policy

| Category | Toast | Log | Crash bundle | Recoverable | Safe mode |
|---|---|---|---|---|---|
| `CONFIG` | yes (fixable) | yes | no | yes | if corrupt boot cfg |
| `FILESYSTEM` | yes | yes | on repeated failure | yes | no |
| `ASSET` | yes (first-run hint) | yes | no | yes (re-extract) | no |
| `ROM_VALIDATION` | yes (wizard) | yes | no | no — new dump | no |
| `NETWORK` | on user action | yes | no | yes (retry) | no |
| `PROTOCOL` | on session | yes + remote peer | desync dump | partial | no |
| `COMPATIBILITY` | yes (version note) | yes | no | partial | no |
| `UPDATE` | silent→toast on manual check | yes | no | yes | no |
| `PLATFORM` | yes | yes | context captured | yes | possible |
| `INTERNAL` | no | yes | **always** | no | on next launch |

Sensitive-content rule: `message` and crash bundles must never contain
ROM paths beyond the basename, room codes, file contents, or absolute
paths outside the install dir. `netlobby` room codes are secrets —
never log them.

## Migration

Adoption is incremental — a module converts its public API to `Result`
when it's touched for another reason. New code must use it; do not add
new `bool`-as-status signatures. `PORT_ERR_INTERNAL` sites replace the
old `assert`-and-continue pattern: invariants that fire in release
builds produce a crash bundle instead of silent corruption.

`check_module_deps.py` already gates include directions; an error-model
lint (flagging `abort()`/`exit()` outside `src/application/` and
`src/platform/compatibility/`) is on the hardening backlog.
