# O2rArchive concurrent `zip_*` access → heap corruption at shutdown

**Status**: FIXED (libultraship `O2rArchive` / `ArchiveManager` / `ResourceManager`
synchronization); the residual exe-CRT list-braiding variant is also FIXED —
the process now links a single static CRT via the `x64-windows-static`
vcpkg triplet, eliminating the dual-debug-list boundary entirely (see
bottom section).
**Date**: 2026-10-16
**Crash signature**: `0xC0000374` (`STATUS_HEAP_CORRUPTION`) or `0xC0000005`,
inside `zip.dll → ucrtbased.dll free`, during process shutdown:
`~ResourceManager → ~ArchiveManager → ~O2rArchive → Close → zip_close`.

## Symptoms

Windows Debug exits crashed ~50% of runs — including runs that had already
produced correct output (menu screenshots, replays). `SSB64_SAFE_MODE=1` did
not help; the crash persisted with only 10 frames executed, so the corrupting
access happens during startup/asset streaming, not gameplay.

## Root cause

`ResourceManager` runs a `BS::thread_pool` (`hw_concurrency - 1` workers) whose
tasks call `LoadFileProcess` → `ArchiveManager::LoadFile` →
`O2rArchive::LoadFile` → `zip_name_locate`/`zip_fopen_index`/`zip_fread`/
`zip_fclose` on the **same `zip_t*` with no synchronization**. libzip archives
are not thread-safe: concurrent calls share the archive's internal entry/
source/error state, so workers intermittently stomped libzip-owned heap
blocks. The stomp went unnoticed until `zip_close()` freed the poisoned
structures at process exit — the crash site was the detector, not the writer.

Contributing defects found along the way:

- `ArchiveManager::LoadFile` / `GetArchiveFromFile` used
  `mFileToArchive[hash]` — `operator[]` *inserts* a null entry on miss, a
  concurrent mutation on every lookup, and it made `HasFile()` (`.count()`)
  report phantom files for any hash ever queried.
- `ResourceManager::LoadResourceProcess` wrote
  `mResourceCache[identifier]` outside `mMutex`, and `UnloadResource` called
  `mResourceCache.contains()` unlocked — same bucket-table race.
- Member destruction order put `mMutex` **before** `mThreadPool`'s join:
  `~thread_pool` waits for tasks that may block on a mutex already destroyed.
- `O2rArchive::WriteFile`'s error path `zip_discard(mZipArchive)` left the
  pointer dangling — a later `Close()` would `zip_close` freed memory.
- `HashToString` results were dereferenced without a null check in all three
  archive classes (`O2rArchive`, `OtrArchive`, `FolderArchive`).

## Fix

- `O2rArchive`: `std::mutex` serializing `LoadFile`/`Open`/`Close`/`WriteFile`
  (every function touching `mZipArchive`).
- `ArchiveManager`: `mutable std::recursive_mutex` guarding `mArchives`,
  `mHashes`, `mFileToArchive`, `mDirectories`, `mGameVersions`,
  `mValidGameVersions`; lookups switched from `operator[]` to `find`.
  Recursive because `Load()`/`ResetVirtualFileSystem()`/`HashToString()`
  re-enter on the same thread. Lock order: manager → archive, never reversed.
- `ResourceManager::~ResourceManager` drains the pool (`mThreadPool->wait()`)
  before member teardown begins.
- `mResourceCache` mutation/`contains` moved under `mMutex`.
- `WriteFile` clears `mZipArchive` after `zip_discard`; `LoadFile(uint64_t)`
  null-checks `HashToString` in all three archive classes.

## Diagnosis trail (kept for the next flaky heap bug)

1. Crash bundles in `logs/crashes/crash-*/previous.log` already contain the
   native backtrace — resolve `BattleShip.exe+0xNNN` offsets with
   `llvm-symbolizer --obj BattleShip.exe <base+offset>` (RVA → add image base).
2. `_CRTDBG_CHECK_ALWAYS_DF` and ASAN (`SSB64_ENABLE_ASAN=ON`, dev-only) did
   NOT reproduce: ASAN forces `/MD` (shared CRT) and replaces the allocator,
   which perturbs the racing layout enough to hide it — a clean ASAN run does
   not exonerate a thread race.
3. `zip.dll`/`SDL2d.dll` etc. are `/MDd` (ucrtbased) while the exe is `/MTd`
   (static debug CRT) — two CRT debug heaps in one process; cross-CRT block
   ownership confusion was ruled out (no ownership crosses the boundary), but
   it remains a hazard class for any future ownership-passing API.

## Residual variant — exe `/MTd` debug list braids with ucrtbased (FIXED)

**Resolution**: unified the CRT. The build now links every vcpkg dependency
(SDL2, fmt, spdlog, libzip, tinyxml2, zlib, …) from the **`x64-windows-static`
triplet**, so the whole process — exe plus all libraries — shares the one
statically-linked `/MTd` CRT. With a single debug block list there is no
foreign list to braid into, and no CRT-owned pointer can cross a module
boundary. `BattleShip.exe` now imports only system DLLs (no `ucrtbased.dll`,
no `MSVCP*D`, no SDL/zip DLLs) — verified via the import table — and 16
consecutive uninstrumented Debug runs boot, draw the menu, and shut down
cleanly.

**Root cause confirmed live**: a `GWATCH` hardware watchpoint on the exe's
`__acrt_first_block`/`__acrt_last_block` anchors captured the braiding store
directly — `exe.__acrt_first_block <= <a live ucrtbased-list node>` written
by the exe CRT's own `free_dbg_nolock` unlink path. A free on a foreign
**head** block (`prev == NULL`) makes the unlink write
`first_block = h->next` into the *wrong CRT's* anchor; subsequent exe
allocations then prepend onto a ucrt node, interleaving the two lists.

**Why earlier checks missed it**: `HeapValidate`-based foreign-heap checks
were structurally blind — the exe's `/MTd` `__acrt_heap`, ucrtbased's
`__acrt_heap`, and `GetProcessHeap()` were **the same NT heap handle**
(logged identical). A ucrt block validates fine on "both" heaps because
there is only one. The working detector was *list membership*, not heap
ownership: a free where `h->prev == NULL` but `h != *exe_first_block`
means the exe CRT is freeing a head of someone else's list — vetoing it
kept runs alive and identified the victims as ucrt-owned strings
(`"General Settings"` etc.) returned across the DLL boundary
(`fmt::format`/`strdup`-style buffers) and destroyed by the exe's CRT.

The menu-path `SIGABRT` observed during this work was unrelated: an
`AddWidget` sidebar-registration assert plus a `ConsoleVariable::GetString`
NULL-string crash — both fixed separately.

After the thread-safety fix above, the same ~50% shutdown crash persisted
under a different shape. `_CrtSetAllocHook` instrumentation on **both** CRTs
(`SSB64_HEAP_HOOK=1`, plus `SSB64_HEAP_CHECK`/`TRACE`/`XCHECK`/`GWATCH` —
see `src/core/observability/heap_check_boot.cpp`) found:

- `zip_close` itself now succeeds cleanly — a pre-close autopsy walked all
  ~167 K live ucrt debug blocks with zero link/gap violations and every free
  inside the `zip_close` window (~42 700 frees) validated.
- The crash moved to the **exe's own static-CRT debug list**: teardown frees
  inside `free_dbg_nolock` (debug_heap.cpp:982, the `prev->next = next` /
  `next->prev = prev` unlink) hit an access violation because a live block's
  `next` points into freed/decommitted memory.
- Vetoed frees show the two debug lists **braided**: one victim's `next->prev`
  pointed at an ucrtbased `per_thread_data.cpp:245` block (`use=2`,
  request number in ucrt's sequence) that believed itself a list head
  (`prev == NULL`). Worker-thread per-thread data is freed by ucrtbased at
  thread exit — a block that only exists inside ucrt's list ended up as the
  exe list's head, so ucrt freeing it leaves exe `next` pointers dangling.
- `HeapValidate` cross-checks on every free of both CRTs always returned
  clean — misleadingly so: both CRTs share the *same NT heap handle* (see
  the confirmed mechanism above), so heap-handle checks cannot distinguish
  ownership. The braiding free turned out to be a plain `free()` of a
  ucrtbased head block through the exe CRT — invisible to `HeapValidate`,
  caught instead by list-membership (`h->prev == NULL` while
  `h != *exe_first_block`) and by `GWATCH` watching the written value.
- A hardware-watchpoint run (`SSB64_HEAP_GWATCH`) on the exe anchors saw
  the braid written by a legitimate CRT unlink — the wild-store and
  `_aligned_malloc` hypotheses are ruled out.
- With vetoing hooks active, runs survive to clean exit — the vetoes keep
  the braided nodes out of the unlink path, which also confirms the crash
  is a *detection* of pre-existing list damage, not the writing of it.

Debug-only artifact: the `_CrtMemBlockHeader` list does not exist in
Release builds (`/MT`/`/MD`), so shipping builds cannot fail this way.

**Post-static verification (2026-10-17, instrumented)**: a full-lifecycle
run of the static-triplet build under the complete diagnostic suite
(`SSB64_HEAP_HOOK`+`CHECK`+`WATCH`+`GWATCH`) passed cleanly — every
`post-loadfile`/`pre-zip_close` probe reported `heap_ok=1`, zero braid
events on the exe anchor, and every hardware-watchpoint hit on watched
`prev` fields resolved via `llvm-symbolizer` to legitimate CRT writes
(`free_dbg_nolock` unlink at debug_heap.cpp:983, the `0xDD` landfill
stamp at :1001). The process reached `SSB64_MAX_FRAMES=400` and exited
0 through `PortGameShutdown` — the historical `free_dbg_nolock+0x6f6`
crash site is quiet.

## Audit hook

A crash inside a third-party DLL at process exit is usually the *detector*,
not the culprit — search for unsynchronized access to that library's objects
upstream of the crash, and distrust clean sanitizer runs on race-shaped bugs.
