/**
 * gbi_decoder.c — Shared F3DEX2 / RDP display list decoder
 *
 * Pure C, no dependencies beyond libc. Used by both the port-side
 * trace tool and the Mupen64Plus trace plugin.
 */
#include "gbi_decoder.h"
#include <string.h>

/* ========================================================================= */
/*  Opcode name table                                                        */
/* ========================================================================= */

typedef struct {
	uint8_t     opcode;
	const char *name;
} GbiOpEntry;

static const GbiOpEntry sOpcodeTable[] = {
	/* F3DEX2 geometry */
	{ GBI_G_NOOP,            "G_NOOP" },
	{ GBI_G_VTX,             "G_VTX" },
	{ GBI_G_MODIFYVTX,       "G_MODIFYVTX" },
	{ GBI_G_CULLDL,          "G_CULLDL" },
	{ GBI_G_BRANCH_Z,        "G_BRANCH_Z" },
	{ GBI_G_TRI1,            "G_TRI1" },
	{ GBI_G_TRI2,            "G_TRI2" },
	{ GBI_G_QUAD,            "G_QUAD" },
	{ GBI_G_LINE3D,          "G_LINE3D" },

	/* F3DEX2 immediate */
	{ GBI_G_SPNOOP,          "G_SPNOOP" },
	{ GBI_G_RDPHALF_1,       "G_RDPHALF_1" },
	{ GBI_G_SETOTHERMODE_L,  "G_SETOTHERMODE_L" },
	{ GBI_G_SETOTHERMODE_H,  "G_SETOTHERMODE_H" },
	{ GBI_G_DL,              "G_DL" },
	{ GBI_G_ENDDL,           "G_ENDDL" },
	{ GBI_G_LOAD_UCODE,      "G_LOAD_UCODE" },
	{ GBI_G_MOVEMEM,         "G_MOVEMEM" },
	{ GBI_G_MOVEWORD,        "G_MOVEWORD" },
	{ GBI_G_MTX,             "G_MTX" },
	{ GBI_G_GEOMETRYMODE,    "G_GEOMETRYMODE" },
	{ GBI_G_POPMTX,          "G_POPMTX" },
	{ GBI_G_TEXTURE,         "G_TEXTURE" },
	{ GBI_G_DMA_IO,          "G_DMA_IO" },
	{ GBI_G_SPECIAL_1,       "G_SPECIAL_1" },
	{ GBI_G_SPECIAL_2,       "G_SPECIAL_2" },
	{ GBI_G_SPECIAL_3,       "G_SPECIAL_3" },

	/* RDP commands */
	{ GBI_G_SETCIMG,         "G_SETCIMG" },
	{ GBI_G_SETZIMG,         "G_SETZIMG" },
	{ GBI_G_SETTIMG,         "G_SETTIMG" },
	{ GBI_G_SETCOMBINE,      "G_SETCOMBINE" },
	{ GBI_G_SETENVCOLOR,     "G_SETENVCOLOR" },
	{ GBI_G_SETPRIMCOLOR,    "G_SETPRIMCOLOR" },
	{ GBI_G_SETBLENDCOLOR,   "G_SETBLENDCOLOR" },
	{ GBI_G_SETFOGCOLOR,     "G_SETFOGCOLOR" },
	{ GBI_G_SETFILLCOLOR,    "G_SETFILLCOLOR" },
	{ GBI_G_FILLRECT,        "G_FILLRECT" },
	{ GBI_G_SETTILE,         "G_SETTILE" },
	{ GBI_G_LOADTILE,        "G_LOADTILE" },
	{ GBI_G_LOADBLOCK,       "G_LOADBLOCK" },
	{ GBI_G_SETTILESIZE,     "G_SETTILESIZE" },
	{ GBI_G_RDPHALF_2,       "G_RDPHALF_2" },
	{ GBI_G_LOADTLUT,        "G_LOADTLUT" },
	{ GBI_G_RDPSETOTHERMODE,  "G_RDPSETOTHERMODE" },
	{ GBI_G_SETPRIMDEPTH,    "G_SETPRIMDEPTH" },
	{ GBI_G_SETSCISSOR,      "G_SETSCISSOR" },
	{ GBI_G_SETCONVERT,      "G_SETCONVERT" },
	{ GBI_G_SETKEYR,         "G_SETKEYR" },
	{ GBI_G_SETKEYGB,        "G_SETKEYGB" },
	{ GBI_G_RDPFULLSYNC,     "G_RDPFULLSYNC" },
	{ GBI_G_RDPTILESYNC,     "G_RDPTILESYNC" },
	{ GBI_G_RDPPIPESYNC,     "G_RDPPIPESYNC" },
	{ GBI_G_RDPLOADSYNC,     "G_RDPLOADSYNC" },
	{ GBI_G_TEXRECTFLIP,     "G_TEXRECTFLIP" },
	{ GBI_G_TEXRECT,         "G_TEXRECT" },
};

static const int sOpcodeTableSize = sizeof(sOpcodeTable) / sizeof(sOpcodeTable[0]);

/* Fast lookup: indexed by opcode byte (0x00..0xFF) */
static const char *sOpcodeLut[256];
static int sLutInit = 0;

static void init_lut(void)
{
	int i;
	if (sLutInit) return;
	memset((void *)sOpcodeLut, 0, sizeof(sOpcodeLut));
	for (i = 0; i < sOpcodeTableSize; i++) {
		sOpcodeLut[sOpcodeTable[i].opcode] = sOpcodeTable[i].name;
	}
	sLutInit = 1;
}

/* ========================================================================= */
/*  Public helpers                                                           */
/* ========================================================================= */

const char *gbi_opcode_name(uint8_t opcode)
{
	static char unknown[16];
	init_lut();
	if (sOpcodeLut[opcode]) return sOpcodeLut[opcode];
	snprintf(unknown, sizeof(unknown), "UNKNOWN_%02X", opcode);
	return unknown;
}

static const char *sFmtNames[] = { "RGBA", "YUV", "CI", "IA", "I" };

const char *gbi_img_fmt_name(uint32_t fmt)
{
	if (fmt <= 4) return sFmtNames[fmt];
	return "?FMT";
}

static const char *sSizNames[] = { "4b", "8b", "16b", "32b" };

const char *gbi_img_siz_name(uint32_t siz)
{
	if (siz <= 3) return sSizNames[siz];
	return "?SIZ";
}

char *gbi_decode_geom_mode(uint32_t mode, char *buf, int buf_size)
{
	buf[0] = '\0';
	int pos = 0;

#define APPEND_FLAG(flag, name) do { \
	if (mode & (flag)) { \
		pos += snprintf(buf + pos, buf_size - pos, "%s%s", pos ? "|" : "", name); \
	} \
} while(0)

	APPEND_FLAG(GBI_G_ZBUFFER,             "ZBUF");
	APPEND_FLAG(GBI_G_SHADE,               "SHADE");
	APPEND_FLAG(GBI_G_CULL_FRONT,          "CULL_F");
	APPEND_FLAG(GBI_G_CULL_BACK,           "CULL_B");
	APPEND_FLAG(GBI_G_FOG,                 "FOG");
	APPEND_FLAG(GBI_G_LIGHTING,            "LIGHT");
	APPEND_FLAG(GBI_G_TEXTURE_GEN,         "TEXGEN");
	APPEND_FLAG(GBI_G_TEXTURE_GEN_LINEAR,  "TEXGEN_LIN");
	APPEND_FLAG(GBI_G_SHADING_SMOOTH,      "SMOOTH");
	APPEND_FLAG(GBI_G_CLIPPING,            "CLIP");

#undef APPEND_FLAG

	if (pos == 0) snprintf(buf, buf_size, "NONE");
	return buf;
}

int gbi_is_dl_call(uint8_t opcode) { return opcode == GBI_G_DL; }
int gbi_is_dl_end(uint8_t opcode)  { return opcode == GBI_G_ENDDL; }

int gbi_dl_is_branch(uint32_t w0)
{
	/* F3DEX2: G_DL w0 lower byte bit 0 = 1 means branch (no push) */
	return (w0 & 0x01) != 0;
}

/* ========================================================================= */
/*  Command decoder                                                          */
/* ========================================================================= */

typedef int (*GbiDecFn)(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size);

static int gbi_dec_vtx(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	/* F3DEX2: w0 = 01[nn][vv]00  nn=count<<4, vv=vbidx+count */
	uint32_t numv = ((w0 >> 12) & 0xFF) >> 4;
	uint32_t vbidx = ((w0 >> 1) & 0x7F) - numv;
	(void)opcode;
	return snprintf(buf, buf_size, "  n=%u v0=%u addr=%08X", numv, vbidx, w1);
}

static int gbi_dec_tri1(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	uint32_t v0 = ((w0 >> 16) & 0xFF) / 2;
	uint32_t v1 = ((w0 >> 8) & 0xFF) / 2;
	uint32_t v2 = (w0 & 0xFF) / 2;
	(void)opcode; (void)w1;
	return snprintf(buf, buf_size, "  v=(%u,%u,%u)", v0, v1, v2);
}

static int gbi_dec_tri2(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	uint32_t v0 = ((w0 >> 16) & 0xFF) / 2;
	uint32_t v1 = ((w0 >> 8) & 0xFF) / 2;
	uint32_t v2 = (w0 & 0xFF) / 2;
	uint32_t v3 = ((w1 >> 16) & 0xFF) / 2;
	uint32_t v4 = ((w1 >> 8) & 0xFF) / 2;
	uint32_t v5 = (w1 & 0xFF) / 2;
	(void)opcode;
	return snprintf(buf, buf_size, "  v=(%u,%u,%u)(%u,%u,%u)", v0, v1, v2, v3, v4, v5);
}

static int gbi_dec_dl(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	const char *kind = gbi_dl_is_branch(w0) ? "BRANCH" : "CALL";
	uint32_t seg = (w1 >> 24) & 0x0F;
	uint32_t off = w1 & 0x00FFFFFF;
	(void)opcode;
	return snprintf(buf, buf_size, "  %s seg=%u off=0x%06X", kind, seg, off);
}

static int gbi_dec_mtx(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	uint32_t params = w0 & 0xFF;
	const char *proj = (params & 0x04) ? "PROJ" : "MV";
	const char *load = (params & 0x02) ? "LOAD" : "MUL";
	const char *push = (params & 0x01) ? "PUSH" : "NOPUSH";
	uint32_t seg = (w1 >> 24) & 0x0F;
	uint32_t off = w1 & 0x00FFFFFF;
	(void)opcode;
	return snprintf(buf, buf_size, "  %s %s %s seg=%u off=0x%06X",
	                proj, load, push, seg, off);
}

static int gbi_dec_geometrymode(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	uint32_t clearbits = ~(w0 & 0x00FFFFFF) & 0x00FFFFFF;
	uint32_t setbits = w1;
	char clr[256], set[256];
	(void)opcode;
	gbi_decode_geom_mode(clearbits, clr, sizeof(clr));
	gbi_decode_geom_mode(setbits, set, sizeof(set));
	return snprintf(buf, buf_size, "  clr=%s set=%s", clr, set);
}

static int gbi_dec_texture(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	uint32_t s = (w1 >> 16) & 0xFFFF;
	uint32_t t = w1 & 0xFFFF;
	uint32_t level = (w0 >> 11) & 0x07;
	uint32_t tile = (w0 >> 8) & 0x07;
	uint32_t on = w0 & 0xFF;
	(void)opcode;
	return snprintf(buf, buf_size, "  s=%04X t=%04X lv=%u tile=%u on=%u",
	                s, t, level, tile, on);
}

static int gbi_dec_setimg(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	uint32_t fmt = (w0 >> 21) & 0x07;
	uint32_t siz = (w0 >> 19) & 0x03;
	uint32_t width = (w0 & 0x0FFF) + 1;
	uint32_t seg = (w1 >> 24) & 0x0F;
	uint32_t off = w1 & 0x00FFFFFF;
	if (opcode == GBI_G_SETZIMG) {
		return snprintf(buf, buf_size, "  seg=%u off=0x%06X", seg, off);
	}
	return snprintf(buf, buf_size, "  fmt=%s siz=%s w=%u seg=%u off=0x%06X",
	                gbi_img_fmt_name(fmt), gbi_img_siz_name(siz), width, seg, off);
}

static int gbi_dec_setcombine(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	/* Decode color combiner modes (packed into w0/w1) */
	uint32_t a0 = (w0 >> 20) & 0x0F;
	uint32_t b0 = (w1 >> 28) & 0x0F;
	uint32_t c0 = (w0 >> 15) & 0x1F;
	uint32_t d0 = (w1 >> 15) & 0x07;
	uint32_t a1 = (w0 >> 12) & 0x07;
	uint32_t b1 = (w1 >> 12) & 0x07;
	uint32_t c1 = (w0 >> 9) & 0x1F;
	uint32_t d1 = (w1 >> 9) & 0x07;
	(void)opcode;
	return snprintf(buf, buf_size,
	                "  cyc0=(%u-%u)*%u+%u cyc1=(%u-%u)*%u+%u",
	                a0, b0, c0, d0, a1, b1, c1, d1);
}

static int gbi_dec_rgba(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	uint32_t r = (w1 >> 24) & 0xFF;
	uint32_t g = (w1 >> 16) & 0xFF;
	uint32_t b = (w1 >> 8) & 0xFF;
	uint32_t a = w1 & 0xFF;
	(void)opcode; (void)w0;
	return snprintf(buf, buf_size, "  rgba=(%u,%u,%u,%u)", r, g, b, a);
}

static int gbi_dec_setprimcolor(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	uint32_t minlevel = (w0 >> 8) & 0xFF;
	uint32_t lodfrac = w0 & 0xFF;
	uint32_t r = (w1 >> 24) & 0xFF;
	uint32_t g = (w1 >> 16) & 0xFF;
	uint32_t b = (w1 >> 8) & 0xFF;
	uint32_t a = w1 & 0xFF;
	(void)opcode;
	return snprintf(buf, buf_size, "  rgba=(%u,%u,%u,%u) minlev=%u lodfrac=%u",
	                r, g, b, a, minlevel, lodfrac);
}

static int gbi_dec_settile(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	uint32_t fmt = (w0 >> 21) & 0x07;
	uint32_t siz = (w0 >> 19) & 0x03;
	uint32_t line = (w0 >> 9) & 0x1FF;
	uint32_t tmem = w0 & 0x1FF;
	uint32_t tile = (w1 >> 24) & 0x07;
	uint32_t cms = (w1 >> 8) & 0x03;
	uint32_t cmt = (w1 >> 18) & 0x03;
	(void)opcode;
	return snprintf(buf, buf_size, "  tile=%u fmt=%s siz=%s line=%u tmem=%u cms=%u cmt=%u",
	                tile, gbi_img_fmt_name(fmt), gbi_img_siz_name(siz), line, tmem, cms, cmt);
}

static int gbi_dec_tile_rect(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	uint32_t uls = (w0 >> 12) & 0xFFF;
	uint32_t ult = w0 & 0xFFF;
	uint32_t tile = (w1 >> 24) & 0x07;
	uint32_t lrs = (w1 >> 12) & 0xFFF;
	uint32_t lrt = w1 & 0xFFF;
	(void)opcode;
	return snprintf(buf, buf_size, "  tile=%u ul=(%u.%u,%u.%u) lr=(%u.%u,%u.%u)",
	                tile, uls >> 2, uls & 3, ult >> 2, ult & 3,
	                lrs >> 2, lrs & 3, lrt >> 2, lrt & 3);
}

static int gbi_dec_loadblock(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	uint32_t uls = (w0 >> 12) & 0xFFF;
	uint32_t ult = w0 & 0xFFF;
	uint32_t tile = (w1 >> 24) & 0x07;
	uint32_t texels = ((w1 >> 12) & 0xFFF) + 1;
	uint32_t dxt = w1 & 0xFFF;
	(void)opcode;
	return snprintf(buf, buf_size, "  tile=%u ul=(%u,%u) texels=%u dxt=%u",
	                tile, uls, ult, texels, dxt);
}

static int gbi_dec_loadtlut(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	uint32_t tile = (w1 >> 24) & 0x07;
	uint32_t count = ((w1 >> 14) & 0x3FF) + 1;
	(void)opcode; (void)w0;
	return snprintf(buf, buf_size, "  tile=%u count=%u", tile, count);
}

static int gbi_dec_setscissor(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	uint32_t ulx = (w0 >> 12) & 0xFFF;
	uint32_t uly = w0 & 0xFFF;
	uint32_t mode = (w1 >> 24) & 0x03;
	uint32_t lrx = (w1 >> 12) & 0xFFF;
	uint32_t lry = w1 & 0xFFF;
	const char *mstr = mode == 0 ? "NOINT" : mode == 2 ? "EVEN" : mode == 3 ? "ODD" : "?";
	(void)opcode;
	return snprintf(buf, buf_size, "  (%u.%u,%u.%u)-(%u.%u,%u.%u) mode=%s",
	                ulx >> 2, ulx & 3, uly >> 2, uly & 3,
	                lrx >> 2, lrx & 3, lry >> 2, lry & 3, mstr);
}

static int gbi_dec_fillrect(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	uint32_t lrx = (w0 >> 12) & 0xFFF;
	uint32_t lry = w0 & 0xFFF;
	uint32_t ulx = (w1 >> 12) & 0xFFF;
	uint32_t uly = w1 & 0xFFF;
	(void)opcode;
	return snprintf(buf, buf_size, "  (%u.%u,%u.%u)-(%u.%u,%u.%u)",
	                ulx >> 2, ulx & 3, uly >> 2, uly & 3,
	                lrx >> 2, lrx & 3, lry >> 2, lry & 3);
}

static int gbi_dec_setfillcolor(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	(void)opcode; (void)w0;
	return snprintf(buf, buf_size, "  color=%08X", w1);
}

static int gbi_dec_setprimdepth(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	uint32_t z = (w1 >> 16) & 0xFFFF;
	uint32_t dz = w1 & 0xFFFF;
	(void)opcode; (void)w0;
	return snprintf(buf, buf_size, "  z=%u dz=%u", z, dz);
}

static const char *gbi_moveword_index_name(uint32_t index)
{
	switch (index) {
	case 0x00: return "MATRIX";
	case 0x02: return "NUMLIGHT";
	case 0x04: return "CLIP";
	case 0x06: return "SEGMENT";
	case 0x08: return "FOG";
	case 0x0A: return "LIGHTCOL";
	case 0x0C: return "FORCEMTX";
	case 0x0E: return "PERSPNORM";
	default:   return "?";
	}
}

static int gbi_dec_moveword(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	uint32_t index = (w0 >> 16) & 0xFF;
	uint32_t offset = w0 & 0xFFFF;
	const char *idx_name = gbi_moveword_index_name(index);
	(void)opcode;
	return snprintf(buf, buf_size, "  idx=%s(0x%02X) off=0x%04X val=%08X",
	                idx_name, index, offset, w1);
}

static int gbi_dec_popmtx(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	(void)opcode; (void)w0;
	return snprintf(buf, buf_size, "  count=%u", w1 / 64);
}

static int gbi_dec_setothermode(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	uint32_t shift = (w0 >> 8) & 0xFF;
	uint32_t len = (w0 & 0xFF) + 1;
	(void)opcode;
	return snprintf(buf, buf_size, "  shift=%u len=%u data=%08X",
	                shift, len, w1);
}

static int gbi_dec_texrect(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	uint32_t lrx = (w0 >> 12) & 0xFFF;
	uint32_t lry = w0 & 0xFFF;
	uint32_t tile = (w1 >> 24) & 0x07;
	uint32_t ulx = (w1 >> 12) & 0xFFF;
	uint32_t uly = w1 & 0xFFF;
	(void)opcode;
	return snprintf(buf, buf_size, "  tile=%u (%u.%u,%u.%u)-(%u.%u,%u.%u)",
	                tile, ulx >> 2, ulx & 3, uly >> 2, uly & 3,
	                lrx >> 2, lrx & 3, lry >> 2, lry & 3);
}

static int gbi_dec_modifyvtx(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	uint32_t where = (w0 >> 16) & 0xFF;
	uint32_t vtx = ((w0 >> 1) & 0x7FFF);
	(void)opcode;
	return snprintf(buf, buf_size, "  where=0x%02X vtx=%u val=%08X",
	                where, vtx, w1);
}

static int gbi_dec_culldl(uint8_t opcode, uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	uint32_t vfirst = ((w0 >> 1) & 0x7FFF);
	uint32_t vlast = ((w1 >> 1) & 0x7FFF);
	(void)opcode;
	return snprintf(buf, buf_size, "  v=%u..%u", vfirst, vlast);
}

typedef struct {
	uint8_t  opcode;
	GbiDecFn fn;
} GbiDecEntry;

static const GbiDecEntry sDecTable[] = {
	{ GBI_G_VTX,            gbi_dec_vtx },
	{ GBI_G_TRI1,           gbi_dec_tri1 },
	{ GBI_G_TRI2,           gbi_dec_tri2 },
	{ GBI_G_DL,             gbi_dec_dl },
	{ GBI_G_MTX,            gbi_dec_mtx },
	{ GBI_G_GEOMETRYMODE,   gbi_dec_geometrymode },
	{ GBI_G_TEXTURE,        gbi_dec_texture },
	{ GBI_G_SETTIMG,        gbi_dec_setimg },
	{ GBI_G_SETCIMG,        gbi_dec_setimg },
	{ GBI_G_SETZIMG,        gbi_dec_setimg },
	{ GBI_G_SETCOMBINE,     gbi_dec_setcombine },
	{ GBI_G_SETENVCOLOR,    gbi_dec_rgba },
	{ GBI_G_SETBLENDCOLOR,  gbi_dec_rgba },
	{ GBI_G_SETFOGCOLOR,    gbi_dec_rgba },
	{ GBI_G_SETPRIMCOLOR,   gbi_dec_setprimcolor },
	{ GBI_G_SETTILE,        gbi_dec_settile },
	{ GBI_G_SETTILESIZE,    gbi_dec_tile_rect },
	{ GBI_G_LOADBLOCK,      gbi_dec_loadblock },
	{ GBI_G_LOADTILE,       gbi_dec_tile_rect },
	{ GBI_G_LOADTLUT,       gbi_dec_loadtlut },
	{ GBI_G_SETSCISSOR,     gbi_dec_setscissor },
	{ GBI_G_FILLRECT,       gbi_dec_fillrect },
	{ GBI_G_SETFILLCOLOR,   gbi_dec_setfillcolor },
	{ GBI_G_SETPRIMDEPTH,   gbi_dec_setprimdepth },
	{ GBI_G_MOVEWORD,       gbi_dec_moveword },
	{ GBI_G_POPMTX,         gbi_dec_popmtx },
	{ GBI_G_SETOTHERMODE_H, gbi_dec_setothermode },
	{ GBI_G_SETOTHERMODE_L, gbi_dec_setothermode },
	{ GBI_G_TEXRECT,        gbi_dec_texrect },
	{ GBI_G_TEXRECTFLIP,    gbi_dec_texrect },
	{ GBI_G_MODIFYVTX,      gbi_dec_modifyvtx },
	{ GBI_G_CULLDL,         gbi_dec_culldl },
};

int gbi_decode_cmd(uint32_t w0, uint32_t w1, char *buf, int buf_size)
{
	uint8_t opcode = (uint8_t)(w0 >> 24);
	const char *name = gbi_opcode_name(opcode);
	int n = 0;
	size_t i;

	/* Common prefix: opcode name + raw words */
	n = snprintf(buf, buf_size, "%-18s w0=%08X w1=%08X", name, w0, w1);
	if (n >= buf_size) return n;

	/* Per-opcode parameter decoding */
	for (i = 0; i < sizeof(sDecTable) / sizeof(sDecTable[0]); i++) {
		if (sDecTable[i].opcode == opcode) {
			n += sDecTable[i].fn(opcode, w0, w1, buf + n, buf_size - n);
			break;
		}
	}
	return n;
}
