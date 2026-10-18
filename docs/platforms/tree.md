# platforms/ — per-platform entry points

Platform support is split across three places by design:

| What | Where |
|------|-------|
| App wrappers / manifests / IDE projects | `platforms/<name>/` (here) |
| CMake platform logic | `platforms/<name>/cmake/<Name>.cmake` |
| OS-abstracted runtime code | `src/platform/` |
| Build/test/package entrypoints | `scripts/{build,testing,packaging}/` |

Each `<name>/README.md` lists the pieces for that platform.
