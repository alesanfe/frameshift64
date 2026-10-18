# Remote governance

This repository works against several GitHub remotes. They are **not**
interchangeable — pushes to the wrong one either fail or publish work to a
stale fork.

## Roles

| Remote name | URL | Role | Push? |
|---|---|---|---|
| `frameshift` | `github.com/alesanfe/frameshift64` | **Canonical.** `main` holds the port; component branches hold submodule contents (`decomp`, `libultraship`, `ssb64` = Torch). | yes (alesanfe) |
| `origin` | `github.com/fatty8me2/smash-classic` | Legacy fork of record. Read-only for alesanfe — pushes 403. | no |
| `fork` | same URL as `origin` | Duplicate alias kept for historical scripts. Prefer `origin`. | no |
| `upstream` | `github.com/JRickey/BattleShip` | Third-party original project this work derives from. Never push. | **never** |

`main` tracks `frameshift/main`. A plain `git push` lands on the canonical
remote — that is intended.

## Why a monorepo remote for submodules

`.gitmodules` points all three components at `frameshift64` branches:

- `components/decomp` → `frameshift64` branch `decomp`
- `components/libultraship` → `frameshift64` branch `libultraship`
- `components/game-runtime` → `frameshift64` branch `ssb64`

The SHAs recorded in the parent's gitlinks must be reachable from *that*
URL or `git submodule update` fails on fresh clones. The legacy per-project
forks (`fatty8me2/ssb-decomp-re`, `JRickey/libultraship`, `JRickey/Torch`)
do not contain our SHAs and alesanfe cannot write to them.

## Rules

1. **Never `git push upstream`** — it is the upstream project's repository.
2. After committing inside a submodule, push it to frameshift64 before
   pushing the parent pointer bump:
   `git -C components/<sm> push https://github.com/alesanfe/frameshift64.git HEAD:refs/heads/<branch>`
   (decomp → `decomp`, libultraship → `libultraship`, game-runtime → `ssb64`)
3. Local checkouts may keep stale `origin` URLs inside submodules — the
   worktree script resets them; `tools/analysis/lint/check_remotes.py`
   reports drift but does not rewrite configs.
4. `git submodule status` must show no `+`/`-` prefix before pushing
   `main` — a `+` means the checkout moved past the recorded gitlink.

## Validation

```
python tools/analysis/lint/check_remotes.py          # read-only audit
python tools/analysis/lint/check_remotes.py --probe  # + ls-remote reachability
```

`--probe` performs network `ls-remote` against every declared remote and
verifies each recorded submodule SHA is advertised on a branch.

## The 403 case (fatty8me2 fork)

Authenticated as `alesanfe`, writes to `fatty8me2/*` and `JRickey/*` return
403. To make the legacy fork updatable, the fork owner must add `alesanfe`
as a collaborator — or pushes must run under credentials that own it.
Until then `origin/main` will lag; do not treat it as a source of truth.
