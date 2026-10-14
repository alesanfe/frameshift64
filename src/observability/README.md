# src/observability — logging infrastructure

`port_log.{c,h}` — the process-wide logger. Locked across platforms
(Interlocked/libnx Mutex/pthread), JSONL mirror via `SSB64_LOG_JSON`,
sinks for UI capture. Leaf module: nothing here may include product
code — decomp sources call `port_log()` directly.
