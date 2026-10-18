#ifndef LBRELOC_DEBUG_H
#define LBRELOC_DEBUG_H

/* Internal interface to lbreloc_byteswap_debug.cpp — the env-gated audit
 * and texture-dump tooling that observes the byteswap + fixup pipeline.
 * For the lbreloc_byteswap / lbreloc_fixup_* TUs only; the extern "C"
 * entry points consumed by decomp code stay declared in lbreloc_byteswap.h. */

#include <stddef.h>
#include <stdint.h>

struct StageAuditCounters {
	uint32_t vtx_scan_caught      = 0;
	uint32_t vtx_scan_skipped_seg = 0;
	uint32_t settimg_scan_caught  = 0;
	uint32_t settimg_scan_skipped_seg = 0;
	uint32_t chain_vtx_fixups     = 0;
	uint32_t chain_settimg_fixups = 0;
	uint32_t runtime_vtx_fixups   = 0;
	uint32_t runtime_tex_fixups   = 0;
};

// SSB64_STAGE_AUDIT=1 — per-file counters, emitted via portStageAuditEmitLoadSummary.
bool stage_audit_enabled();
void stage_audit_reset_per_file();
StageAuditCounters &stage_audit();

// SSB64_TEX_FIXUP_LOG=1 — one line per applied/skipped fixup in debug_traces/tex_fixup.log.
// tex_log_enabled()/tex_dump_enabled() let the hot path skip byte collection
// entirely when the tooling is off.
bool tex_log_enabled();
bool tex_dump_enabled();
void tex_log_emit(const char *path, int file_id,
                  uint32_t file_off, uint32_t size_bytes,
                  int fmt, int siz, int bpp,
                  const void *first_bytes,
                  const char *note);

// SSB64_TEX_DUMP=1 — TGA dumps under tex_dump/.
void tex_dump_chain(int file_id, uint32_t file_off,
                    int fmt, int siz, uint32_t bpp,
                    const uint8_t *src, size_t src_bytes);
void tex_dump_known_dims(int file_id, uint32_t file_off,
                         int fmt, int siz, int bpp,
                         const uint8_t *src, size_t src_bytes,
                         uint32_t w, uint32_t h);

#endif // LBRELOC_DEBUG_H
