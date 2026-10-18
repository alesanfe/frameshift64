# Android

| Piece | Location |
|-------|----------|
| Gradle wrapper + Activity | `platforms/android/app/` (here) |
| Native JNI bridges | `src/platform/android/` (torch_runner, touch overlay) |
| Coroutine asm | `src/platform/coroutine/coroutine_{aarch64,armv7}.S` |
| Env/test scripts | `scripts/development/` |

`libtorch_runner.so` extracts ROM on-device; downloads go through
`src/platform/http/download_service.cpp` (JNI → HttpURLConnection tasks in
`BattleShipActivity`).
