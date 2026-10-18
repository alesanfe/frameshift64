# Tests

Tests live here — organized by **level** first, domain second. `scripts/testing/`
holds thin wrappers that invoke harnesses from this tree; never embed test
logic there.

## Levels

| Dir | What runs | Examples |
|-----|-----------|----------|
| `unit/` | One component, no ROM, no network, no processes | `platform/coroutine_test.cpp`, `resources/vpk0_test.*` |
| `integration/` | Components wired together; may run the real binary headless or the extraction pipeline | `assets/extraction_recipes_test.py`, `modding/mod_package_test.py` |
| `contract/` | Cross-component wire/format contracts (`specs/` is the normative source) | netplay protocol, lobby API, mod manifest, save format |
| `e2e/` | Full game sessions via `harness/` | netplay room codes, first-run, packaging |
| `fuzz/` | Mutational drivers + `corpora/` seeds | `tools/analysis/fuzz/run_parser_fuzz.py` consumes `fuzz/corpora/<parser>/` |
| `security/` | Hostile-input and abuse-model tests | path traversal, zip bombs, lobby abuse |
| `smoke/` | Cheap "does it boot/respond" probes | headless boot, lobby `/health` |
| `regression/` | One test per fixed bug — `issue_<num>_<desc>_test` or the bug-doc slug |
| `performance/` | Benchmarks + `baselines/` |

Domain subdirs under a level mirror `src/` where it helps discoverability
(`unit/netplay/{session,transport,rollback}`, `unit/{platform,resources,…}`).
Unit tests of the adapter layer live in `unit/adapters/` (not `unit/integration/`
— that name means the level, not the module).

## Inputs vs outputs

| Dir | Role |
|-----|------|
| `fixtures/` | Hand-built inputs that provoke a scenario (corrupt saves, bad packets, invalid mods) |
| `golden/` | Canonical expected outputs (hashes, serialized packets, decoded frames) |
| `support/` | Reusable test utilities — C++ `include/smash_test/` + `src/`, Python `python/` |
| `harness/` | Process/environment drivers — headless game runner, lobby spawner, netplay session runner |
| `data/` | Static public data only; runtime artifacts go to `out/tests/`, never here |

No ROM bytes or extracted assets may be committed — build synthetic data or
require `SMASH_TEST_ROM_{US,JP}` env vars and label the test `requires-rom`.

## Naming

`<unit>_<scenario>_test.{cpp,py}` — e.g. `udp_transport_reordered_packets_test.cpp`.
Regressions: `issue_<num>_<desc>_test.<ext>` when a stable issue id exists.

## Running

pytest is the unified orchestrator (config: `pyproject.toml`, fixtures:
`conftest.py`, marker conventions in `tests/support/config/test-suites.yml`).
It collects the `test_*.py` wrappers, which drive the standalone
`*_test.py` scripts, ctest binaries, and game processes underneath.

```bash
# declarative suites — the canonical entry point
python tools/developer/testing/run_tests.py --suite quick          # unit+smoke, no rom/net/gpu
python tools/developer/testing/run_tests.py --suite pull-request   # unit+integration+contract+security
python tools/developer/testing/run_tests.py --suite netplay        # everything marked netplay
python tools/developer/testing/run_tests.py --list                 # suite -> marker expression

# or pytest directly
pytest -m "unit or integration"
pytest -m "e2e and netplay" --build-dir out/build/verify/Debug --keep-artifacts
pytest -n auto -m "not serial and not requires_rom"      # xdist
pytest --collect-only -q                                # discovery

# the underlying drivers still run standalone (and via scripts/testing/*.ps1)
python tests/e2e/netplay/lobbytest_multi_test.py --players 4 --seconds 60
python tests/integration/assets/extraction_recipes_test.py
python tools/analysis/fuzz/run_parser_fuzz.py --parser manifest --binary build/Release/BattleShip.exe --iters 300
```

Fixtures available to every test: `build_dir` / `game_executable`
(autodetects the newest binary under `out/build/`, override with
`--build-dir`/`--executable`), `require_rom` (skips unless
`--rom-{us,jp}` or `baserom.<region>.z64` exists), `artifacts_dir`
(per-test `out/test-results/artifacts/<id>/`, kept on failure or
`--keep-artifacts`), `test_environment`, `ctest`,
`native_coverage_env`.

## Coverage

Two lanes, one report tree under `out/coverage/`:

```bash
# Python lane (tools/, tests/support/python) — pytest-cov
pytest -m "unit or integration" --cov --cov-branch \
       --cov-report=term-missing --cov-report=html:out/coverage/python/html

# C/C++ lane — LLVM source-based coverage on a clang/gcc build
cmake -B out/build/<preset> -DSSB64_ENABLE_COVERAGE=ON && cmake --build out/build/<preset>
LLVM_PROFILE_FILE=$PWD/out/coverage/native/raw/%p-%m.profraw \
    python tools/developer/testing/run_tests.py --suite pull-request
python tools/developer/testing/coverage.py --binary out/build/<preset>/BattleShip \
    --output-dir out/coverage/native        # profdata + html + lcov.info
```

The C/C++ report excludes `components/decomp/`, `components/libultraship/`, `components/game-runtime/`,
`tests/` and generated files — it measures the modern `src/` layer only.

CTest/label taxonomy (`smash_add_test`) is defined in
`tests/CMakeLists.txt` and enabled with `-DSSB64_BUILD_TESTING=ON`;
pytest reaches those binaries through `test_ctest_unit`.

## Flaky policy

No `flaky/` dir. Quarantined tests go in `config/known-failures.yml` with an
issue, affected platforms, and an expiry — CI keeps running them
non-blocking and the entry expires loudly.
