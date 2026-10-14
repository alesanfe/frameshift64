# Environment variable reference

All `SSB64_*` environment variables read by the binaries and tools,
classified. Three categories:

- **Config** — user-facing / runtime behavior. Stable; changes need a
  compat note.
- **Diagnostic** — read-only observation (logs, dumps, traces). Safe in
  release: they observe, never mutate gameplay or weaken validation.
- **Test hook (gated)** — test-harness knobs that alter gameplay, inject
  traffic, weaken validation, or hijack boot. Compiled only when
  `-DSSB64_TEST_HOOKS=ON` (default ON for dev builds, forced OFF by
  `platforms/*/packaging/package-*`).

This file is enforced by `tools/lint/check_env_vars.py` (wired into
`repo-checks.yml`): any env read whose literal `SSB64_*` name is not listed
here fails CI. Brace groups (`X_{A,B}`) expand; write full names otherwise.

When adding a variable: put it in the right category here. If setting it can
mutate game state, inject bytes onto the wire, accept what validation would
reject, or bypass the normal boot path, gate it on `SSB64_TEST_HOOKS` and
verify `strings(release binary)` does not contain its name.

## Netplay / session (config — ship)

| Var | Effect |
|-----|--------|
| `SSB64_NETPLAY` | enable netplay mode |
| `SSB64_NETPLAY_{HOST,PEER,BIND,BOOTSTRAP}` | bootstrap endpoints |
| `SSB64_NETPLAY_SESSION` | session id override |
| `SSB64_NETPLAY_SEED` | deterministic session seed |
| `SSB64_NETPLAY_ROLLBACK` | rollback on/off (default on) |
| `SSB64_NETPLAY_DELAY` | input delay frames |
| `SSB64_NETPLAY_{LOCAL_PLAYER,REMOTE_PLAYER}` | fixed slot ids for direct-connect |
| `SSB64_LAN_{HOST,JOIN}` | direct-LAN sessions |
| `SSB64_LOBBY_{HOST,JOIN,SPECTATE,AUTOSTART,MIN_PLAYERS,FKIND,STAGE,CODE_FILE,FORCE_RELAY}` | lobby bootstrap + automation |

## Diagnostics (ship)

| Var | Effect |
|-----|--------|
| `SSB64_NETPLAY_DEBUG_STAGES` | stage-gated packet logs |
| `SSB64_NETPLAY_CONFIRMED_TRACE` | per-tick confirmed-state trace file |
| `SSB64_ROLLBACK_TRACE_TICK` | rollback trace window |
| `SSB64_SAVE_PATH` | save location override (CI/debug) |
| `SSB64_NO_UPDATE_CHECK` | disable updater |
| `SSB64_LOG_PATH` | log destination |
| `SSB64_LOG_JSON` | mirror every log line as JSONL (`{"t","sid","lvl","cat","msg"}`) to `<log>.jsonl` — structured events for tooling; `token=`/`key=`/`secret=`/`password=` values and the home-dir prefix are redacted in the mirror |
| `SSB64_MUTE` | silence audio |
| `SSB64_INTERP_{FPS,LOG,SNAP_DIST}` | frame-interpolation tuning/dump |
| `SSB64_{DUMP_DRAWS,DUMP_ALL_FIGATREE,DUMP_FILE_ID,STAGE_AUDIT,TEX_DUMP,TEX_FIXUP_LOG,TEX_FIXUP_LOG_FILE_ID}` | asset/render dump switches |
| `SSB64_{DUMP_SYNTH_RELOC,DUMP_SYNTH_RELOC_FILE_ID,SYNTH_INSPECT}` | synthetic-reloc synthesis dumps/inspect JSON |
| `SSB64_{RCP_COOLDOWN,RCP_CYCLE_BUDGET,RCP_FORCE_N,RCP_LOG,RCP_RECT_GATE,RCP_TRI_WEIGHT_PCT}` | RCP workload tuning/logging |
| `SSB64_RENDERDOC_{DIR,FRAMES}` | RenderDoc capture trigger |
| `SSB64_SCREENSHOT_{DIR,FRAMES}` | screenshot capture |
| `SSB64_{TRACE_SWITCH_CTX,TRACE_INTRO_ANIM,TRACE_WALLPAPER_PERSP}` | execution traces |
| `SSB64_{DIAG_ARM_WALLPAPER_N,DIAG_ARM_WALLPAPER_SCENES,RENDER_DIAG,RENDER_DIAG_FILE_ID,RENDER_DIAG_LIMIT}` | render diagnostics |
| `SSB64_RELOC_{FROMSOURCE,GEN_SEED}` | reloc pipeline diagnostics |
| `SSB64_LOG_LBRELOC_LOAD` | reloc load log |
| `SSB64_{AUDIO_OUTPUT_FILTER,RESAMPLE_DUMP,ADPCM_DUMP}` | audio diagnostics |
| `SSB64_{FREEZE_PACING,GAME_THREAD_CAP_RESUMES,GFX_DEFER_VI}` | pacing/frame diagnostics |
| `SSB64_{ACMD_TRACE,ACMD_TRACE_DIR,ACMD_TRACE_TASKS}` | `tools/debugging/acmd_trace` |
| `SSB64_GBI_TRACE{,_DATA,_DIR,_FRAMES,_START}` | `tools/debugging/gbi_trace` |
| `SSB64_{VERSION,APP_NAME}` | build/region identity |

## Test hooks (gated — `SSB64_TEST_HOOKS=ON` only)

| Var | Effect |
|-----|--------|
| `SSB64_NETPLAY_MUTANT` | `accept_bad_session` / `accept_bad_checksum` / `no_forward_guard` |
| `SSB64_NETPLAY_PACKETFUZZ` | inject hostile datagrams through real parser |
| `SSB64_NETPLAY_TEST_DESYNC` | RNG corruption at tick N |
| `SSB64_ROLLBACK_SNAPSHOT_STRIDE` | snapshot every Nth tick instead of every tick (test hook; default 1) |
| `SSB64_NETPLAY_DROP_MS` | override remote-drop timeout (10s default) for same-machine rigs |
| `SSB64_NETPLAY_FUZZ` | deterministic synthetic inputs on local slots |
| `SSB64_NETPLAY_SIM_{LATENCY_MS,JITTER_MS,LOSS_PCT,DUP_PCT,REORDER_PCT,REORDER_WINDOW_MS,CORRUPT_PCT,SEED}` | seeded impairment sim in `udp_transport` |
| `SSB64_COMPAT_TOKEN` | forge lobby compat token |
| `SSB64_ROLLBACK_{AUTOVS,SYNCTEST,SEED,STAGE,FIGHTERS,MINUTES,MUTANT}` | rollback/determinism rig |
| `SSB64_RIG_EXIT` | exit code = test verdict |
| `SSB64_REPLAY_{RECORD,RECORD_FRAMES,PLAY,TAKEOVER}` | input replay harness / mid-replay live takeover |
| `SSB64_SYNC_{VERIFY,TRACE,DUMP_TICK}` | per-tick field-grouped hash oracle |
| `SSB64_FORCE_{ITEM_KIND,CONTAINER_DROP}` | force item spawns |
| `SSB64_START_SCENE`, `SSB64_SPGAME_{STAGE,FKIND}`, `SSB64_COOP_{P2,P2_FKIND}` | boot/scene/co-op overrides |
| `SSB64_NO_MOTIONFIX` | disable the demo-script reloc fix (bisect aid) |
| `SSB64_SCENE_HEAP_FREE` | arena free+malloc per scene transition (ASan bait) |
| `SSB64_ADDR_JITTER` | shift process heap layout for local-peer tests |
| `SSB64_MAX_FRAMES` | auto-shutdown after N frames |
| `SSB64_HEADLESS` | run full sim + netplay without rendering (display lists dropped, 60 Hz timer pacing) — multi-instance rigs |
| `SSB64_WIZARD_AUTOCANCEL` | auto-cancel the first-run wizard at frame N |
| `SSB64_SYNTH_SELFTEST` | deblob synthesis validation harness; exits before game boot |
| `SSB64_FUZZ_{PARSER,INPUT}` | parser fuzz entry (manifest/replay/json); parse file then exit — driven by `tools/fuzz/run_parser_fuzz.py` |

*Policy:* gating rule = "does setting this mutate gameplay, let a
packet/peer/session be accepted that correct validation would reject, inject
bytes into the wire, or bypass normal boot?" → `SSB64_TEST_HOOKS`.
Observation-only and user-facing config stay available in shipped builds.
