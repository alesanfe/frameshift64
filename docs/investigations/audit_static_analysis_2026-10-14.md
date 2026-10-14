# Auditoría de análisis estático — 2026-10-14

Suite instalada en `.venv-lint` (Python 3.12) y ejecutada sobre el árbol
completo. Configuración reproducible: `tools/lint/requirements.txt`,
`.clang-tidy`, `config/static-analysis/cppcheck-suppressions.txt`, `.codespellrc`, `.yamllint.yml`,
`config/semgrep/smash-classic.yml`, `.pre-commit-config.yaml`, `tools/lint/run_project_lint.py`,
scripts/development/configure-lint.bat (local, gitignored) (vcvars + VCPKG_ROOT → `build-lint/` con Ninja y
`compile_commands.json`, 833 TUs / 830 ficheros únicos).

Informes crudos en `lint-reports/` (gitignored).

## Cobertura por herramienta

| Herramienta | Ámbito | Resultado |
|---|---|---|
| clang-tidy 22.1.8 | 86 TUs propios (`port/ include/ android/ uwp/ tools/ tools/debugging/`) | 2287 warnings, 22 "errors" |
| cppcheck 1.5.3 | 831 ficheros (todo el compile DB, deps incl.) | 244 errors, 367 warnings, 5410 style |
| cpplint 2.0.2 | código propio | 12236 marcadores (≈9987 whitespace/tab — baseline Google-style no aplica; decomp usa tabs por .clang-format) |
| cmake-lint | 5 ficheros CMake | 2378 marcadores (1823× C0327 whitespace) |
| lizard | código propio | 75 funciones > umbrales (CCN>15, >120 líneas, >6 params) |
| codespell | repo sin decomp/deps | ruido: `te`/`siz` en datos extraídos y campos RDP → añadidos a ignore-list |
| yamllint | workflows + configs | release.yml: 7 errores `too many spaces after colon`; newlines CRLF |
| ruff | scripts Python propios | 221 avisos (UP031, I001, UP045 — modernización) |
| bandit | scripts/tools/debug_tools | eval() en tools/creditsTextConverter.py:110,137; subprocess sin shell en tools (esperado) |
| semgrep (reglas propias) | net code + código propio | 62 findings → tras afinar reglas: atoi en args de red, recv buffers |

## Hallazgos confirmados (código propio)

1. **`src/compatibility/port_aobj_fixup.cpp:439`** — `in_end = in + size` se calcula *antes*
   de comprobar `src == nullptr`: aritmética de punteros sobre NULL = UB
   formal. Fix trivial: mover el check antes del cálculo.
2. **`src/netplay/session/netlobby.cpp` ~30 sitios** — `atoi`/`strtoul` sobre campos de
   mensajes `SSBL STARTM/START/GO` y JSON del Worker sin validación de rango
   ni de formato (cert-err34-c, regla `unbounded-atoi-on-packet`). Coherente
   con el análisis previo: el parser asume peers bienintencionados. Con
   SEC-1/SEC-2 sin cerrar, estos campos son superficie de ataque real.
3. **`src/diagnostics/renderdoc_app.h:43`** — `#error "Unknown platform"`: cppcheck lo
   evalúa sin macros de plataforma; verificar que solo se incluya bajo
   Windows/MSVC o dar fallback.
4. **`src/presentation/UIWidgets.cpp:969`** — `uninitvar: longest` (cppcheck).
5. **`src/platform/coroutine/coroutine_posix.cpp:47-51`** — `PortCoroutine` sin inicializadores
   de miembro; `src/netplay/transport/udp_transport.cpp:47` — `PendingPacket::release_ms` sin
   default (hoy siempre se asigna en el único constructor; frágil).
6. **`src/resources/loading/textures/hires/HiResHook.cpp:104`, `src/presentation/Menu.cpp:419`,
   `PortMenu.cpp:1689`, `port.cpp:632/1181/1436`, `ShaderDownloader.cpp:747`** —
   `try{}` con excepciones deshabilitadas según clang-tidy. El build MSVC real
   los compila (¿`/EHsc` por target, no global?), pero un build con
   excepciones off compilaría `try` como error. Confirma divergencia de flags
   entre la db Ninja y VS.

## Candidatos (requieren reproducción)

- `ShaderDownloader.cpp:569/586/802` `accessMoved` sobre `s_tempDir` —
  probablemente FP (el `clear()` post-move es legal), pero el patrón
  move+clear bajo mutex merece `std::exchange`.
- `decomp/src/sys/netpeer.c:1906` — `dbg` shadowed; `netrollback.c:88-93` —
  6 miembros de `SYNetRollbackState` nunca usados (`arena_size`, `heap`,
  `spawn_serial`, `aobj_fixup*`) — ¿estado de snapshot que no se serializa?
  **Relevante para R3**: si `aobj_fixup` forma parte del estado rollback y no
  entra en snapshot/hash, es un hueco real del sync. Verificar contra
  `port_aobj_fixup_snapshot_*`.
- 43× `performance-no-int-to-ptr`, 122× narrowing — estructural en un port
  N64; revisar solo los que toquen ticks/slots.
- `bugprone-random-generator-seed` en `udp_transport.cpp` — `sSimRng(1)`
  semilla fija: **es intencional** (SIM_SEED determinista); documentar.

## Infraestructura ya existente (no rehacer)

- Harness de simulación: `SSB64_NETPLAY_SIM_*`, cola ordenada por release_ms.
- `SSB64_ROLLBACK_SYNCTEST`, `NETPLAY_FUZZ`, `CONFIRMED_TRACE`, scripts
  `netplay_*.ps1`, `rollback_synctest.ps1`.
- `port_aobj_fixup_snapshot_save/load` ya existe (el parser en 439 tiene la
  precarga de `in_end` mencionada arriba).

## Cobertura ausente (confirmada por esta pasada)

- El simulador no puede duplicar/reordenar/corromper (solo delay+jitter+loss).
- Sin fuzzing de parsers binarios (`port_udp_recv_from`, framing relay).
- Sin job de CI que ejecute tests — build.yml solo compila.
- `unbounded-atoi-on-packet`: ningún `isdigit`/rango antes de `atoi` en todo
  `handle_game_message`.

## Prioridad recomendada (orden del plan)

1. `port_aobj_fixup.cpp:439` (UB trivial, un diff).
2. Validación de argumentos `SSBL` en `netlobby.cpp` + rate-limit/cierre SEC-*.
3. `release_ms`/`PortCoroutine`/`HashKey` inicializadores (un commit de
   higiene, sin cambio semántico).
4. CI: job `synctest` + `netplay_localtest` con build Debug.
5. Baseline cpplint/cmake-lint solo informativo; **no** reformatear decomp.

## CI añadido (misma pasada)

| Workflow | Qué hace | Bloqueo |
|---|---|---|
| `static-analysis.yml` | fast-lints (codespell/yamllint/ruff/bandit/lizard/cpplint/cmake-lint/semgrep-smash) + clang-tidy & cppcheck sobre `compile_commands.json` real (ubuntu+clang) | Report-only en baseline; artefactos 14 días |
| `codeql.yml` | C/C++ con build manual + JS del Worker; semanal | Bloquea; necesita GHAS si el repo es privado |
| `determinism.yml` | SyncTest d=1, stress d=6, localtest limpio + matriz 50/100/150 ms + loss 3-5% — reutiliza los `.ps1` existentes | Nightly/dispatch; **skip limpio si falta el secreto `BASEROM_US_Z64`** (base64 de la ROM del usuario — nunca commiteada) |
| `android.yml` | `GenerateF3DO2R` (sin ROM) → `gradlew assembleDebug`, APK como artefacto | Bloquea en PR |
| `dependabot.yml` | github-actions, npm (cloudflare), gradle, submódulos (limit 0 — los bumps van por worktree) | — |

Pendiente por diseño: coverage (gcov/lcov) y replay-verification dependen del
mismo gate de ROM; IWYU/jscpd/cpd quedan como siguientes candidatos.
Secret scanning nativo se activa en Settings > Security (no es workflow).

## No-verificados / límites

- clang-tidy corre con driver clang sobre flags MSVC: los "cannot use try
  with exceptions disabled" son artefacto si el TU real lleva `/EHsc`;
  tratar como señal de flags divergentes, no de bug.
- Resultados de `decomp/` (shifts by 31, OOB en scsubsysunused) son código
  heredado a propósito — no tocar salvo impacto en port.
- `lint-reports/` y `.venv-lint/` quedan fuera de git por .gitignore.
