# UWP (Xbox / Windows Store)

| Piece | Location |
|-------|----------|
| Project wrapper | `platforms/uwp/` (here) |
| CI | `.github/workflows/build-uwp.yml` |
| Feature gating | `BATTLESHIP_UWP` + `SSB64_SRC_PORT` exclusions in `CMakeLists.txt` |

No in-app updater, no downloaders, no modding (MSIX model + no exec
memory); shaders ship packaged.
