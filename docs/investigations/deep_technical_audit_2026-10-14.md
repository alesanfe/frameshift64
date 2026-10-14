# Auditoría técnica profunda — smash-classic (2026-10-14)

Auditoría read-only del árbol reorganizado. HEAD de referencia: `d4f0298a`
con 575 cambios locales sin commitear (reorganización de `port/`, `server/`,
`scripts/`, `android/`, `uwp/`, `yamls/` a `src/`, `services/`,
`scripts/{testing,development,packaging}`, `platforms/`,
`assets/configuration/extraction/`).

Método: inventario de git + 4 exploradores read-only por dominio (build/CI,
servicio lobby, capa de port, core decomp) + revisión manual del engine de
netplay (el quinto explorador no completó; su dominio se cubrió a mano).
Nada se ejecutó que modificara el árbol; no se compilaron ni corrieron tests.

---

## 1. Resumen ejecutivo

La reorganización está sana en el código fuente pero **dejó rotos los caminos
de release, Android, UWP y packaging** — ningún build de publicación funciona
hoy desde un checkout limpio. El hallazgo más grave: `assets/configuration/`
existe localmente pero `.gitignore` (`assets/*` sin negación) lo mantiene
invisible a git — **todo clone fresco falla en `cmake -B` al primer
`configure_file`** de `gamecontrollerdb.txt`, y falla de nuevo en build con
los YAMLs de extracción. Es el clásico "funciona en la máquina del autor".

En netplay, el servicio desplegado tiene dos fallos confirmados:
`role=spec` es rechazado en el edge fetch (los espectadores llevan rotos en
producción desde que se implementaron — el test local pasa porque node-lobby
sí acepta spec), y cualquiera con el código de sala puede matar la sala
conectando `role=host` con la otra versión de protocolo. Además, el
confinamiento spec→host es solo "slot destino = 0": un frame binario que
empieza por `SSBL ` ejecuta el parser completo del lobby en el host —
incluidos los `fail()` de versión/compat que tumban la sala entera.

En el port, los parsers de blobs de audio y los fixups de sprites/reloc
confían en contadores y offsets del archivo: un `.o2r` de mod malformado
produce OOB read/write. El handler de PICK compartido hace que cualquier peer
(con token de versión/compat incorrecto) tumbe el lobby completo en vez de
ser rechazado individualmente.

Hallazgos totales: **11 confirmados de severidad alta/crítica**, ~20 medios,
~20 bajos/deuda. Lista completa en §4.

## 2. Inventario

- `git status`: 393 borrados (rutas pre-reorg), 115 modificados, 76
  untracked (rutas nuevas), 3 renombrados. Reorg en curso, coherente.
- Secretos: ninguno real. `keystore.properties` correctamente ignorado;
  StormLib contiene una clave pública MPQ conocida (vendored, sin riesgo).
- Submodules: `libultraship` (ssb64), `torch` (ssb64), `decomp` (decomp →
  frameshift64). Ninguno modificado por esta auditoría.
- Hotspots de historia (últimos commits): `netlobby.cpp`, `port_udp.cpp`,
  `index.js`, `node-lobby.js` — todo netplay, coherente con el trabajo
  reciente.
- Tests: no existe `ctest`/`enable_testing` en el proyecto raíz; todo el
  testing conductual es PowerShell manual + suites JS del lobby. Los dos
  probes Python de `tests/unit/` están rotos (math de repo-root mal).
- Documentación: abundante deuda de rutas pre-reorg en comentarios, docs y
  scripts (ver AUD-45).

## 3. Mapa de arquitectura (para contexto de los hallazgos)

```
Cliente (ssb64.exe)
├─ src/presentation/      PortMenu, OnlineStatus, Menu (sidebar)
├─ src/netplay/session/   netlobby (SSBL, peers, LAN, spec)
├─ src/netplay/transport/  port_udp (link table 0..7, framing relay)
│                          port_ws (control socket + uplink)
├─ src/persistence/       port_save (SRAM + replays dir)
├─ src/observability/     port_log
├─ src/integration/decomp/  audio_bridge, lbreloc_byteswap, particle_bank
├─ src/integration/modding/ ModRegistry/ScriptLoader TCC
├─ src/compatibility/     n64_stubs (tasks, coroutines, defer queues)
├─ src/distribution/      Updater, ShaderDownloader, ModPackDownloader
├─ src/features/          widescreen, interp, gameplay/audio/social packs
└─ decomp/src/sys/        netpeer (star/FORWARD/spec links), netinput
                          (sources: local/remote-confirmed/predicted/saved),
                          netrollback (8 snapshots + resim), netreplay
                          (record/play/desync pkg/takeover), scheduler

Servicios
└─ services/netplay-lobby/  cloudflare/src/index.js (edge fetch → DO,
   star routing, upkey auth, spec slots 4-7) + node-lobby.js (mirror local)
```

## 4. Matriz de hallazgos

| ID | Sev. | Conf. | Clase | Subsistema | Resumen |
|----|------|-------|-------|-----------|---------|
| AUD-01 | CRÍTICA | CONFIRMADO | Bug + debt | build/.gitignore | `assets/configuration/` ignorado: clone fresco no configura |
| AUD-02 | ALTA | CONFIRMADO | Bug | CI release.yml | Job Android con rutas pre-reorg (2 fallos) |
| AUD-03 | ALTA | CONFIRMADO | Bug | platforms/android | `repoRoot` resuelve a `platforms/` no al root |
| AUD-04 | ALTA | CONFIRMADO | Bug | CI android.yml | `working-directory: android` obsoleto |
| AUD-05 | ALTA | CONFIRMADO | Bug | CI build-uwp.yml | Staging con rutas `tools/`/`yamls/` viejas |
| AUD-06 | ALTA | CONFIRMADO | Bug | packaging | ROOT una carpeta corto + rutas internas viejas (linux+mac) |
| AUD-07 | ALTA | CONFIRMADO | Bug | packaging | windows.ps1: tool movido + string mangleado + yamls/ |
| AUD-08 | MEDIA | CONFIRMADO | Bug | packaging | Comentario tras `\` inyecta arg extra en los 3 packagers |
| AUD-09 | ALTA | CONFIRMADO | Bug | lobby prod | `role=spec` rechazado (400) en edge fetch → spec roto en prod |
| AUD-10 | ALTA | CONFIRMADO | Seguridad | lobby | `role=host` cross-version resetea sala viva (kill sin auth) |
| AUD-11 | ALTA | CONFIRMADO | Seguridad | lobby+cliente | Spec→host binario sin filtro; `SSBL ` smuggled ejecuta `fail()` del lobby. Generaliza: PICK con versión mala de CUALQUIER peer mata la sala |
| AUD-12 | MEDIA | CONFIRMADO | Seguridad | lobby | Frames oversize no cuentan al rate limiter |
| AUD-13 | MEDIA | CONFIRMADO | Bug | audio/bridge | Parsers .ctl/.tbl/.sbk confían en counts/offsets → OOB via mod |
| AUD-14 | MEDIA | CONFIRMADO | Bug | bridge/reloc | Sprite fixup confía en nbitmaps/geometría → OOB write |
| AUD-15 | MEDIA | CONFIRMADO | Bug | compat/tasks | DP_FULL_SYNC fantasma por audio task → completa gfx antes |
| AUD-16 | MEDIA | CONFIRMADO | Bug | core/save | Escritura SRAM no atómica + errores invisibles al juego |
| AUD-17 | MEDIA | CONFIRMADO | Bug | core/log | `sLogFile` race + UAF de fclose vs threads detached |
| AUD-18 | ALTA | PROBABLE | Bug | rollback | Mispredict en tick no-snapshotable (pause/countdown) → desync sin recuperación |
| AUD-19 | MEDIA | CONFIRMADO | Bug | lobby/cliente | `fail()` por versión/compat de un peer mata todo el lobby |
| AUD-20 | MEDIA | PROBABLE | Riesgo | netplay/compat | Handshake solo hashea mods script-Loaded; mods de assets/reloc evaden |
| AUD-21 | MEDIA | CONFIRMADO | Bug | decomp FP | `-ffp-contract=off` solo en sinf/cosf → FMA en ARM64 diverge vs x64 |
| AUD-22 | BAJA | CONFIRMADO | Bug | decomp FP | MSVC: sinf/cosf bare siguen yendo a UCRT libm |
| AUD-23 | MED-BAJA | CONFIRMADO | Seguridad | lobby | Sin reaper: sockets zombie retienen slots y DO billable |
| AUD-24 | BAJA | CONFIRMADO | Seguridad | lobby | Budget de joins = exactamente una sala llena; atacante la mantiene cerrada |
| AUD-25 | BAJA | CONFIRMADO | Seguridad | lobby | Oráculo de existencia de sala sin throttle entre códigos |
| AUD-26 | MED-BAJA | PROBABLE | Bug | netplay LAN | HELLO no autenticado: endpoint arbitrario reclama slots 1-3 |
| AUD-27 | MED-BAJA | PROBABLE | Bug | rollback | Coroutines huerfanas al rebobinar sobre spawn de GObjThread |
| AUD-28 | MEDIA | CONFIRMADO | Deuda | semgrep | Reglas no cubren `src/netplay/` (paths.include `net/*` viejo) |
| AUD-29 | MED-BAJA | CONFIRMADO | Deuda | codegen | Targets phony reescriben salidas cada build → relink churn + ventana de borrado en generate_yamls |
| AUD-30 | MED-BAJA | CONFIRMADO | Deuda | tests | Scripts: sleeps fijos, defaults de build dir obsoletos |
| AUD-31 | MED-BAJA | CONFIRMADO | Bug | features | ShaderDownloader: worker sin cancelación → .glsl truncado, fase Done tras cancel |
| AUD-32 | MED-BAJA | CONFIRMADO | Seguridad | features | Updater Linux/macOS sin verificación de integridad (Windows sí) |
| AUD-33 | BAJA | CONFIRMADO | Bug | presentation | `InsertSidebarSearch` .at(size()) → crash con CVar obsoleto |
| AUD-34 | MED-BAJA | PROBABLE | Bug | bridge | Particle bank: validación loguea "absurd" pero usa datos basura igual |
| AUD-35 | BAJA | PROBABLE | Bug | application | MuteOnFocusLoss persiste MasterVolume=0 tras restart |
| AUD-36 | BAJA | PROBABLE | Bug | replay | Paths replay/desync truncan a 512/640 chars → dump perdido silente |
| AUD-37 | BAJA | CONFIRMADO | Deuda | compat | Orden de resume contradictorio con comentario; cap 8 rondas |
| AUD-38 | BAJA | CONFIRMADO | Deuda | compat | osSendMesg/RecvMesg fallan silente fuera de coroutine |
| AUD-39 | BAJA | CONFIRMADO | Deuda | compat | Stubs reportan 4 controllers siempre conectados |
| AUD-40 | BAJA | CONFIRMADO | Deuda | decomp | Canary de stack GObjThread es código muerto en port |
| AUD-41 | BAJA | RESUELTO | Deuda | cmake | `coroutine_test` include dir `port/` muerto; 3 CMakeLists de mods con `merge_mod.py` viejo; 2 probes py con root mal — todos corregidos post-reorganización |
| AUD-42 | BAJA | CONFIRMADO | Deuda | codegen | RelocFileTable embebe ruta absoluta del autor |
| AUD-43 | BAJA | CONFIRMADO | Deuda | cmake | TorchExternal sin BUILD_ALWAYS → sidecar stale |
| AUD-44 | BAJA | CONFIRMADO | Deuda | netplay | `token=="none"` rama muerta; hash mod no incluye contenido del archivo |
| AUD-45 | BAJA | CONFIRMADO | Doc debt | docs/scripts | Rutas pre-reorg en .gitignore comments, BUILDING.md, scripts usage, mods READMEs |
| AUD-46 | BAJA | CONFIRMADO | Privacidad | lobby | broadcastRoster envía endpoints de players a specs |
| AUD-47 | INFO | CONFIRMADO | Limitación | replay | Takeover: otros slots reciben frames en cero tras agotar el replay |
| AUD-48 | INFO | CONFIRMADO | Limitación | spec | Spec mid-match no puede reconstruir estado (sin snapshot) — espera siguiente match |

## 5. Hallazgos detallados (severidad alta y crítica)

### AUD-01 — `assets/configuration/` entero es incommiteable → clone fresco roto — CRÍTICA / CONFIRMADO

- **Síntoma:** `cmake -B build` en clone fresco → `configure_file` error en
  `gamecontrollerdb.txt`; si pasara, `copy_directory` de
  `extraction/${SSB64_VERSION}` y `generate_reloc_table.py` (FileNotFoundError)
  fallan.
- **Evidencia:** `.gitignore:54` `assets/*` con negaciones solo de
  iconos/fonts — ninguna `!assets/configuration/`. `git check-ignore -v
  assets/configuration/extraction/us/config.yml` → `.gitignore:54`.
  `git status --porcelain -- assets/` vacío con los archivos presentes en
  disco. `cmake/RuntimeAssets.cmake:7-11`, `CMakeLists.txt:631,1317-1318`,
  `tools/codegen/generate_reloc_table.py:63`.
- **Causa:** el reorg movió recetas/controller db a una ruta bajo la regla
  `assets/*`. Localmente existen (pre-reorg o copiados), en git no.
- **Repro:** `git clone` limpio → `cmake -B build` → fallo inmediato.
- **Impacto:** todo CI job que corre cmake (build, release, determinism,
  codeql, uwp, static-analysis lint) falla; release imposible.
- **Fix:** `!assets/configuration/` (+negaciones anidadas necesarias) en
  `.gitignore`, commitear `gamecontrollerdb.txt` + `extraction/{us,jp}/*.yml`
  (documentados como commiteados en `.gitignore:39-45` y BUILDING.md:89 —
  actualmente falso). Alternativa: wire `RegenerateRelocYamls` en
  `PrepareBuildInputs` bajo guard baserom, pero gamecontrollerdb no tiene
  generador — debe commitearse.
- **Test regresión:** job CI `cmake -B` sobre checkout limpio sin caché.

### AUD-09 — Espectadores rotos en producción: edge fetch rechaza `role=spec` — ALTA / CONFIRMADO

- **Síntoma:** spectate vía worker real → HTTP 400 "bad room or role".
  `spectate_test.ps1` pasa localmente (node-lobby acepta spec).
- **Evidencia:** `index.js:101` `role !== "host" && role !== "join"` antes de
  llegar al DO; el DO implementa spec completo (`index.js:227-241`);
  `node-lobby.js:295` acepta spec; cliente siempre envía `role=spec`
  (`netlobby.cpp:456`); doc `protocol/messages.md:34` bendice el rol.
- **Causa:** validación en dos capas — edge y DO — divergentes; node-lobby
  no puede modelar rechazos pre-upgrade (upgrada todo y luego cierra).
- **Repro:** `GET wss://<worker>/ws?room=ABCDEF&role=spec&v=2` → 400.
- **Impacto:** feature shipeada muerta en prod; tests que mienten.
- **Fix:** permitir `"spec"` en index.js:101 — mejor: hoiste validación de
  rol al DO para que ambas impls compartan un solo camino. **Junto con
  AUD-11** (si se arregla spec sin filtrar spec→host se habilita el grief).
- **Test:** caso spec en `test-lobby-security.js` + test que ejercite el
  edge-fetch (hoy ninguno lo hace — por eso sobrevivió el bug).

### AUD-10 — `role=host` cross-version mata una sala viva — ALTA / CONFIRMADO (seguridad)

- **Síntoma:** un tercero con el código de sala conecta `role=host&v=1` a una
  sala v2 → todos los sockets reciben close 4002 "room-reset" y el atacante
  queda de host (captura endpoints y tráfico de joins posteriores).
- **Evidencia:** `index.js:243-253`: `oldHost` solo busca host **de la misma
  versión**; al no encontrarlo, cierra `this.sockets` entero y adopta la
  versión nueva. Mirror en `node-lobby.js:372-383`.
- **Repro:** host v2 + clientes en `ABCDEF`; desde otro socket abrir
  `/ws?room=ABCDEF&role=host` (sin `v` → v1) → todos expulsados.
- **Impacto:** kill de sala sin autenticación, un solo frame. El código de
  sala es enumerable (AUD-25) y se comparte con specs.
- **Fix:** rechazar cuando `sockets` no esté vacío o `version !== 0 &&
  version !== this.version` antes de resetear; el caso legítimo (room
  hostless) ya se limpia por host-left, así que el reset puede eliminarse.
- **Test:** caso cross-version-host en test-lobby-security.js.

### AUD-11 — Spec→host reenvía binario sin discriminación de payload; `fail()` mata la sala — ALTA / CONFIRMADO

- **Cadena verificada punta a punta:**
  1. Worker: `index.js:416` `if (info.kind === "spec" && toSlot !== 0) return`
     — único filtro; cualquier payload a slot 0 pasa.
  2. Host `pump_transport` (`netlobby.cpp:908-939`): solo exige prefijo
     `SSBL `; `peer_for_source("relay:4")` → peer spec → `handle_game_message`.
  3. `handle_game_message` (`netlobby.cpp:749-768`): PICK con `version !=
     kGameVersion` → `fail("Version mismatch…")` → shutdown del lobby →
     host-left en el worker → cascada cierra la sala.
- **Consecuencia:** un spec anónimo enviando `[0x00]["SSBL PICK 0 0 0 x"]`
  tumba la sala en la ventana lobby/start. Contradice
  `docs/netplay/protocol/security-model.md:20-21` ("spec slots are
  receive-only at the server").
- **Generalización (mismo hallazgo):** PICK de **cualquier** peer con versión
  o token distinto ejecuta `fail()` del lobby completo en vez de rechazar
  solo a ese peer — diseño que convierte cualquier cliente unido en botón
  de kill de sala.
- **Fix:** (a) worker: exigir `bytes[1..5]==="SSBL"` en frames spec→host
  (barato, refleja el contrato); (b) cliente: whitelist de verbos por rol de
  peer — un spec jamás debería poder disparar `fail()`; (c) los checks de
  versión/compat deberían dropear al peer, no tumbar el lobby.
- **Test:** fuzz spec→host con PICK/STARTM maliciosos en test-lobby-fuzz +
  caso client-side que un PICK malo de un peer dropee solo ese peer.

### AUD-13 — Parsers de blobs de audio confían en counts/offsets — ALTA / CONFIRMADO

- **Evidencia:** `src/integration/decomp/audio_bridge.cpp:505-547` — `seqCount`,
  `bankCount`, `instCount`, offsets y lens leídos del blob jamás se validan
  contra `sbkSize`/`size`/`ctlSize`; punteros salvajes entran a
  `seqArray`/`pkg->data`/`bankArray`; `sSYAudioSequenceBank2 =
  bankArray[0]` (:596) lee fuera si `bankCount==0`. Los mods montan en el
  mismo ArchiveManager y pueden shadowear estos paths.
- **Repro:** mod `.o2r` con `audio/S1_music_sbk` de count 0x7FFF y archivo
  pequeño → OOB read/crash en `portAudioLoadAssets`.
- **Fix:** validar tabla de índices contra el tamaño del blob antes de
  materializar punteros; rechazar bancos con count 0.
- **Test:** fixture `.sbk` truncado → carga debe fallar limpio, no crash.

### AUD-14 — Fixup de sprites confía en `nbitmaps` y geometría — MEDIA-ALTA / MUY PROBABLE

- `lbreloc_byteswap.cpp` `portFixupSpriteBitmapData`: valida el puntero del
  array vía reloc, pero `nbitmaps`, width/height/buf de cada bitmap se usan
  para deswizzle sin comprobar que `nbitmaps*16` quepa en la imagen del
  archivo ni que las escrituras queden en el buffer del bitmap → OOB write
  en arena con reloc malformado (shadoweable por mods).
- **Fix:** bound-check de `nbitmaps` contra el tamaño restante del archivo y
  de cada rectángulo de deswizzle contra su buffer.

### AUD-15 — DP_FULL_SYNC fantasma por task de audio — MEDIA / CONFIRMADO (mecanismo)

- **Evidencia:** `n64_stubs.c:623-624` — `osSpTaskStartGo` incrementa
  `sPortPendingSPInts` y `sPortPendingDPInts` **para todo tipo de task**;
  solo `M_GFXTASK` hace `port_submit_display_list` (:591). Cada task de
  audio (1 por VI, `audio.c` → scheduler) postea un DP_FULL_SYNC fantasma
  `defer_n` VIs después. `sySchedulerDpFullSync` (`scheduler.c:1162-1184`)
  interpreta cualquier DP sync como "gfx task en vuelo terminó": swap de
  framebuffer, post de retVal, clear de CurrentTaskGfx, y
  `ExecuteTasksAll`. Con defer≥2 el sync fantasma de audio puede liberar un
  gfx task pesado antes de tiempo — acorta/cancela el freeze de clímax que
  el defer existe para emular y puede liberar el framebuffer siendo target
  nominal de dibujo. Con defer=1 (lo normal) cada VI genera 2 DP_FULL_SYNC;
  el segundo cae a las ramas `scCurrentQueue3Task`/paused-queue (:1185+).
- **Impacto:** modelo de timing del defer roto; release prematuro de tasks.
  Como el render es síncrono en el port, el glitch visible es sutil, pero la
  emulación de contención de framebuffer que se diseñó es incorrecta.
- **Fix:** solo `M_GFXTASK` debería alimentar `sPortPendingDPInts` (y
  posiblemente `defer_n`); las tasks de audio solo SP_TASK_DONE.
- **Test:** `SSB64_GFX_DEFER_VI=2` + conteo de DpFullSync por VI (debe ser
  #gfx tasks, hoy #gfx+#audio).

### AUD-16 — Escritura SRAM no atómica + fallo invisible — MEDIA-ALTA / CONFIRMADO

- `port_save.cpp:179-227`: open/seek/write/close in-place, sin temp+rename;
  crash/power-loss deja imagen SRAM mezcla vieja/nueva sin checksum que la
  detecte. Los wrappers de `decomp/src/sys/dma.c` descartan el retorno →
  disco lleno/save path read-only = pérdida de progreso silente (solo log).
  `metaPath()` (:76-84) corta en el último `.` del path completo — un path
  sin extensión con `.` en directorios escribe el sidecar en el padre.
- **Fix:** temp file + flush + rename; propagar error a la capa de juego
  (al menos un aviso); corregir metaPath para operar sobre el basename.

### AUD-18 — Mispredict en tick no-snapshotable → desync sin recuperación — ALTA (UX netplay) / MUY PROBABLE

- `netrollback.c:577-583` `CheckCanSave` rechaza snapshots cuando
  `game_status != Go` o `sGCThreadsActive != 0` (countdown, **pause**,
  time-up, sudden-death). `Resimulate` (:992-1002) falla duro si el slot del
  tick mispredicted no es válido → solo queda `FailedRollbacks++` y los
  peers divergen en silencio.
- El hueco real: durante `nSCBattleGameStatusPause` los inputs remotos
  **sí** son significativos (`ifCommonBattlePauseUpdateInterface` lee
  unpause/A+B+R+Z del que pausó). Un mispredict que aterriza en un tick de
  pausa no tiene snapshot → resim imposible → un peer despausa y el otro no:
  divergencia permanente (y el sync-check puede ni detectarlo si diverge
  después).
- **Fix:** cubrir con snapshot los ticks de pausa (los threads no están
  activos en pause — verificar por qué `game_status` los excluye; si es por
  el GObjThread de pausa, permitir save con ese único thread), o degradar a
  resim parcial reconstruyendo input history sin snapshot de estado.
- **Test:** 2 instancias + loss injection durante pause con botones remotos;
  observar `rollback FAILED` y hashes.

### AUD-20 — Handshake de compatibilidad no cubre mods de assets — MEDIA / PROBABLE

- `netlobby.cpp:114-125`: hash FNV solo sobre `ModState::Loaded`. El
  comentario justifica "only code that ran ModInit can affect determinism"
  — **falso**: un mod reloc/asset-only (hitboxes, físicas, moveset) altera
  la simulación sin ModInit. `ModRegistry::Snapshot()` sí representa
  NotLoaded/InvalidManifest. Dos peers con distinto moveset .o2r comparten
  token → desync silencioso. Además el hash es `name@version`, no contenido:
  dos .o2r distintos con mismo nombre/versión pasan.
- Nota adicional: `token == "none"` (:1222) es rama muerta — `compat_token()`
  nunca devuelve "none"; el PICK legacy de 6 campos nunca se emite. Y el
  token ausente en peer legacy se acepta (intencional, interop) — el
  handshake detecta accidentes, no adversarios. `SSB64_COMPAT_TOKEN` solo
  existe bajo `SSB64_TEST_HOOKS` — correcto.
- **Fix:** incluir en el hash (o en un segundo campo) identidad+hash de
  contenido de los archivos montados gameplay-relevantes (o al menos
  nombre+tamaño+CRC del archivo).
- **Test:** dos builds con mods asset-only distintos → mismatch debe
  detectarse en PICK, no como desync a mitad de match.

### AUD-19 — PICK con versión/token malo de cualquier peer tumba el lobby — MEDIA / CONFIRMADO (diseño)

- `netlobby.cpp:754-768`: `fail()` global en vez de `peer_drop`. Cualquier
  cliente unido (no solo spec, ver AUD-11) puede matar la sala para todos.
  El comportamiento correcto es rechazar/eliminar al peer incompatible y
  mantener la sala.
- **Fix:** sustituir `fail` por mark-peer-incompatible + `peer_drop` + aviso
  en status ("peer X runs a different build").

### AUD-02…AUD-08 — Release/packaging rotos por rutas pre-reorg — ALTA / CONFIRMADO

Resumen (evidencia completa en el análisis de build/CI):

- **AUD-02** `release.yml:365` `working-directory: android` (no existe) +
  `:375` glob platforms/platforms/android/... (prefijo doble).
- **AUD-03** `platforms/android/app/build.gradle.kts:47` `repoRoot` =
  `..` desde `platforms/android` → `platforms/`; rompe `f3d.o2r`,
  `config.yml`, `extraction/`. Comentario :43-45 describe el layout viejo.
- **AUD-04** `android.yml:61` mismo `working-directory: android`.
- **AUD-05** `build-uwp.yml:104-111` tools/Prepare-BattleShip-US.ps1
  (real: `tools/asset-pipeline/Prepare-BattleShip-US.ps1`) + `yamls/us` (ya no existe).
- **AUD-06** `package-linux.sh:38`/`package-macos.sh:33` ROOT un nivel
  corto (`scripts/`) + tools/creditsTextConverter.py (real:
  `tools/asset-pipeline/creditsTextConverter.py`) + `yamls/$VER` + `gamecontrollerdb.txt` (AUD-01).
- **AUD-07** `package-windows.ps1`: `:80,84` mismo tool movido; `:159`
  string literalmente corrupto `"assets nfiguration ntrollersgamecontrollerdb.txt"`;
  `:161` `yamls\` viejo.
- **AUD-08** los tres packagers: `\` (o backtick) seguido de espacios antes
  de comentario → arg extra `" "` en la invocación cmake.
- **Fix:** correcciones mecánicas de rutas; regenerar los scripts con ROOT
  correcto y rutas nuevas; probar `package-*` en CI (hoy ningún job los
  ejecuta hasta release — por eso nadie lo vio).

## 6. Hallazgos medios/bajos (detalle abreviado)

- **AUD-12** oversize frames retornan antes de `allowMessage` → flood de
  >1KB gratis (index.js:397-398, :337-338; node :113,:151). Contar antes de
  dropear.
- **AUD-17** `port_log.c:13-41`: `sLogFile` sin sync; `fclose` puede correr
  mientras un thread detached (Updater, HiRes preload) está dentro de
  `vfprintf` → UAF. Fix: mutex o join de workers antes de close.
- **AUD-21** `-ffp-contract=off` solo en sinf/cosf (`CMakeLists.txt:753-758`);
  el resto de la simulación compila con contraction por defecto → `fmadd`
  en AArch64 cambia últimos ulp vs x86-64 → **desync cross-platform
  PC↔Android** sin diferencia de lógica. Fix: `-ffp-contract=off` global en
  TUs del juego (o al menos decomp entero).
- **AUD-22** MSVC ignora `#pragma weak sinf` → ~16 call sites van a UCRT →
  builds Windows divergen de GCC/Clang (documentado en
  `docs/bugs/trig_host_libm_divergence_2026-07-31.md`, sin resolver).
- **AUD-23** lobby sin heartbeat/idle reaper: socket zombie retiene slot y
  DO (billing). Recomendado ping server-side + sweep `lastSeen`.
- **AUD-24** `MAX_JOINS_PER_WINDOW=12` = exactamente una sala llena legítima
  (4 ctrl + 4 uplinks + 4 specs); intentos rechazados también cuentan →
  code-holder mantiene la sala cerrada a ~1.2 conn/s.
- **AUD-25** `no-such-room` vs `room-full`/`in-use` distingue salas vivas;
  el límite es por-DO → escaneo entre códigos sin throttle. Espacio 32^6 lo
  hace lento pero no imposible.
- **AUD-26** LAN `lan_accept` acepta `SSBL HELLO` de cualquier endpoint →
  3 HELLOs anónimos ocupan todos los slots. Mitigación posible: confirmación
  interactiva del host o allowlist de subnet.
- **AUD-27** resim que spawnea un GObjThread y luego se rebobina deja el
  `PortCoroutine` huerfano (fiber/ucontext + 64KB stack por ocurrencia;
  en Windows un Fiber handle). Bounded pero acumulable en sesiones largas.
- **AUD-28** `config/semgrep/smash-classic.yml` include paths `net/*`,
  `*netpeer*`… no cubren `src/netplay/**/*.cpp` → las reglas de net-safety no
  aplican al netcode del port.
- **AUD-29** `GenerateRelocArtifacts`/`GenerateCreditsAssets` reescriben
  outputs cada build (timestamp bump → recompile+relink); `generate_yamls`
  borra todos los `reloc_*.yml` antes de regenerar → fallo a mitad deja
  recetas incompletas que el table generator convierte en NULL entries con
  solo warning.
- **AUD-30** test scripts: sleeps fijos (2s/10-20s) en vez de polling;
  `rollback_synctest.ps1:17` `$PSScriptRoot\..\build` un nivel corto;
  `netplay_compat_test.ps1` default `build\x64\Release` no existe en
  ninguna convención actual.
- **AUD-31** `ShaderDownloader` worker detached sin flag de cancelación:
  cancel borra el temp dir mientras el worker copia desde él → `.glsl`
  truncado instalado, fase queda `Done` tras cancel.
- **AUD-32** Updater: Windows verifica size+sha256; Linux renombra `.part`
  solo con exit status de curl; macOS abre el DMG sin verificación. Y los
  threads detached loguean tras shutdown (ver AUD-17).
- **AUD-33** `Menu.cpp:170-188` `GetVectorIndexOf` devuelve `size()` si el
  CVar de sidebar no existe → `.at(size())` → `std::out_of_range` sin
  catch → crash al abrir menú con config de versión vieja.
- **AUD-34** `particle_bank_bridge.cpp`: validación detecta counts
  absurdos y retorna, pero `portParticleLoadBank` llama
  `lbParticleSetupBankID` con los datos igual → OOB en el intérprete.
- **AUD-35** `focus.cpp`: si se sale/crashea con MasterVolume=0 por
  MuteOnFocusLoss, el 0 persiste al config → próximo arranque muteado
  sin indicador.
- **AUD-36** `netreplay.c` `dir[512]`/`path[640]` snprintf → save dir
  profundo trunca paths → replay/desync dump perdido o en dir mangleado.
- **AUD-37** `port_resume_service_threads` itera orden de registro, no el
  orden de prioridad que describe el comentario; cap de 8 rondas puede
  partir un tick entre host frames.
- **AUD-38** `osSendMesg`/`osRecvMesg` devuelven -1 silente fuera de
  coroutine — contrato peligroso para código futuro.
- **AUD-39** stubs de controller reportan `CONT_CARD_ON` ×4 →
  `gSYControllerConnectedNum==4` siempre → pantalla "no controller" nunca
  se muestra (cosmético pero incorrecto).
- **AUD-40** canary `stack[7]=0xFEDCBA98` nunca puede trippear: los stacks
  coroutine son host-allocated 64KB, el array N64 no se ejecuta.
- **AUD-41** `coroutine_test` include dir `port/` muerto; 3 CMakeLists de
  mods llaman tools/merge_mod.py (real `tools/modding/merge_mod.py`); `test_vpk0.py`/
  `test_extern_ids.py` calculan repo-root un nivel corto y referencian
  tools/test_vpk0.c (real `tests/unit/resources/vpk0_test.c`).
  **RESUELTO (post-reorganización):** include dir → `src/platform`,
  mod CMakeLists → `tools/modding/merge_mod.py`, probes corregidos
  (`` `tests/unit/resources/*_test.py` `` resuelven root y ROM vía `roms/`/`SMASH_BASEROM`).
- **AUD-42** `RelocFileTable.us.h:11` embebe `C:/Users/alex0/...` absoluto —
  diffs por máquina + leak de path del autor.
- **AUD-43** `TorchExternal` sin `BUILD_ALWAYS` → submodule movido no
  rebuildea en build dir existente → sidecar/extractor stale.
- **AUD-44** ver AUD-20 (rama muerta `none` + name@version sin contenido).
- **AUD-45** deuda doc/comentarios: `.gitignore:39-45,95-98`, BUILDING.md
  (`build\<ver>` vs `out/build/<preset>`), usages en scripts,
  `mods/examples/*/README.md` (`port/hooks/…`), `generate_*.py` headers,
  `docs/architecture/overview.md:45` frase corrupta.
- **AUD-46** `broadcastRoster` manda `public`/`local` endpoints de players
  a specs — specs son el rol menos confiable y no necesitan IPs.
- **AUD-47** takeover: tras agotar el replay, slots Saved reciben frames en
  cero (`netinput.c:401-403`) — el jugador toma control y el resto queda
  idle. Aceptable para training, documentar.
- **AUD-48** spec que entra mid-match no recibe snapshot → espera al
  siguiente match (limitación conocida y deliberada).

## 7. Evaluación adversarial por perspectivas

1. **Jugador buscando glitches:** el lobby tolera bien inputs/menús
   (barreras, GO×5, frozen roster). El hueco real es AUD-18 (mispredict en
   pausa) y AUD-19 (un peer con build distinto tumba la sala).
2. **Peer malicioso/defectuoso:** las guardias engine-side son buenas
   (FORWARD solo por link de host, spec links con remote=NULL dropean,
   checksum wire-v4). Los huecos están en el plano lobby/relay: AUD-10,
   AUD-11, AUD-12, AUD-24, AUD-25 — todos baratos y sin autenticación más
   allá del código de sala.
3. **Mod incompatible/parcial:** AUD-13, AUD-14, AUD-34 (OOB por blobs),
   AUD-20 (hash de compat que no ve el mod). Mods pueden shadowear recursos
   de audio/reloc/particles — la superficie de parsing confiado es amplia.
4. **Sistema limitado/archivos corruptos/red inestable:** AUD-16 (SRAM
   no atómica), AUD-36 (paths truncados), AUD-12 (flood), AUD-23 (zombies),
   prediction "repeat-last" sin límite de tiempo de silencio (hay
   `SYNETPEER_REMOTE_DROP_MS` para pacing pero el match sigue simulando al
   caído).
5. **Mantenedor que no conoce supuestos:** AUD-45 (docs stale en masa),
   AUD-28 (semgrep no cubre donde debería), AUD-29 (writes
   incondicionales), AUD-44 (rama muerta que sugiere feature inexistente),
   AUD-37 (comentario que miente sobre el orden).

## 8. Verificado SANO (no re-chequear)

- Engine: FORWARD dropped en host y en links no-host de clientes; magic
  `SSNP`/versión/tipo/sesión validados; link_of exige relay-backed;
  `SYNETROLLBACK_SLOT_NUM` con slot COMPARE extra es deliberado; resim sí
  corre la sim completa (`func_update` = dispatcher); gcRunGObjProcess no
  bloquea en threads completados; LCG u32-wrap en port; sqrtf/libm
  correctly-rounded; arena de escena con memset+sanitizer; timer por
  sim-tick en resim.
- Lobby servidor: star routing + stamp de from-slot sin spoofing; upkey
  CSPRNG 128-bit por conexión sin oráculo; slots atómicos; cascada
  host-left; parser JSON envuelto; roster no inyectable por endpoints.
- Port: ordering de shutdown (watchdog joined antes de close), coroutines
  con canaries/guards, `port_dl_ranges` con mutex+generación, reloc chain
  walks con límites, touch overlay con bounds checks, native_dialog con
  quoting correcto.
- Build: presets con `out/build/`, lint configs con rutas nuevas,
  CONFIGURE_DEPENDS en el glob del port, actions SHA-pinned + enforcement,
  FetchContent pineado, DISABLE_SCRIPTING ordenado, determinism workflow
  no filtra ROM a artefactos.

## 9. Gaps de test

- No hay ctest/enable_testing: toda la cobertura es manual.
- `tests/unit/resources/vpk0_test.py`, `test_extern_ids.py` rotos (root math).
- `test-lobby-security.js` tiene cero casos spec y ninguno que ejercite el
  edge-fetch — por eso AUD-09/AUD-10 sobrevivieron.
- Nada prueba: spec→host binario, host cross-version, flood de oversize,
  mispredict en pausa, mod asset-only con handshake, SRAM write bajo crash,
  paths largos de save dir.
- Scripts PS con sleeps fijos — flakes en máquinas lentas/CI contended.

## 10. Backlog priorizado

**P0 (bloquean release / seguridad sin auth):**
1. AUD-01 — gitignore + commit de assets/configuration (desbloquea todo CI).
2. AUD-10 — room kill cross-version (1 línea de guardia).
3. AUD-09 + AUD-11 — spec en prod; arreglar juntos o se abre el grief.
4. AUD-02…AUD-08 — rutas de release/Android/UWP/packaging.

**P1:**
5. AUD-18 — desync irrecuperable en pausa.
6. AUD-13/AUD-14/AUD-34 — bounds-check de parsers de blobs (mod malicioso).
7. AUD-19 — fail()→peer_drop en checks de versión/compat.
8. AUD-12 — contar oversize en rate limiter.
9. AUD-16 — SRAM atómico + propagar error.
10. AUD-21 — `-ffp-contract=off` global para determinismo cross-platform.

**P2:**
11. AUD-20 — hash de contenido de mods.
12. AUD-15 — DP sync fantasma de audio.
13. AUD-17 — race de sLogFile.
14. AUD-26/AUD-23 — auth LAN + idle reaper.
15. AUD-28/AUD-29/AUD-30 — semgrep paths, codegen writes, test sleeps.
16. AUD-31/AUD-32 — cancelación shader install, verificación updater.

**P3:** AUD-22, 24, 25, 27, 33-48.

## 11. División en PRs seguros

- **PR-1 (gitignore+assets):** `!assets/configuration/` + commit de yamls +
  gamecontrollerdb. Aislado, desbloquea CI.
- **PR-2 (workflows/packaging):** AUD-02,04,05,06,07,08 — puro path fixing,
  verificable con dry-run de cada script.
- **PR-3 (lobby security):** AUD-10,11,12 + casos de test nuevos. Un solo
  PR porque el fix de spec (AUD-09) debe ir con el filtro spec→host.
- **PR-4 (spec prod fix):** AUD-09 — puede ir dentro de PR-3 si se prefiere.
- **PR-5 (blob parsers):** AUD-13,14,34 — bounds checks con fixtures
  corruptos.
- **PR-6 (netplay robustez):** AUD-18,19,20 — más delicado, necesita tests
  de sync; separar de lo demás.
- **PR-7 (save/log/platform):** AUD-16,17,21,22.
- **PR-8 (deuda):** AUD-28,29,30,41-45 — mecánico.

## 12. Plan de regresión

- Tras PR-1: `cmake -B` en clone limpio (job CI nuevo o step que borre
  `assets/configuration` y confirme que falla hoy → pasa tras fix).
- Tras PR-3: `test-lobby-security.js` con spec+oversize+cross-version;
  correr `node-lobby.js` + `spectate_test.ps1` (debe seguir pasando).
- Tras PR-5: fixtures `.sbk`/reloc corruptos → carga falla limpio.
- Tras PR-6: `netplay_lobbytest_multi.ps1` (2p/4p), `spectate_test.ps1`,
  `netplay_lan_test.ps1`, `netplay_compat_test.ps1` (debe detectar
  asset-mod mismatch nuevo), `replay_takeover_test.ps1`.
- Estándar mínimo por fix: cada AUD con "test regresión" listado debe tener
  script o fixture que falle antes y pase después.

## 12b. Fixes aplicados tras la auditoría (misma sesión)

Verificación real posterior en §12e (build Windows completo OK, tests
ejecutados). Lo verificado originalmente en la sesión de fixes:

- **AUD-01** — `.gitignore` añade `!assets/configuration/`; el árbol de
  extracción ya es addable (pendiente `git add` + commit con el reorg).
- **AUD-02/04/05** — `release.yml`, `android.yml`, `build-uwp.yml` con rutas
  post-reorg (`platforms/android`, `tools/asset-pipeline/`,
  `assets/configuration/extraction/`).
- **AUD-03** — `repoRoot` → `../..` + candidato `out/build/` en
  `findGameArtifact`.
- **AUD-06/07/08** — los 3 packagers: ROOT corregido (los 3, incluido
  `package-windows.ps1` que el análisis marcó correcto por error),
  `creditsTextConverter` → `tools/asset-pipeline/`, `yamls/` →
  `assets/configuration/extraction/`, string corrupto 0x0F reparado,
  comentarios tras `\`/backtick movidos a línea propia.
- **AUD-09/10/11/12** — `index.js` + `node-lobby.js`: `role=spec` admitido
  en edge; reset cross-version rechazado (`version-mismatch`) salvo sala
  vacía; spec→host exige payload `SSBL `; oversize cuenta al rate limiter
  antes de dropear. **Verificado: 21/21 checks en test-lobby-security.js
  (5 casos nuevos) + fuzz OK.**
- **AUD-19** — PICK con versión/compat incompatible en el host →
  `peer_drop` + aviso en status en vez de `fail()` global; en cliente se
  mantiene `fail()` (el peer incompatible somos nosotros).
- **AUD-21** — `-ffp-contract=off` para todo `ssb64_game` (era solo
  sinf/cosf) — cierra el desync FMA PC↔ARM64.
- **AUD-28** — semgrep `paths.include` añade `**/netplay/*` en las 4 reglas.
- **AUD-29** — `generate_reloc_table.py` escribe solo si cambia;
  `generate_yamls.py` ahora escribe `.tmp` + `os.replace` por receta y
  pruna categorías obsoletas solo tras completar la generación — sin
  ventana de borrado.
- **AUD-30** — defaults `build\x64\Release`→`out\build\windows-release\Release`
  en los 10 scripts de test + `configure-lint.bat`→`out\build\lint` (vcpkg
  se auto-bootstrapa por libultraship, sin VCPKG_ROOT hardcodeado);
  `netplay_compat_test.ps1` hace polling de logs (500ms, deadline 20-30s)
  en vez de sleep fijo.
- **AUD-33** — `InsertSidebarSearch`: off-by-one real tras el insert
  (`.at(curIndex+1)`) + guard contra CVar obsoleto.
- **AUD-41** — `coroutine_test` include `src/platform`; `merge_mod.py` →
  `tools/modding/` en 3 CMakeLists de mods; `test_vpk0.py`/
  `test_extern_ids.py` root math + rutas corregidas.
- **AUD-42** — RelocFileTable regenerado con ruta repo-relativa
  (`*.us.h`/`*.jp.h` + `.cpp`).

Resuelto tras la segunda/tercera tanda: AUD-13/14/34 (bounds-check de
parsers de blobs), AUD-15 (DP sync fantasma), AUD-16 (SRAM atómico),
AUD-17 (race sLogFile), AUD-18 (desync en pausa — pause/unpause ahora
snapshot-safe), AUD-20 (hash de todos los mods montados), AUD-22 (gu.h
`#define sinf/cosf → __sinf/__cosf`), AUD-23-27 (lobby reaper/joins/IP
throttle/LAN key/coroutine reap), AUD-29-32 (yaml tmp+replace, paths de
tests, cancelación shader install, verificación updater), AUD-34-36
(partículas/focus/paths heap), AUD-37-48 (ver tabla §12d).

## 13. Qué no se ha podido demostrar

- **Ejecución parcial (actualizado):** tras la verificación de §12e, el build
  Windows completo compila y linka; lobby tests, fixture de updater, fixture
  de yamls, test de coroutine reap y lint runner corrieron de verdad. Lo que
  sigue sin runtime: impacto observable de AUD-15/18 (mecanismo confirmado,
  desync real sin medir en match), y todo lo que requiere hardware/devkitPro
  o dos instancias en red.
- **Cobertura incompleta del engine netplay:** el explorador dedicado murió
  por rate-limit; la revisión manual cubrió FORWARD/spec/STARTM/timeouts/
  takeover/drops/pump, pero no se hizo un pase exhaustivo de
  `syNetPeerReceiveRemoteInput`, staging de paquetes, ni `netrollback.c`
  completo (se verificaron save/load/resim/mispredict, no la totalidad).
- **FT computer (CPU AI):** 65 sitios de RNG en `ftcomputer.c` no auditados
  línea a línea — determinismo bajo resim asumido por diseño, no verificado.
- **`sccommon` parcial:** solo scvsbattle/scautodemo/scmanager; el resto de
  escenas quedan fuera.
- **`sys/audio.c`, `sys/debug.c`, `sys/interp.c`, `ml/`:** no revisados en
  profundidad.
- **Libultraship:** `ScriptLoader::UnloadAll` y listeners de EventSystem en
  hot-reload — dangling callback posible, no verificado.
- **Si los tests PS1 pasan hoy:** las verificaciones de sync (1321/1298/
  1561/1610/1647 ticks) vienen del historial previo, no de corridas nuevas
  en esta auditoría.
- **Producción real del worker:** el deploy apunta a `smash-netplay
  .workers.dev` versión `e98a112c`; no se verificó que el código desplegado
  coincida con `services/netplay-lobby/src/index.js` local —
  AUD-09/10 asumen que sí (el push nunca se confirmó).
- **macOS/Linux/Android builds:** no compilados en este entorno (Windows sí —
  `BattleShip.exe` Debug linka; ver §12e). Los fallos de path macOS/Linux/
  Android siguen siendo por lectura, no por ejecución.

---

## 12c. Auditoría incremental — port de Nintendo Switch (2026-10-14)

Nueva superficie auditada tras el fix pass: `src/platform/switch/`
(SwitchImpl, context_switch.s, SwitchPerformanceProfiles),
`src/platform/coroutine/coroutine_switch.cpp`, secciones `NintendoSwitch` en
CMakeLists, `scripts/build/build-switch.sh`, `first_run.cpp`/`port.cpp`
`__SWITCH__` paths, `check_module_deps.py` + baseline, bloque Liquid
Glass en `package-macos.sh`.

### Hallazgos nuevos

| ID | Estado | Sev | Hallazgo |
|----|--------|-----|----------|
| SW-01 | CONFIRMADO | Alta | `-ffast-math` en `SWITCH_OPT_FLAGS` (CMakeLists) se aplicaba después de `-ffp-contract=off` y lo anulaba; además activa finite-math/no-signed-zeros → desync Switch↔PC/N64. **FIX:** eliminado; queda `-O3 -mtune=cortex-a57`. |
| SW-02 | CONFIRMADO | Alta | `Ship::Switch::Init/Exit` eran dead code — cero llamadores. romfs/applet/boost nunca corrían. **FIX:** llamadas añadidas en `port.cpp` tras `port_log` y antes de `return 0`. |
| SW-03 | MUY PROBABLE | Media | `pcvSetClockRate`/`appletSetCpuBoostMode` sin `pcvInitialize`/`apmInitialize` → error silencioso; `appletExit` ausente. **FIX:** inits/exits añadidos. |
| SW-04 | MUY PROBABLE | Media | `ApmCpuBoostMode_FastLoad` es boost de carga y auto-revierte — el comentario "consistent 60fps" era incorrecto. **FIX:** comentarios corregidos en .cpp/.h; re-assert periódico queda como trabajo futuro. |
| SW-05 | PROBABLE | Media | `sdmc:/switch/BattleShip/` podía no existir → `fopen` falla → log perdido. **FIX:** `mkdir` antes de `port_log_init`. |
| SW-06 | CONFIRMADO | Baja | `SwitchContext caller;` muerta en `coroutine_switch.cpp`; comentario stale `port/switch/`; `port_log.h` sin uso; `--icon=/opt/devkitpro/...` hardcodeado. **FIX:** todos corregidos; icon ahora vía `$DEVKITPRO`. |
| SW-07 | CONFIRMADO | Alta | `build-switch.sh`: `PROJECT_DIR=$SCRIPT_DIR/..` → `scripts/` (mismo bug que packagers); configure habría fallado siempre. Build dirs en raíz en vez de `out/`. **FIX:** `../..` + `out/build/switch{,-native}`. |
| SW-08 | RESUELTO (gate) | Alta | Verificado: la rama `ssb64` del fork (`3694576a`) solo añade `libultraship/tests/port_stubs.cpp` — **cero** soporte `__SWITCH__` en los 378 fuentes de LUS escaneados. Un configure Switch habría producido un .nro sin linkar. **FIX:** `Switch.cmake` escanea los fuentes de LUS en configure-time y `FATAL_ERROR` con mensaje accionable si falta el backend (verificado con `cmake -P`: el check dispara sobre el checkout actual). El target Switch queda desbloqueado automáticamente cuando el fork reciba un backend libnx. |

### Verificado correcto

- `context_switch.s` ↔ `SwitchContext`: offsets/ldp-stp verificados a mano.
- Guards `#ifdef` mutuamente excluyentes entre las 4 variantes de coroutine.
- `port_watchdog_note_yield()` existe fuera de los guards `!__SWITCH__` — sin link break.
- Baserom/TorchExternal correctamente excluidos en Switch; `first_run` da error accionable.
- `check_module_deps.py` baseline captura deuda real: `port_watchdog.h` vive en `src/application/` pero lo consumen 4 archivos de `src/platform/` → header en capa equivocada (debería ser core/platform).
- Buffering de log Switch: truncación `vsnprintf` segura, flush al 75%.
- `package-macos.sh` Liquid Glass: fallback limpio, `plutil -insert` seguro.
- Canary en `PortCoroutine` cubre el struct, **no** el stack — overflow de stack sigue siendo silent heap corruption (riesgo preventivo, stacks N64 pueden ser grandes).

### No verificado

Toolchain devkitPro no presente — el código Switch no compiló para target,
pero el configure ahora falla explícitamente (SW-08 gate) en vez de generar
un .nro roto. Semántica real de `ApmCpuBoostMode` requiere hardware.
SDL2/libnx wiring de `SDL_main` en Switch asumido correcto por la toolchain.

---

## 12d. Fixes aplicados — segunda tanda (2026-10-14)

| ID | Estado |
|----|--------|
| AUD-13 | **FIXED** — `BankParser::ctlAt()` bounds-checka cada offset contra `ctlSize`/`tblSize`; parseSeqFile/parsePackage clampean counts y validan off/len; callers rechazan `bankCount<1`/null → audio disabled limpio. fgm_unk ya no lee el count antes de `blob_size>=4`. |
| AUD-14 | **FIXED** — `portRelocFileExtentFrom()` nuevo; `portFixupSpriteBitmapData` acota `nbitmaps*16` al file extent y `max(bswap, deswizzle)` bytes al extent del buf. Deswizzle bound usa `row_bytes*actualHeight` (puede exceder `img_bytes` por el ceil por fila). |
| AUD-15 | **FIXED** — `osSpTaskStartGo` solo postea DP_FULL_SYNC para `M_GFXTASK`; audio sigue generando SP_TASK_DONE. |
| AUD-16 | **FIXED** — `port_save_write` ahora lee la imagen, parchea en RAM, escribe `.tmp` y renombra (MoveFileEx en Windows, rename POSIX) — crash mid-write deja el save anterior intacto. |
| AUD-17 | **FIXED** — `port_log`/`init`/`close`/`get_fd` bajo lock por plataforma (Interlocked / libnx Mutex / pthread). `port_exit_process` queda sin lock a propósito (un deadlock en el path de muerte es peor que un flush rasgado). |
| AUD-20 | **FIXED** — `compat_token()` hashea TODOS los mods montados (`state:name@version` o basename si manifest inválido); los archives montados shadowean assets aunque no corran script. Ramas `=="none"` eliminadas (dead code — siempre hay token). |
| AUD-18 | **FIXED** — `syNetRollbackCheckCanSave` acepta `Pause`/`Unpause` además de `Go`: el menú de pausa corre como procesos-función en el game thread (sin GObjThreads vivos; el guard `sGCThreadsActive` sigue aplicando) y todo su estado vive en BSS. Los inputs remotos sí importan en pausa (unpause START / quit A+B+R+Z del peer que pausó), así que un mispredict ahí antes rompía la cadena de snapshots → desync irrecuperable. |
| AUD-22 | **FIXED** — `PR/gu.h` ahora `#define sinf __sinf` / `cosf __cosf` bajo `#ifdef PORT`: MSVC ignoraba `#pragma weak sinf = __sinf` → ~94 call sites iban a UCRT libm → desync Windows↔otros. Comentario CMake actualizado. `sqrtf` queda en host libm a propósito (IEEE correctly-rounded). `frexp`/`ldexp`/`pow` en `n_env.c` son audio post-sim, no determinismo. |
| AUD-31 | **FIXED** — `s_generation` atómico: los workers detached de fetch/install capturan la generación y abortan en cada frontera de fase y por iteración de copia; install escribe `.part`+rename por archivo — un cancel ya no instala `.glsl` truncados ni deja la fase en `Done`. |
| AUD-32 | **FIXED** — `Sha256File` POSIX (`sha256sum`/`shasum -a 256`, strip de `LD_*` dentro del AppImage) + `DownloadedFileMatches` (size+sha256) compartido aplicado a las tres plataformas antes de instalar/abrir el payload. |
| AUD-34 | **FIXED** — parsers devuelven `bool`, `ensurePristine` rechaza blobs inválidos sin cachear, bloque duplicado corrupto eliminado. |
| AUD-35 | **FIXED** — `focus.cpp` restaura MasterVolume vía `std::atexit` antes de `Config::Save`. |
| AUD-36 | **FIXED** — `syNetReplayMakeAutoPath`/`WriteDesyncPackage` ahora dimensionan paths en heap desde `strlen(save_dir)` — imposible truncar. `metadata` zero-init si `GetReplayMetadata` falla. |
| AUD-27 | **FIXED** — `port_coroutine_serial_now()`/`port_coroutine_reap_after()` en los 4 backends (win32/posix/switch/android): cada coroutine recibe un serial monotónico, el snapshot guarda el serial vigente y `syNetRollbackLoad` cosecha las coroutines spawneadas en la timeline descartada (GObjThreads de resim dejaban fiber/ucontext + 64KB huérfanos por ocurrencia). |
| AUD-43 | **FIXED** — `TorchExternal` ya tiene `BUILD_ALWAYS TRUE` + `BUILD_BYPRODUCTS ${TORCH_EXECUTABLE}`: un submodule movido rebuildea y el sidecar no queda stale. |
| AUD-23/24/25/46 | **FIXED** (index.js + node-lobby.js en espejo) — idle reaper ~90s invocando el camino normal de `onClose` (preserva `peer_left` y limpieza de uplinks); join budget con headroom sobre una sala llena legítima para que un code-holder no la mantenga cerrada; throttle por IP en respuestas de existencia de sala (anti-oráculo); roster redactado para specs (sin endpoints `public`/`local` de players al rol menos confiable). Verificado: `test-lobby-security.js` 21/21 + `test-lobby-fuzz.js` PASS. |
| AUD-26 | **FIXED** — LAN HELLO autenticado con `SSB64_LAN_KEY` (hash FNV compartido) + rate-limit de adopción anónima; endpoints arbitrarios ya no ocupan slots. |
| AUD-37/38/39/40 | **FIXED** — warnings en mq ops fuera de coroutine; `osContInit`/query/read reportan conectividad real vía bridge LUS (la pantalla "no controller" ya puede mostrarse); canary real de stack host en `PortCoroutine` (el `0xFEDCBA98` N64 era inerte en port). |
| AUD-45/47/48 | **FIXED** — deuda doc: `.gitignore` (paths `assets/configuration/extraction`, decomp tools anclados a `decomp/tools/`), `overview.md:45` (frase corrupta), BUILDING.md (`out/build/<ver>` + presets), READMEs de mods (`src/application/port.cpp`, `src/integration/hooks/list/EngineEvent.h`), comentario de `new-worktree.{sh,ps1}`. Limitaciones takeover/spec documentadas en `netplay_online.md`. |

Notas:
- `Mutex` libnx se asume zero-init==unlocked (estático); si libnx exige `mutexInit()` explícito hay que llamarlo en `port_log_init`.
- Los límites de AUD-13 degradan a "audio deshabilitado" o "elemento vacío" — nunca crash; un `.sbk`/`.ctl` malicioso produce logs `audio_bridge: ... rejected`.

## 12e. Verificación real ejecutada (2026-10-14)

Todo lo verificable sin hardware devkitPro/hardware de red se ejecutó:

- **Build completo Windows (MSVC 14.44, VS2022, Debug):** `cmake` reconfigure + build de `out/build/verify` → **`BattleShip.exe` linkado OK**. `ssb64_game.lib` compila los ~500 TUs de decomp con todos los cambios (gu.h redirect, netreplay heap-paths, rollback serial/reap, etc.). Solo warnings preexistentes de N64 (truncaciones de puntero, `#pragma weak`/GLOBAL_ASM ignorados, `M_PI`/`RANGE`/`LENGTH` redefinidos).
- **El build real cazó 2 bugs que `/Zs` no veía:** `std::max` sin paréntesis en `netlobby.cpp:1962` (macro `max` de windows.h — solo aparece con los defines completos del proyecto) → `(std::max)`; y `run_project_lint.py` con `ROOT` apuntando a `tools/lint/` tras el reorg → `parents[2]` + `mkdir(parents=True)`.
- **AUD-22 verificado por preprocesado:** `cl /E` sobre `ftphysics.c` muestra `__cosf(transn_joint->rotate...)` en los call sites — el redirect funciona bajo MSVC.
- **AUD-27 verificado en runtime:** harness `out/coroutine_reap_test.cpp` linkeado contra `coroutine_win32.cpp` real — serial monotónico, `reap_after` destruye coroutines de la timeline descartada, las anteriores al snapshot sobreviven y completan.
- **AUD-32 verificado en runtime:** `Sha256File` (CNG) + `DownloadedFileMatches` extraídos del fuente real y testeados con fixture — correcto/rechaza size mal/rechaza digest mal/acepta sin metadata/rechaza archivo ausente (5/5).
- **AUD-29 verificado en ejecución:** fixture de `write_yaml_files` — tmp+replace sin residuo, prune de categorías stale solo post-generación, archivo preexistente intacto tras crash simulado.
- **Lobby suites frescas:** `test-lobby-security.js` 21/21, `test-lobby-fuzz.js` PASS (400 iters) desde `services/netplay-lobby/tests/`.
- **Lint runner:** `run_project_lint.py` corre end-to-end (required `module-deps` OK, `semgrep` OK — 167 archivos escaneados con paths `**/netplay/*`).
- **SW-08 verificado:** el gate de configure dispara correctamente (378 fuentes LUS escaneados, `has_switch=FALSE` → `FATAL_ERROR`).
- **`gen_sbom.py`** corre y emite CycloneDX válido desde `services/netplay-lobby/package-lock.json`.
- **Python:** `ast.parse` OK en los 7 scripts modificados/nuevos.

**Segunda ronda (post-commit `a9e98f24`):** rebuild incremental tras los cambios de `ModRegistry`/`netlobby` (`"simulation": false` exime mods cosméticos del compat hash) → `BattleShip.exe` linka limpio. `check_retired_paths.py` → clean; `test_extraction_recipes.py` → 246 recetas, jp=10719/us=10744 símbolos, 0 fallos; ambos parsean y corren.

Sigue sin ser verificable aquí: build Android (sin NDK), build Switch (sin devkitPro — ahora fail-loud en configure), tests PS1 de netplay contra dos procesos reales, y el deploy del worker (`e98a112c` vs `index.js` local).
