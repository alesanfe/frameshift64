# Post-audit verification — October 2026

Reproducible record of the stabilization pass that followed the large
refactor series. Captures exact commit, submodule SHAs, toolchain,
commands, results, and known limitations so every green result can be
re-derived from a clean tree.

## Verified state

| Item | Value |
|---|---|
| Verified commit | `de5cd5ce2` (published refactor tip at audit time) |
| Follow-up commits | `42f3fc36a` (portable leak/TLS tests), `5f015c0b9` (sanitizer CI), `07fff8ad4` (complexity-checker tests) |
| `components/decomp` | `9a8884c` |
| `components/game-runtime` | `d70f360` |
| `components/libultraship` | `6afb461` |
| OS | Windows 11 (MSVC build), WSL2 Ubuntu 22.04 (gcc 11.4 runs) |
| Toolchain | VS 2022 Build Tools, Ninja, CMake ≥3.26, Python 3.11, Node, OpenSSL 3.x, libcurl 8.23.0 (websockets+ssl) via vcpkg |

Safety references: `backup/post-audit-2026-10-20` branch;
uncommitted work preserved under `out/audit-capture/` (worktree.patch,
per-submodule patches, git-state.txt).

## Clean-room procedure

The evidence build was produced in an independent worktree at
`.claude/worktrees/clean-check`, not in the dirty main tree:

```bash
git worktree add .claude/worktrees/clean-check de5cd5ce2
cd .claude/worktrees/clean-check
git submodule update --init --recursive   # pinned gitlinks, no dirty content
cmake -B build -DNON_PORTABLE=ON -DSSB64_BUILD_TESTING=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

`git status`/`git submodule foreach 'git status'` inside the worktree
were empty before the build — the results below do not depend on the
uncommitted changes present in the main checkout.

## Results

| Check | Result |
|---|---|
| `BattleShip.exe` link | OK — 962 ninja edges, exit 0 |
| Torch asset extraction | OK — `BattleShip.o2r` produced (~88 s) |
| `ctest -L netplay` | 12/12 PASS (incl. ws_win32/curl/fallback leak tests) |
| `check_complexity.py --report` | 0 violations, baseline empty |
| `git show --check de5cd5ce2` | clean except one blank-line-EOF in `complexity-baseline.txt` |
| Binary/NUL scan of commit (125 files) | 0 binary files, 0 NUL bytes |
| Restored symbols in clean `.obj`s | `gwatchArmAll` (external), `gwatchArmThread` (anon-ns), `DRP_Shutdown`, `port_profile_save` (extern "C", unmangled) |
| Damaged-file audit | `frame_interpolation.cpp`, `ShaderDownloader.cpp`, gbi tools, `port_init_phases.cpp`, `profiles.cpp`, `crash_handler.cpp`, `heap_check_gwatch.cpp`, `heap_watchpoint.cpp`, `Menu_frame.cpp` — no truncation, no conflict markers, no BOM/invalid UTF-8 (decorative `===` separators in gbi tools are intentional) |

Additional evidence collected earlier on the same commit family (WSL,
gcc 11.4): fake-backend + real-fallback-backend TSan runs (no races),
ASan+UBSan fallback churn (553 checks), curl wss:// loopback with a
self-signed CA via mount namespace (1053 checks), WinHTTP handle-leak
run (883 checks, small warm-up noise documented in
`out/verification-de5cd5ce2.md`).

## Ongoing at audit time

- `src/networking/netplay/session/netlobby_protocol.cpp` split —
  in progress in the main tree (`netlobby_server.cpp`,
  `netlobby_internal.h`); intentionally not touched by this pass.
  The uncommitted `netlobby.cpp` state triggers one live ratchet
  hit (`port_netlobby_update` CCN=19 > 15); committed state is clean.
- Submodule dirty diffs (decomp 296 / game-runtime 125 / libultraship
  317 lines) belong to parallel work — preserved under
  `out/audit-capture/`, not incorporated.
- `services/netplay-lobby/tmp_probe/` — untracked probe output.

## Known limitations

- The wss:// leak test needs libcurl ≥7.86; where the toolchain curl is
  older (e.g. distro libcurl 7.81 on Ubuntu 22.04) the test is not
  registered — the vcpkg-based Windows/Linux jobs cover it.
- TLS trust on Linux tests is staged via a user+mount namespace
  (unshare). Where `unshare`/openssl are unavailable the test exits 77
  (SKIP), never a false pass.
- WinHTTP/Schannel has no non-interactive trust override, so the TLS
  variant is Linux-only by design.
- Emscripten and Android builds exist as CI jobs but a full wasm link
  of the env bridge and an arm64 device smoke test are still pending.
- Sanitizer coverage is scoped to the netplay transport subset; the
  decomp TUs are not sanitizer-clean upstream.

## Re-verification

To re-derive every row above: repeat the clean-room procedure at the
listed commit + submodule SHAs. Expected outcome is byte-identical for
the test matrix; build times vary. The `.o2r` byte-reproducibility
check (item 57/58) is still open and is the next gate before release.
