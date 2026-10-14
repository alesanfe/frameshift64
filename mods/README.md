# mods/

Mod SDK and content for the BattleShip mod system (TCC-compiled C hooks).

| Dir | Contents |
|-----|----------|
| `sdk/` | Public headers + cmake helpers consumed by mod projects (WIP — see `docs/modding/modding.md`). |
| `examples/` | Working example mods (`hooktest`, `playertint`) — was `workspace/`. Each has its own `CMakeLists.txt`. |
| `templates/` | `basic-mod/` — copy this to start a new mod (was `workspace/template/`). |
| `tests/` | Mod-loading checks. |

The in-engine runtime (hook manager, registry, symbol resolver, mod bridge) lives in `src/bridge/modding/`; the event-dispatch hook list in `src/bridge/hooks/`.

**Note:** the repo root `mods/` dir is also where HiResPack scans for user texture packs on dev launches — everything except `sdk/`, `examples/`, `templates/`, `tests/` and this file is gitignored (see `.gitignore`).
