# src/platform/compatibility — decomp-on-modern-toolchain shims

Stubs and fixups that let the N64 decomp run under a modern LP64 host:
libc/N64 symbol stubs (`libc_compat`, `n64_stubs`, `audio_stubs`,
`debug_stubs`), animation-object fixups (`port_aobj_fixup`), and the
linker segment-symbol table (`segment_symbols`).

## Allowed dependencies

decomp headers, `observability` (port_log). Nothing else — this layer
must stay closer to the metal than to features.

## Tests

Covered indirectly by every ROM-gated run; dedicated entries live in
`tests/unit/` when a shim grows real logic.
