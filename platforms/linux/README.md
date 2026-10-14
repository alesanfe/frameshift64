# Linux

| Piece | Location |
|-------|----------|
| CMake | `platforms/linux/cmake/Linux.cmake` + presets |
| Packaging | `platforms/linux/packaging/package-linux.sh` (AppImage) |
| CI | `.github/workflows/{build,release}.yml` |

Downloads/updates use the curl-child `download_service`; SHA-256 is
in-process (`src/distribution/integrity/FileHash.cpp`) — no `sha256sum` needed.
