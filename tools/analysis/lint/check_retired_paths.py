#!/usr/bin/env python3
"""check_retired_paths — the pre-reorganization layout must not come back.

Two checks, both cheap:

1. No retired top-level directory exists. The old paths must never be
   reintroduced — their replacements are listed for the error message.

2. No first-party source/build file resolves an old path. Scans #include
   directives in src/ and path literals in CMakeLists.txt + cmake/ for
   the retired prefixes. Documentation is exempt on purpose: bug
   write-ups in docs/ legitimately describe the old layout.

Exits 0 when clean, 1 listing every violation.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# retired path -> its replacement
RETIRED_DIRS = {
    "port": "src/",
    "server": "services/",
    "debug_tools": "tools/debugging/",
    "workspace": "mods/",
    "android": "platforms/android/",
    "uwp": "platforms/uwp/",
    "ci": ".github/workflows/ + scripts/ci/",
    "yamls": "assets/game/",
    "assets/configuration": "assets/{game} + config/controllers/",
    "assets/recipes": "assets/game/",
    "assets/controllers": "config/controllers/",
    "assets/custom": "assets/fonts/",
    "assets/readme-stats": "assets/branding/readme/",
    # src/ consolidation (2026-10-16): flat modules -> grouped tree
    "src/diagnostics": "src/core/observability/",
    "src/config": "src/core/config/",
    "src/persistence": "src/core/persistence/",
    "src/distribution": "src/application/update/",
    "src/integration": "src/game/{engine,modding}/",
    "src/gameplay": "src/game/gameplay/",
    "src/audio": "src/interfaces/audio/",
    "src/input": "src/interfaces/input/",
    "src/rendering": "src/interfaces/rendering/",
    "src/presentation": "src/interfaces/presentation/",
    "src/netplay": "src/networking/netplay/",
    "src/social": "src/networking/social/",
    "src/compatibility": "src/platform/compatibility/",
    # components/ consolidation (2026-10): submodules moved out of root
    "decomp": "components/decomp/",
    "libultraship": "components/libultraship/",
    "torch": "components/game-runtime/",
    # tools/ regrouping (2026-10): analysis / developer / code-generation
    "tools/lint": "tools/analysis/lint/",
    "tools/fuzz": "tools/analysis/fuzz/",
    "tools/testing": "tools/developer/testing/",
    "tools/modding": "tools/developer/modding/",
    "tools/codegen": "tools/code-generation/",
    "tools/asset-pipeline": "tools/assets/pipeline/",
    # packaging/ hoisted to root; tests/config -> tests/support/config
    "platforms/windows/packaging": "packaging/windows/",
    "platforms/linux/packaging": "packaging/linux/",
    "platforms/macos/packaging": "packaging/macos/",
    "tests/config": "tests/support/config/",
}

# path fragments that must not appear in source includes / build files
RETIRED_REF = re.compile(
    r'(port/(?:bridge|net|gui|audio|resource|interpolation|widescreen'
    r'|enhancements|mods|hooks|stubs|assets)/'
    r'|server/cloudflare'
    r'|debug_tools/'
    r'|workspace/(?:hooktest|playertint|template)'
    # post-module-split retired paths (2026-10 src/ modularization):
    r'|src/bridge/'
    r'|"bridge/(?:modding|hooks)/'
    r'|src/features/'
    r'|"features/(?:enhancements|cosmetics|audio|social|internal|gameplay)/'
    r'|src/application/update/(?:Updater|ModPackDownloader|ShaderDownloader'
    r'|FileHash|ReleaseSig)\.(?:cpp|h)'
    r'|"distribution/(?:ReleaseSig|FileHash|ModPackDownloader'
    r'|ShaderDownloader|Updater)\.h"'
    r'|src/networking/netplay/(?:netlobby|netrules|port_udp|port_ws)\.(?:cpp|h)'
    r'|src/networking/netplay/netrollback_seg\.h'
    r'|"netplay/(?:netlobby|netrules)\.h"'
    r'|src/platform/(?:coroutine_|coroutine\.h|http_download'
    r'|download_service|zip_extract|exe_dir|port_window_icon|io/)'
    # resources/ module dissolved (relocation->game/engine, loading->owner
    # modules) and generated/ isolation:
    r'|src/resources/'
    r'|"resources/(?:relocation|loading|interpolation|audio|css_icons|shaders|textures)/'
    r'|src/game/engine/decomp/ssb64_reloc_rebuild\.(?:us|jp)\.cpp'
    r'|include/reloc_data\.(?:us|jp)\.h'
    # platforms/ physical layout (per-platform cmake/packaging/resources):
    r'|cmake/platforms/'
    r'|scripts/packaging/'
    r'|src/platform/ssb64\.rc'
    # test-tree reorg (level-based tests/): the flat unit/ names and the
    # seeds/ corpus dir are retired — new paths under tests/<level>/<domain>
    r'|tests/unit/test_(?:extraction_recipes|extern_ids|vpk0|package_mod)'
    r'|tests/unit/coroutine_test\.cpp'
    r'|tests/fuzz/seeds'
    # tests support consolidation (2026-10): fixtures/harness/golden →
    # tests/support/{fixtures,harness,golden}/
    r'|tests/(?:fixtures|harness|golden)/'
    # assets/configuration/ split into recipes/ + controllers/:
    r'|assets/configuration'
    # assets/ reshuffle (2026-10-15): recipes→game, controllers→config/,
    # custom/fonts→fonts/, readme-stats→branding/readme/:
    r'|assets/(?:recipes|controllers|custom|readme-stats)(?:/|")'
    # config/analysis/ consolidation (2026-10): semgrep + static-analysis
    r'|config/(?:semgrep|static-analysis)/'
    # src/ consolidation (2026-10-16): retired flat module dirs must not
    # reappear as path literals in source includes or build files. New
    # grouped paths (src/core/config/ etc.) don't contain these fragments.
    r'|src/(?:diagnostics|distribution|integration|gameplay|observability'
    r'|persistence|compatibility|netplay|social|audio|input|rendering'
    r'|presentation)/'
    r'|src/config/'
    # tools/ + packaging/ regrouping (2026-10). The components/* submodule
    # roots aren't scanned here: <libultraship/...> include prefixes stay
    # legal (the move changed disk location, not include namespace).
    r'|tools/(?:lint|codegen|fuzz|testing|modding|asset-pipeline)/'
    r'|platforms/(?:windows|linux|macos)/packaging/'
    r'|tests/config/)'
)

SCAN_EXT = {".c", ".cpp", ".h", ".hpp", ".cmake", ".txt"}


def main() -> int:
    failures = []

    for retired, replacement in RETIRED_DIRS.items():
        if (ROOT / retired).is_dir():
            failures.append(
                f"retired directory '{retired}/' exists again — use '{replacement}'"
            )

    for base in ("src", "cmake"):
        for path in (ROOT / base).rglob("*"):
            if path.suffix not in SCAN_EXT or not path.is_file():
                continue
            for lineno, line in enumerate(
                path.read_text(encoding="utf-8", errors="replace").splitlines(), 1
            ):
                hit = RETIRED_REF.search(line)
                if hit:
                    rel = path.relative_to(ROOT)
                    failures.append(f"{rel}:{lineno}: old path '{hit.group(1)}'")

    for line in (ROOT / "CMakeLists.txt").read_text(encoding="utf-8").splitlines():
        hit = RETIRED_REF.search(line)
        if hit:
            failures.append(f"CMakeLists.txt: old path '{hit.group(1)}'")

    if failures:
        print("check_retired_paths FAILED:")
        for f in failures:
            print(f"  {f}")
        return 1
    print("check_retired_paths: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
