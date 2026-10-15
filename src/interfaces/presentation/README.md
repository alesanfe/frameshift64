# src/interfaces/presentation — ImGui menu layer

The port's UI: `Menu`/`PortMenu`/`MenuTypes`, `UIWidgets`,
`OnlineStatus`, `UpdatePrompt`, `native_dialog` (OS file pickers) and
`Compat.h`. Everything the player sees outside gameplay rendering.

## Allowed dependencies

`features` (it toggles them), `distribution` (update prompts),
`netplay` (lobby status), `platform`, `observability`, `config`.

## Forbidden dependencies

decomp internals, `integration` — presentation talks to features and
systems through their public headers only.

## Tests

Menu behavior is exercised manually and by e2e rigs; widget-level
checks belong in `tests/unit/presentation/` (planned).
