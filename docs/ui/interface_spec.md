# Interface spec — surfaces, features, navigation

This document is the single definition of BattleShip's user-facing
interface: what each surface is, what it does, and how users move
through it. Two surfaces exist, with deliberately different contracts:

- **C++ ImGui UI** — the in-game menu, overlays, and prompts
  (`src/presentation/`). Full-featured; this is the product's UI.
- **HTML** — two small, single-purpose pages: the **web loader shell**
  (`platforms/web/shell.html`, the Emscripten port's launch surface) and
  the **lobby portal** (`services/netplay-lobby/.../board_page.mjs`, the
  netplay service's public read-only dashboard).

Both obey the same product rules: state is never carried by color alone
(every status has an icon + text), technical data renders in mono type,
and async work always shows an explicit state — never an unlabeled
spinner or a silent jump.

---

## Part 1 — C++ ImGui UI (`src/presentation/`)

### 1.1 Layers (dependency flows strictly downward)

| Layer | Contents | Rule |
|---|---|---|
| Tokens | `DesignTokens.h` — `Role`, `TypeScale`, `kSpace*`, `kRadius*`, theme + colorblind palettes | Screens name roles, never colors |
| Views | `ui/UiKit.*` — `Card`, `StatusPill`, `StepList`, `StatBlock`, `KeyValueRow`, `EmptyState`, `Breadcrumb`, `ImpactChip`, `IconBadge`; `UIWidgets*` widgets; `Menu*` frame | Only layer that calls ImGui; state in, intents out |
| ViewModels | `*ViewModel.*` — `OnlineStatus`, `Diagnostics`, `Extraction`, `Mods`, `Replays`, `Updates` | Own all port-ABI reads + commands; emit plain `*ViewState` snapshots per frame |
| Framework | `framework/` — `AsyncOperation` (ticketed lifecycle + `AsyncBackgroundPolicy`), `UiDispatcher` (worker→main mailbox), `IViewModel`, `ModalService`, `ToastService`, `SettingsRepository`, `PendingSettingsTransaction`, `UiError`, `NavigationService` | The cross-thread + lifecycle seam; no ImGui |
| Domain | `port_*` ABI, `netplay_snapshot` (one session walk per frame), CVars | Authoritative state — UI never mutates it outside commands |

### 1.2 Information architecture — the Esc menu

The menu is a full-window surface over a scrim. Top level: 7 sections,
each a row of sidebars; each sidebar is one screen of widgets.
Persisted route = `section/sidebar` CVars.

| Section | Sidebars | Purpose |
|---|---|---|
| **Play** | Jump to Screen, Records, Replays | Scene jumps, personal stats, replay browser |
| **Game Files** | Extraction, Paths | ROM/asset archive state, re-extraction, file locations |
| **Customization** | Script Mods, Shaders, Texture Packs | Mod inventory + enable/disable, shader packs, texture packs |
| **Online** | Netplay | Room host/join, player list, ready states, match start |
| **Settings** | General (2-col), Display, Image Quality, Audio, Input Mappings, Interface, Accessibility, Classic Mode, Match Rules, Practice, Control Enhancements, Cheats, Advanced, Developer, Modified | All persistent configuration; `Modified` is a generated audit page of non-default values |
| **Support** | Diagnostics, Actions, Updates | Health card + bundle export, utilities (rescan/restore), updater channel + progress |
| **About** | Build | Version, build SHA, credits |

### 1.3 Navigation model

- **Route** = `section/sidebar` (+ optional widget anchor). Written to
  `Menu.*SidebarSection` CVars — the persisted truth, survives restarts.
- **NavigationService** (`MenuNavigation()`) owns history: `NavigateTo`
  pushes the departed route onto a back stack and queues a jump through
  the existing `Ship::navigate*` sink; `GoBack`/`GoForward` replay it.
  `SyncCurrent` observes every landed route so clicks are recorded too.
  Browser semantics: a new navigation clears the forward stack.
- **Jump producers**: the search field ("Go to" buttons) and the
  `Modified` audit page (per-setting "Go") both emit `NavigateTo`.
- **Breadcrumb** renders `Settings › Display` under the section header;
  the active sidebar gets an accent bar/underline — selection is never
  fill-color-only.
- **Simple mode** collapses Settings to a flat curated list; the same
  routes still apply.

### 1.4 Overlays — orthogonal to routes

Drawn over the game regardless of menu state; driven by ViewModel mode,
not by navigation:

| Surface | Trigger | Content |
|---|---|---|
| `OnlineStatusWindow` | `OnlineStatusViewState.mode` | Desynced: warning + Return-to-room/Leave. AfterMatch: roster, ping, Rematch (host) / Leave |
| `UpdatePrompt` | UpdatesViewModel state | Update available → download progress → restart affordance |
| `Toasts` | `ToastService` queue | Transient success/info/error, non-blocking |
| Modals | `ModalService` | Confirmations (destructive actions, pending-changes exit) |
| Narration | `narration.cpp` | Screen-reader speech for menu changes |

### 1.5 Async contract

Every long-running screen operation (downloads, extraction, lobby
probes) is an `AsyncOperation<T>`: `Idle → Pending → {Succeeded | Failed
| Cancelled}`, with a generation ticket so stale worker replies are
rejected. Worker results cross to the UI only through
`MainThreadDispatcher().Drain()` — once per frame on the main thread,
never mid-Draw. `AsyncBackgroundPolicy` declares what happens when the
owning screen hides (cancel / continue / continue+toast / block).

---

## Part 2 — HTML surfaces

### 2.1 Web loader shell (`platforms/web/shell.html`)

One page, one job: get the wasm game booted with its assets. It is a
state machine, not a site — no routes, no back stack.

**States (linear flow):**

```
boot → (no assets) → ROM pick → extract (torch.wasm) → start → canvas
                     │                │                   │
                     └─ error ────────┴── fail(code,msg) ─┘
```

- **Boot**: loads `BattleShip.js` + restores `/persist` (IndexedDB).
  Shows status line only.
- **ROM pick**: file picker + drag-drop anywhere + the "never uploaded"
  privacy note. `describeRom()` sniffs the header (format, title,
  country, CRC) so the user sees *what* ROM before extraction.
- **Extract**: torch.wasm steps on the MEMFS staged recipe tree,
  cancelable (`extractCancel`).
- **Error**: `fail(code, msg)` → typed code + copyable diagnostics
  bundle + safe-mode and single-thread-build links. Never a dead page.

**Always-available chrome** (independent of state):

- **Gear button** → storage panel: usage/quota bar, persistence request,
  save/settings **export & import** (plain JSON bundle), "clear game
  assets (keep saves)", diagnostics copy.
- **SW update toast**: service worker found an update → "Restart to
  apply" (user-gated, never auto).
- **Touch overlay** (touch devices / `?touch=`): floating analog stick
  (WASD, 8-way, 50 ms press floor), A/B/Z/R/L/Start buttons, C cluster,
  menu hamburger → `__ssb64_touch.menu` edge consumed by
  `port_web_touch_poll()` on the main thread; auto-hidden while a
  gamepad is connected or the ImGui menu is open.
- **Virtual keyboard proxy** for text input fields (SDL-Emscripten has
  no `StartTextInput`).
- **WebGL context-loss** handler: `preventDefault` + offer reload —
  required or the canvas dies silently.

**URL contract** (query params are the shell's only "routes"):

| Param | Effect |
|---|---|
| `autostart=1` | Skip the start button once assets exist |
| `lobby_host=URL` / `lobby_join=URL&lobby_code=CODE` | Straight into a netplay room |
| `SSB64_LOBBY_FORCE_RELAY=1` | Relay-only path (no UDP attempt) |
| `SSB64_SAFE_MODE=1` | Skip mods & texture packs this run |
| `st` / `BattleShipST.html` | Single-thread build variant |
| `touch=0\|1` | Force touch overlay off/on |

### 2.2 Lobby portal (`board_page.mjs`, served at `GET /`)

Public, read-only dashboard of the netplay service. Single template
literal, no build step, no assets — it fetches the service's own JSON
endpoints (`/stats`, `/leaderboard`, `/history`, `/tournament`,
`/replays`) from the browser, so it can never leak internals and works
identically on the Worker and node-lobby adapters.

**Sections**: Service stats · Leaderboard (rank, rating, W/L,
moderation flags) · Recent verified matches · Tournaments (size,
status, winner) · Shared replays.

Navigation is deliberately absent: one page, CSS-grid cards, re-fetched
every 30 s. Replay IDs link to `/replay/<id>` (the raw blob). A future
detail view would extend the JSON API rather than add SPA routing — the
page stays below the size where a router pays for itself.

---

## Part 3 — Cross-surface rules

1. **Roles before colors.** C++: `Role` enum → theme/colorblind
   palette. HTML: `.flag`/`status` classes carry icon + text, not just
   hue.
2. **Mono for technical data** (hashes, paths, codes, IDs) — C++
   `KeyValueRow(..., mono)` and the shell's `ui-monospace` classes.
3. **Explicit async states.** C++ `AsyncOperation`; JS: the shell's
   `setStatus`/`fail` states. No unlabeled spinners, no silent jumps.
4. **One snapshot per frame/request.** C++ ViewModels capture state
   once per Draw (shared `netplay_snapshot` walk); the portal renders
   once per fetch. No mid-render re-reads.
5. **Errors are typed + copyable.** C++ `UiError` diagnostic codes;
   shell `fail(code, …)` + diagnostics bundle. Never a dead screen.
6. **i18n**: C++ strings go through `TR()` (pt/es/de catalogs exist).
   HTML surfaces are English-only today; the portal's table labels are
   the first strings to externalize if localization is added there.
7. **Keep game state authoritative.** UI (either surface) reads
   snapshots and issues commands; it never mutates the session, the
   simulation, or settings stores directly.
