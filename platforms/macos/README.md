# macOS

| Piece | Location |
|-------|----------|
| CMake | `platforms/macos/cmake/macOS.cmake` + presets |
| Packaging | `packaging/macos/package-macos.sh` (incl. Liquid Glass icon) |
| Entitlements | `cmake/macos_entitlements.plist`, `cmake/macho_text_rwx.py` |

Text+exec sections need the `macho_text_rwx` post-link step for
TCC-compiled mods.
