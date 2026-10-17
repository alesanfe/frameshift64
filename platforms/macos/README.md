# macOS

| Piece | Location |
|-------|----------|
| CMake | `platforms/macos/cmake/macOS.cmake` + presets |
| Packaging | `packaging/macos/package-macos.sh` (incl. Liquid Glass icon) |
| Entitlements | `cmake/macos_entitlements.plist`, `cmake/macho_text_rwx.py` |

Text+exec sections need the `macho_text_rwx` post-link step for
TCC-compiled mods.

## Cross-compiling from Linux (osxcross)

Compile+link verification of the arm64 Mach-O is possible without Apple
hardware. Verified 2026-10-17: full `BattleShip` arm64 binary (28 MB) +
`BattleShip.o2r` + `f3d.o2r` from a `ubuntu:24.04` Docker container.

### Setup

```bash
# container deps
apt install -y clang cmake ninja-build git curl patch cpio xz-utils \
    libssl-dev liblzma-dev libxml2-dev libzstd-dev libbz2-dev zlib1g-dev \
    python3 python3-pil python3-yaml clang-tools-18

# toolchain
git clone https://github.com/tpoechtrager/osxcross /opt/osxcross
curl -L -o /opt/osxcross/tarballs/MacOSX14.5.sdk.tar.xz \
    https://github.com/joseluisq/macosx-sdks/releases/download/14.5/MacOSX14.5.sdk.tar.xz
cd /opt/osxcross && UNATTENDED=1 TARGET_DIR=/opt/osxcross-target ./build.sh

# deps (arm64 binary ports: libsdl2, libzip, spdlog, nlohmann-json,
# tinyxml2, glew — pulled as .tbz2, not source builds)
export OSXCROSS_TARGET_DIR=/opt/osxcross-target \
       MACOSX_DEPLOYMENT_TARGET=14.0 \
       PATH=/opt/osxcross-target/bin:$PATH
echo 1 | /opt/osxcross/tools/osxcross-macports install \
    libsdl2 libzip spdlog nlohmann-json tinyxml2 glew
ln -sfn /opt/osxcross-target/macports/pkgs/opt/local /opt/local

# C++20 modules scanning (yaml-cpp): CMake looks for clang-scan-deps
# next to the wrapper compilers
ln -sf /usr/lib/llvm-18/bin/clang-scan-deps /opt/osxcross-target/bin/clang-scan-deps

# adhoc codesign is a no-op under osxcross (post-build step calls it)
printf '#!/bin/sh\nexit 0\n' > /usr/local/bin/codesign && chmod +x /usr/local/bin/codesign
```

### Gotchas hit while bringing this up

- **Torch is a host tool.** `ExternalProject_Add` does not forward the
  toolchain file, but `CC`/`CXX`/`OSXCROSS_*` env vars leak into its
  configure and produce an arm64 binary that cannot run for extraction.
  Fix: wipe `out/build/macos/TorchExternal/src/TorchExternal-build`,
  reconfigure it with `env -i` (no osxcross env), same `CMAKE_ARGS` as the
  parent (`-DUSE_STANDALONE=ON -DBUILD_SSB64=ON -DBUILD_NAUDIO=ON` — the
  SSB64 flag is what registers `SSB64:RELOC`), build once, then let the
  parent build consume it.
- **libtcc config.h.** `common.cmake` only runs tinycc's `./configure`
  when `config.h` is absent, and it autodetects the Linux host →
  `TCC_TARGET_X86_64` → `tcc_relocate` stays behind `TCC_IS_NATIVE` and
  the link fails. Regenerate once:
  `cd out/build/macos/_deps/tinycc-src && ./configure --cpu=aarch64 --targetos=Darwin`.
- **macports abs paths.** Ports carry baked `/opt/local` paths — the
  symlink above is required, not optional.
- **fmt lives at `lib/libfmt12/cmake/fmt`** (nonstandard layout) — pass
  `fmt_DIR` explicitly or spdlog's `find_dependency(fmt)` breaks.

### Configure + build

```bash
arm64-apple-darwin23.5-cmake -B out/build/macos -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DNON_PORTABLE=ON \
    -DCMAKE_PREFIX_PATH=/opt/local \
    -Dfmt_DIR=/opt/local/lib/libfmt12/cmake/fmt \
    -DPython3_EXECUTABLE=/usr/bin/python3
arm64-apple-darwin23.5-cmake --build out/build/macos -j4
```

Output: `out/build/macos/BattleShip` — `Mach-O 64-bit arm64` (thin, SDK
14.5, `arm64-apple-darwin23.5-lipo -info` confirms). Runtime validation on
real Apple hardware / `macos-14` CI is still required (Metal, entitlements,
notarization are not exercised by a cross-link).
