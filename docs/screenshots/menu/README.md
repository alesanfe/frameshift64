# Menu screenshots

Per-section captures of the in-game menu (ESC). Regenerate with:

```bash
python tools/testing/shoot_menu_sections.py
```

Requires a dev build with `SSB64_TEST_HOOKS` (the script uses
`SSB64_SCREENSHOT_FRAMES` / `SSB64_MAX_FRAMES`).

## Settings

| | |
|---|---|
| ![General](settings-general.png) | ![Display](settings-display.png) |
| General | Display |
| ![Image Quality](settings-image-quality.png) | ![Shaders](settings-shaders.png) |
| Image Quality | Shaders |
| ![Advanced](settings-advanced.png) | ![Match Rules](settings-match-rules.png) |
| Advanced | Match Rules |
| ![Classic Mode](settings-classic-mode.png) | ![Practice](settings-practice.png) |
| Classic Mode | Practice |
| ![Interface](settings-interface.png) | ![Input Mappings](settings-input-mappings.png) |
| Interface | Input Mappings |
| ![Control Enhancements](settings-control-enhancements.png) | ![Audio](settings-audio.png) |
| Control Enhancements | Audio |
| ![Accessibility](settings-accessibility.png) | ![Cheats](settings-cheats.png) |
| Accessibility | Cheats |

## Online

![Netplay lobby](online-netplay.png)

## Game Files

| | |
|---|---|
| ![Extraction](gamefiles-extraction.png) | ![Paths](gamefiles-paths.png) |
| Extraction | Paths |
| ![Texture Packs](gamefiles-texture-packs.png) | ![Script Mods](gamefiles-script-mods.png) |
| Texture Packs | Script Mods |

## Support

| | |
|---|---|
| ![Actions](support-actions.png) | ![Updates](support-updates.png) |
| Actions | Updates |

## About

![About / Build](about-build.png)

## Windows & states

| | |
|---|---|
| ![Stats](window-stats.png) | ![Console](window-console.png) |
| Stats overlay | Console |
| ![Gfx Debugger](window-gfx-debugger.png) | ![Controller Config](window-controller-config.png) |
| Gfx Debugger | Controller Config |
| ![Online Status](window-online-status.png) | ![Popout mode](menu-popout.png) |
| Online Status | Menu in popout window |
| ![Controller nav](menu-controller-nav.png) | |
| Menu with controller nav hints | |

## Not capturable automatically

- First-run wizard — requires a real ROM and a clean `%APPDATA%\BattleShip` profile; capture manually.
- Update prompt — only appears when the updater finds a newer release.
- Lobby "in room" / "in match" states — need an active netplay session.
