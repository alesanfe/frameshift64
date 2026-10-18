/**
 * lbreloc_byteswap_debug.cpp — env-gated audit + texture-dump tooling for
 * the reloc byteswap pipeline (split out of lbreloc_byteswap.cpp).
 *
 *   SSB64_STAGE_AUDIT=1          per-file fixup counters, one summary line
 *   SSB64_TEX_FIXUP_LOG=1        per-fixup log in debug_traces/tex_fixup.log
 *   SSB64_TEX_FIXUP_LOG_FILE_ID  filter the log to a single reloc file id
 *   SSB64_TEX_DUMP=1             TGA dumps of fixed textures under tex_dump/
 *
 * Nothing here runs unless its env gate is set — all entry points early-out
 * on the disabled path.
 */

#include "lbreloc_debug.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" void port_log(const char *fmt, ...);

#include "RelocFileTable.h"  // gRelocFileTable (generated) for audit path names

// ============================================================
//  Stage audit (SSB64_STAGE_AUDIT=1)
// ============================================================
//
// Per-file counters for the byteswap + reloc-chain fixup pipeline.
// Reset at the top of portRelocByteSwapBlob; emitted as a single
// port_log line by portStageAuditEmitLoadSummary after the reloc
// chain walk completes. Helps identify stage files whose G_VTX /
// G_SETTIMG targets the DL scan skipped AND the chain walk failed
// to recover.

static int sStageAuditState = 0;  // 0=unchecked, 1=on, -1=off

static StageAuditCounters sStageAudit;
StageAuditCounters &stage_audit() { return sStageAudit; }
static uint64_t sStageAuditRuntimeVtxTotal = 0;
static uint64_t sStageAuditRuntimeTexTotal = 0;
/* Total dispatches regardless of reloc-file membership vs heap-only. */
static uint64_t sStageAuditRuntimeVtxDispatches = 0;
static uint64_t sStageAuditRuntimeVtxHeap = 0;
static uint64_t sStageAuditRuntimeTexDispatches = 0;
static uint64_t sStageAuditRuntimeTexHeap = 0;

bool stage_audit_enabled()
{
	if (sStageAuditState == 0) {
		const char *e = std::getenv("SSB64_STAGE_AUDIT");
		sStageAuditState = (e != nullptr && e[0] == '1') ? 1 : -1;
	}
	return sStageAuditState == 1;
}

void stage_audit_reset_per_file()
{
	sStageAudit = StageAuditCounters{};
}

/* Count u32 words at 8-byte-aligned (w0) positions whose high byte matches
 * the given opcode — diagnostic for F3D vs F3DEX2 discrimination. */
static uint32_t count_opcode_matches(const void *data, size_t size, uint8_t opcode)
{
	const uint32_t *words = static_cast<const uint32_t *>(data);
	size_t n = size / 4;
	uint32_t hits = 0;
	for (size_t i = 0; i + 1 < n; i += 2) {
		if (((words[i] >> 24) & 0xFF) == opcode) hits++;
	}
	return hits;
}

/* Same as count_opcode_matches but steps by 1 word, so it also catches
 * commands whose (w0, w1) pair lands at an odd word offset within the file. */
static uint32_t count_opcode_matches_any_phase(const void *data, size_t size, uint8_t opcode)
{
	const uint32_t *words = static_cast<const uint32_t *>(data);
	size_t n = size / 4;
	uint32_t hits = 0;
	for (size_t i = 0; i + 1 < n; i += 1) {
		if (((words[i] >> 24) & 0xFF) == opcode) hits++;
	}
	return hits;
}

extern "C" void portStageAuditEmitLoadSummary(unsigned int file_id, const char *path, size_t size)
{
	if (!stage_audit_enabled()) return;
	port_log("[STAGE_AUDIT] file=%3u size=0x%06zx vtx_scan=%u/%u settimg_scan=%u/%u chain=vtx:%u,st:%u path=%s\n",
	         file_id, size,
	         sStageAudit.vtx_scan_caught, sStageAudit.vtx_scan_skipped_seg,
	         sStageAudit.settimg_scan_caught, sStageAudit.settimg_scan_skipped_seg,
	         sStageAudit.chain_vtx_fixups, sStageAudit.chain_settimg_fixups,
	         path ? path : "(null)");
	stage_audit_reset_per_file();
}

/* Separate entry point for opcode-census diagnostic. Call after the load
 * summary from the bridge. */
extern "C" void portStageAuditEmitOpcodeCensus(unsigned int file_id, const char *path,
                                                const void *data, size_t size)
{
	if (!stage_audit_enabled() || data == nullptr || size < 8) return;
	/* F3D: G_VTX=0x01, G_MTX=0x01 is ALSO MTX-first in F3DEX2 — use the
	 * two cases that diverge most strongly to tell the formats apart.
	 * F3D     : G_VTX=0x01  G_ENDDL=0xB8  G_DL=0x06
	 * F3DEX2  : G_VTX=0x04  G_ENDDL=0xDF  G_DL=0x06
	 */
	uint32_t f3d_vtx    = count_opcode_matches(data, size, 0x01);
	uint32_t f3d_enddl  = count_opcode_matches(data, size, 0xB8);
	uint32_t f3dex2_vtx = count_opcode_matches(data, size, 0x04);
	uint32_t f3dex2_end = count_opcode_matches(data, size, 0xDF);
	/* Any-phase variants: step by 1 word rather than 2. If DL data is at
	 * an odd-word offset within the file, the even-phase scan misses every
	 * command. The difference between *_any and the scan counts flags that. */
	uint32_t f3d_vtx_any    = count_opcode_matches_any_phase(data, size, 0x01);
	uint32_t f3d_end_any    = count_opcode_matches_any_phase(data, size, 0xB8);
	uint32_t f3dex2_vtx_any = count_opcode_matches_any_phase(data, size, 0x04);
	uint32_t f3dex2_end_any = count_opcode_matches_any_phase(data, size, 0xDF);
	port_log("[STAGE_AUDIT_GBI] file=%3u f3d:vtx=%u(%u),end=%u(%u)  f3dex2:vtx=%u(%u),end=%u(%u)  path=%s\n",
	         file_id, f3d_vtx, f3d_vtx_any, f3d_enddl, f3d_end_any,
	         f3dex2_vtx, f3dex2_vtx_any, f3dex2_end, f3dex2_end_any,
	         path ? path : "(null)");

	/* Stage-file deep inspection: byte histogram + first bytes to understand layout. */
	if (path != nullptr && std::strstr(path, "reloc_stages/Stage") != nullptr) {
		const uint8_t *p = static_cast<const uint8_t *>(data);
		/* Byte-0 histogram: count occurrences at each (byte_offset % 4) position. */
		uint32_t byte0_hits[256] = {0};
		uint32_t byte3_hits[256] = {0};
		for (size_t i = 0; i + 3 < size; i += 4) {
			byte0_hits[p[i]]++;
			byte3_hits[p[i + 3]]++;
		}
		port_log("[STAGE_AUDIT_HIST] file=%3u  byte0: 0x04=%u 0xDF=%u 0xF3=%u 0xFD=%u 0xB8=%u  byte3: 0x04=%u 0xDF=%u 0xF3=%u 0xFD=%u 0xB8=%u\n",
		         file_id,
		         byte0_hits[0x04], byte0_hits[0xDF], byte0_hits[0xF3], byte0_hits[0xFD], byte0_hits[0xB8],
		         byte3_hits[0x04], byte3_hits[0xDF], byte3_hits[0xF3], byte3_hits[0xFD], byte3_hits[0xB8]);
		size_t dump = size < 128 ? size : 128;
		char hex[512];
		size_t pos = 0;
		for (size_t i = 0; i < dump && pos + 4 < sizeof(hex); i++) {
			pos += std::snprintf(hex + pos, sizeof(hex) - pos, "%02X", p[i]);
			if ((i & 3) == 3 && pos + 1 < sizeof(hex)) hex[pos++] = ' ';
		}
		hex[pos] = '\0';
		port_log("[STAGE_AUDIT_HEX] file=%3u first128=%s\n", file_id, hex);
	}
}

extern "C" void portStageAuditNoteRuntimeVtx(unsigned int num_vtx)
{
	if (!stage_audit_enabled()) return;
	sStageAuditRuntimeVtxTotal += num_vtx;
}

extern "C" void portStageAuditNoteRuntimeTex(unsigned int num_bytes)
{
	if (!stage_audit_enabled()) return;
	sStageAuditRuntimeTexTotal += num_bytes;
}

/* Per-file dispatch counters so we can see exactly which reloc files
 * supply vertex data during gameplay. */
static std::unordered_map<int, uint64_t> sStageAuditVtxPerFile;
static std::unordered_map<int, uint64_t> sStageAuditTexPerFile;

static void stage_audit_dump_per_file_vtx()
{
	/* Sort by count desc, emit as one line. */
	std::vector<std::pair<int, uint64_t>> v(sStageAuditVtxPerFile.begin(), sStageAuditVtxPerFile.end());
	std::sort(v.begin(), v.end(), [](auto &a, auto &b){ return a.second > b.second; });
	for (auto &p : v) {
		const char *path = (p.first >= 0) ? gRelocFileTable[p.first] : "(heap)";
		port_log("[STAGE_AUDIT] vtx_per_file: file=%4d hits=%llu path=%s\n",
		         p.first, (unsigned long long)p.second, path ? path : "(null)");
	}
}

extern "C" void portStageAuditNoteVtxDispatch(int in_reloc_file)
{
	if (!stage_audit_enabled()) return;
	sStageAuditRuntimeVtxDispatches++;
	if (!in_reloc_file) sStageAuditRuntimeVtxHeap++;
	if ((sStageAuditRuntimeVtxDispatches & 0xFFF) == 0) {
		port_log("[STAGE_AUDIT] runtime: vtx_disp=%llu vtx_heap=%llu vtx_fix=%llu  tex_disp=%llu tex_heap=%llu tex_fix=%llu\n",
		         (unsigned long long)sStageAuditRuntimeVtxDispatches,
		         (unsigned long long)sStageAuditRuntimeVtxHeap,
		         (unsigned long long)sStageAuditRuntimeVtxTotal,
		         (unsigned long long)sStageAuditRuntimeTexDispatches,
		         (unsigned long long)sStageAuditRuntimeTexHeap,
		         (unsigned long long)sStageAuditRuntimeTexTotal);
		stage_audit_dump_per_file_vtx();
	}
}

extern "C" void portStageAuditNoteVtxDispatchFile(int file_id)
{
	if (!stage_audit_enabled()) return;
	sStageAuditVtxPerFile[file_id]++;
}

extern "C" void portStageAuditNoteTexDispatchFile(int file_id)
{
	if (!stage_audit_enabled()) return;
	sStageAuditTexPerFile[file_id]++;
}

extern "C" void portStageAuditNoteTexDispatch(int in_reloc_file)
{
	if (!stage_audit_enabled()) return;
	sStageAuditRuntimeTexDispatches++;
	if (!in_reloc_file) sStageAuditRuntimeTexHeap++;
}

// ============================================================
//  Texture-fixup diagnostic log (SSB64_TEX_FIXUP_LOG=1)
// ============================================================
//
// One-line entry per byteswap-side texture fixup invocation. See
// portRelocTexFixupLog() in lbreloc_byteswap.h for the user-facing knobs.
// All entries are interleaved into debug_traces/tex_fixup.log so chain /
// pass2 / runtime / heap / sprite paths can be diffed and grepped together.
//
// Initialization is lazy and once-only. The log is opened on the first call
// after the env var has been read.

static int  sTexLogState = 0;        // 0=uninit, 1=enabled, -1=disabled
static FILE *sTexLogFile = nullptr;
static int  sTexLogFileFilter = -1;  // -1 = no filter

static void tex_log_init_once()
{
	if (sTexLogState != 0) return;
	const char *env = std::getenv("SSB64_TEX_FIXUP_LOG");
	if (env == nullptr || env[0] == '0' || env[0] == '\0') {
		sTexLogState = -1;
		return;
	}
	sTexLogFile = std::fopen("debug_traces/tex_fixup.log", "w");
	if (sTexLogFile == nullptr) {
		// Fall back to stderr so we never silently swallow the request.
		sTexLogFile = stderr;
	}
	const char *fenv = std::getenv("SSB64_TEX_FIXUP_LOG_FILE_ID");
	if (fenv != nullptr && fenv[0] != '\0') {
		sTexLogFileFilter = (int)std::strtol(fenv, nullptr, 0);
	}
	sTexLogState = 1;
	std::fprintf(sTexLogFile,
		"# tex_fixup.log — SSB64 texture byteswap-fixup trace\n"
		"# columns: [path] file=<id> off=0x<file_off> size=<bytes> "
		"fmt=<n> siz=<n> bpp=<n> first16=<hex> note=<...>\n");
	std::fflush(sTexLogFile);
}

bool tex_log_enabled()
{
	if (sTexLogState == 0) tex_log_init_once();
	return sTexLogState == 1;
}

static void tex_log_first16(char *out, size_t out_sz, const void *data, size_t bytes)
{
	if (data == nullptr || bytes == 0 || out_sz < 3) {
		if (out_sz > 0) out[0] = '-';
		if (out_sz > 1) out[1] = '\0';
		return;
	}
	size_t n = bytes < 16 ? bytes : 16;
	const uint8_t *p = static_cast<const uint8_t *>(data);
	size_t pos = 0;
	for (size_t i = 0; i < n && pos + 3 < out_sz; i++) {
		std::snprintf(out + pos, out_sz - pos, "%02X", p[i]);
		pos += 2;
	}
	if (pos < out_sz) out[pos] = '\0';
}

// Centralized emit. Caller has already pre-formatted everything except the
// optional first16 hex (which we never want to compute when the log is off).
void tex_log_emit(const char *path, int file_id,
                         uint32_t file_off, uint32_t size_bytes,
                         int fmt, int siz, int bpp,
                         const void *first_bytes,
                         const char *note)
{
	if (!tex_log_enabled()) return;
	if (sTexLogFileFilter >= 0 && file_id != sTexLogFileFilter) return;

	char hex[40];
	tex_log_first16(hex, sizeof(hex), first_bytes, size_bytes);

	char fmt_buf[16] = "?";
	if (fmt >= 0) std::snprintf(fmt_buf, sizeof(fmt_buf), "%d", fmt);
	char siz_buf[16] = "?";
	if (siz >= 0) std::snprintf(siz_buf, sizeof(siz_buf), "%d", siz);
	char bpp_buf[16] = "?";
	if (bpp >= 0) std::snprintf(bpp_buf, sizeof(bpp_buf), "%d", bpp);
	char file_buf[24];
	if (file_id < 0) std::snprintf(file_buf, sizeof(file_buf), "heap");
	else             std::snprintf(file_buf, sizeof(file_buf), "%d", file_id);

	std::fprintf(sTexLogFile,
		"[%-12s] file=%-6s off=0x%06X size=%-6u fmt=%s siz=%s bpp=%s first16=%s%s%s\n",
		path, file_buf, file_off, size_bytes,
		fmt_buf, siz_buf, bpp_buf, hex,
		note ? " note=" : "", note ? note : "");
	std::fflush(sTexLogFile);
}

// Public C-callable variadic logger declared in lbreloc_byteswap.h.
// Currently unused by external code but exposed so future hooks (e.g. an
// interpreter-side LoadBlock context line) can interleave entries into the
// same log without re-implementing the gating.
extern "C" void portRelocTexFixupLog(const char *fmt, ...)
{
	if (!tex_log_enabled()) return;
	std::va_list ap;
	va_start(ap, fmt);
	std::vfprintf(sTexLogFile, fmt, ap);
	va_end(ap);
	std::fflush(sTexLogFile);
}

// ============================================================
//  Texture PNG dump (SSB64_TEX_DUMP=1)
// ============================================================
//
// One-shot per texture, fired from chain_fixup_settimg and
// portFixupSpriteBitmapData immediately after the byteswap fix is applied.
// The data passed in is in N64 big-endian byte order — i.e. the same layout
// Fast3D's ImportTexture* readers consume — so what we decode here is what
// the renderer will eventually upload as RGBA8.
//
// The chain path doesn't know the texture's actual width/height (LOADBLOCK
// only encodes the total texel count), so for chain-path dumps we emit one
// TGA per plausible width in {8,16,32,64,128} that divides the pixel count
// evenly.  Sprite-path dumps have real (width_img × actualHeight) so they
// emit one image at the correct dimensions.
//
// Filename: tex_dump/f<file>_o<offset>_fmt<f>siz<s>_w<w>_h<h>.tga
//
// TGA format: uncompressed 32-bit BGRA, top-down.  Header is 18 bytes;
// supported by every image viewer.

static int sTexDumpState = 0;        // 0=uninit, 1=enabled, -1=disabled

static void tex_dump_init_once()
{
	if (sTexDumpState != 0) return;
	const char *env = std::getenv("SSB64_TEX_DUMP");
	if (env == nullptr || env[0] == '0' || env[0] == '\0') {
		sTexDumpState = -1;
		return;
	}
	// Best-effort directory create.  On Windows the binary's CWD is
	// build/Debug, so this lands at build/Debug/tex_dump/.
	std::error_code ec;
	std::filesystem::create_directories("tex_dump", ec);
	sTexDumpState = 1;
}

bool tex_dump_enabled()
{
	if (sTexDumpState == 0) tex_dump_init_once();
	return sTexDumpState == 1;
}

// Write one 32-bit RGBA texel buffer (already in r,g,b,a byte order) as a
// TGA image.  TGA uses BGRA byte order so we swizzle on the way out.
static void dump_tga_rgba(const char *path,
                          const uint8_t *rgba, uint32_t w, uint32_t h)
{
	FILE *f = std::fopen(path, "wb");
	if (f == nullptr) return;
	uint8_t hdr[18] = {0};
	hdr[2]  = 2;                  // uncompressed truecolor
	hdr[12] = (uint8_t)(w & 0xFF);
	hdr[13] = (uint8_t)((w >> 8) & 0xFF);
	hdr[14] = (uint8_t)(h & 0xFF);
	hdr[15] = (uint8_t)((h >> 8) & 0xFF);
	hdr[16] = 32;                 // 32 bpp
	hdr[17] = 0x28;               // top-down, 8 alpha bits
	std::fwrite(hdr, 1, 18, f);
	std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
	for (size_t i = 0; i < (size_t)w * h; i++) {
		bgra[i * 4 + 0] = rgba[i * 4 + 2]; // B
		bgra[i * 4 + 1] = rgba[i * 4 + 1]; // G
		bgra[i * 4 + 2] = rgba[i * 4 + 0]; // R
		bgra[i * 4 + 3] = rgba[i * 4 + 3]; // A
	}
	std::fwrite(bgra.data(), 1, bgra.size(), f);
	std::fclose(f);
}

// Decode N64 texel data (in BE byte order, post fixup) into a flat RGBA8
// buffer.  Returns true if (fmt,siz) is supported and the byte count makes
// sense for the requested dimensions.  CI formats are decoded with a
// fabricated greyscale palette since we have no TLUT context here.
static bool decode_to_rgba8(const uint8_t *src, size_t src_bytes,
                            int fmt, int siz,
                            uint32_t width, uint32_t height,
                            std::vector<uint8_t> &out_rgba)
{
	size_t need_pixels = (size_t)width * height;
	out_rgba.assign(need_pixels * 4, 0);

	auto scale_5_8 = [](uint8_t v) -> uint8_t {
		return (uint8_t)((v << 3) | (v >> 2));
	};
	auto scale_4_8 = [](uint8_t v) -> uint8_t {
		return (uint8_t)((v << 4) | v);
	};
	auto scale_3_8 = [](uint8_t v) -> uint8_t {
		return (uint8_t)((v << 5) | (v << 2) | (v >> 1));
	};

	// siz: 0=4b, 1=8b, 2=16b, 3=32b
	if (siz == 3) {
		// RGBA32: 4 bytes per pixel, byte order R G B A
		size_t need = need_pixels * 4;
		if (need > src_bytes) return false;
		for (size_t i = 0; i < need_pixels; i++) {
			out_rgba[i * 4 + 0] = src[i * 4 + 0];
			out_rgba[i * 4 + 1] = src[i * 4 + 1];
			out_rgba[i * 4 + 2] = src[i * 4 + 2];
			out_rgba[i * 4 + 3] = src[i * 4 + 3];
		}
		return true;
	}
	if (siz == 2) {
		// 16-bit per pixel, BE.  fmt=0:RGBA5551, fmt=3:IA88, fmt=4:I16, others:RGBA5551
		size_t need = need_pixels * 2;
		if (need > src_bytes) return false;
		for (size_t i = 0; i < need_pixels; i++) {
			uint16_t c = ((uint16_t)src[i * 2] << 8) | src[i * 2 + 1];
			if (fmt == 3) {
				// IA88 (16b): hi=intensity, lo=alpha
				uint8_t I = (uint8_t)(c >> 8);
				uint8_t A = (uint8_t)(c & 0xFF);
				out_rgba[i * 4 + 0] = I;
				out_rgba[i * 4 + 1] = I;
				out_rgba[i * 4 + 2] = I;
				out_rgba[i * 4 + 3] = A;
			} else if (fmt == 4) {
				// I16 isn't a real RDP format — treat as IA88 fallback
				uint8_t I = (uint8_t)(c >> 8);
				out_rgba[i * 4 + 0] = I;
				out_rgba[i * 4 + 1] = I;
				out_rgba[i * 4 + 2] = I;
				out_rgba[i * 4 + 3] = 255;
			} else {
				// RGBA5551 (and CI16 falls back here)
				uint8_t r = (c >> 11) & 0x1F;
				uint8_t g = (c >>  6) & 0x1F;
				uint8_t b = (c >>  1) & 0x1F;
				uint8_t a = c & 1;
				out_rgba[i * 4 + 0] = scale_5_8(r);
				out_rgba[i * 4 + 1] = scale_5_8(g);
				out_rgba[i * 4 + 2] = scale_5_8(b);
				out_rgba[i * 4 + 3] = a ? 255 : 0;
			}
		}
		return true;
	}
	if (siz == 1) {
		// 8b per pixel.  fmt=2:CI8 (no palette here → grayscale),
		// fmt=3:IA44, fmt=4:I8
		if (need_pixels > src_bytes) return false;
		for (size_t i = 0; i < need_pixels; i++) {
			uint8_t v = src[i];
			if (fmt == 3) {
				uint8_t I = (v >> 4) & 0xF;
				uint8_t A = v & 0xF;
				out_rgba[i * 4 + 0] = scale_4_8(I);
				out_rgba[i * 4 + 1] = scale_4_8(I);
				out_rgba[i * 4 + 2] = scale_4_8(I);
				out_rgba[i * 4 + 3] = scale_4_8(A);
			} else {
				out_rgba[i * 4 + 0] = v;
				out_rgba[i * 4 + 1] = v;
				out_rgba[i * 4 + 2] = v;
				out_rgba[i * 4 + 3] = 255;
			}
		}
		return true;
	}
	if (siz == 0) {
		// 4b per pixel.  fmt=2:CI4 (no palette → grayscale),
		// fmt=3:IA31, fmt=4:I4
		if ((need_pixels + 1) / 2 > src_bytes) return false;
		for (size_t i = 0; i < need_pixels; i++) {
			uint8_t byte = src[i / 2];
			uint8_t nib = (i & 1) ? (byte & 0x0F) : (byte >> 4);
			if (fmt == 3) {
				uint8_t I = (nib >> 1) & 0x7;
				uint8_t A = nib & 1;
				out_rgba[i * 4 + 0] = scale_3_8(I);
				out_rgba[i * 4 + 1] = scale_3_8(I);
				out_rgba[i * 4 + 2] = scale_3_8(I);
				out_rgba[i * 4 + 3] = A ? 255 : 0;
			} else {
				uint8_t v = scale_4_8(nib);
				out_rgba[i * 4 + 0] = v;
				out_rgba[i * 4 + 1] = v;
				out_rgba[i * 4 + 2] = v;
				out_rgba[i * 4 + 3] = 255;
			}
		}
		return true;
	}
	return false;
}

// Dump a texture for which we know the width and height (sprite path).
void tex_dump_known_dims(int file_id, uint32_t file_off,
                                int fmt, int siz, int bpp,
                                const uint8_t *src, size_t src_bytes,
                                uint32_t w, uint32_t h)
{
	if (!tex_dump_enabled()) return;
	std::vector<uint8_t> rgba;
	if (!decode_to_rgba8(src, src_bytes, fmt, siz, w, h, rgba)) return;
	char path[256];
	std::snprintf(path, sizeof(path),
	              "tex_dump/sprite_f%d_o0x%06X_fmt%dsiz%dbpp%d_w%u_h%u.tga",
	              file_id, file_off, fmt, siz, bpp, w, h);
	dump_tga_rgba(path, rgba.data(), w, h);
}

// Dump a chain-path texture across plausible widths.
void tex_dump_chain(int file_id, uint32_t file_off,
                           int fmt, int siz, uint32_t bpp,
                           const uint8_t *src, size_t src_bytes)
{
	if (!tex_dump_enabled()) return;
	if (bpp == 0 || src_bytes == 0) return;

	size_t total_bits = src_bytes * 8;
	if (total_bits % bpp != 0) return;
	size_t pixels = total_bits / bpp;
	if (pixels == 0 || pixels > 256 * 256) return;

	static const uint32_t kCandidateWidths[] = {8, 16, 32, 64, 128, 256};
	int written = 0;
	for (uint32_t w : kCandidateWidths) {
		if ((size_t)w > pixels) break;
		if (pixels % w != 0) continue;
		uint32_t h = (uint32_t)(pixels / w);
		if (h > 256) continue;

		std::vector<uint8_t> rgba;
		if (!decode_to_rgba8(src, src_bytes, fmt, siz, w, h, rgba)) continue;
		char path[256];
		std::snprintf(path, sizeof(path),
		              "tex_dump/chain_f%d_o0x%06X_fmt%dsiz%dbpp%u_w%u_h%u.tga",
		              file_id, file_off, fmt, siz, bpp, w, h);
		dump_tga_rgba(path, rgba.data(), w, h);
		if (++written >= 4) break; // cap to keep file count manageable
	}
}
