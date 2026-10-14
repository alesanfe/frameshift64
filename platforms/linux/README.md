# Linux

| Piece | Location |
|-------|----------|
| CMake | `cmake/platforms/Linux.cmake` + presets |
| Packaging | `scripts/packaging/package-linux.sh` (AppImage) |
| CI | `.github/workflows/{build,release}.yml` |

Downloads/updates use the curl-child `download_service`; SHA-256 is
in-process (`src/distribution/FileHash.cpp`) — no `sha256sum` needed.
