// android_torch_bridge.cpp — non-CLI entry points for Torch on Android.
//
// Torch's normal main() lives behind STANDALONE && !__EMSCRIPTEN__ in
// components/game-runtime/src/main.cpp and uses CLI11 to drive the Companion class. On
// Android we ship Torch as a separate shared library (libtorch_runner.so)
// that the first-run Activity loads to extract a ROM the user picked
// via the Storage Access Framework.
//
// This file is only built on Android (gated in the root CMakeLists.txt).
// It deliberately avoids any SDL2 / libultraship deps — libtorch_runner.so
// must be loadable BEFORE SDLActivity is set up, otherwise SDL2's
// JNI_OnLoad would trip over the missing SDLActivity Java class.

#if defined(__ANDROID__)

#include "Companion.h"
#include "icon_scale.h"
#include "rgba16_swizzle.h"

#include "libvpk0/vpk0.h"
#include "n64graphics/n64graphics.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/android_sink.h"

#include <android/log.h>
#include <jni.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>

#define LOG_TAG "ssb64.torch"
#include "android_log.h"

namespace {

constexpr uint32_t kRelocTableRomAddr = 0x1AC870;   // US v1.0 (NALE) reloc table base
constexpr uint32_t kRelocTableEntrySize = 12;       // RELOC_TABLE_ENTRY_SIZE
constexpr uint32_t kRelocFileCount = 2132;          // US v1.0 reloc file count
// The data region begins right after the table, which includes a trailing +1
// sentinel entry. This mirrors torch SSB64::GetRelocLayout:
//   dataStart = tableRomAddr + (fileCount + 1) * entrySize
//             = 0x1AC870 + 2133 * 12 = 0x1B2C6C
// (The previous hardcoded 0x1AEAA0 was wrong, so every VPK0-compressed stage
//  file was read from the wrong ROM offset and failed to decode.)
constexpr uint32_t kRelocDataStart =
    kRelocTableRomAddr + (kRelocFileCount + 1) * kRelocTableEntrySize;
constexpr int kIconW = ssb64::kCssStageIconW;
constexpr int kIconH = ssb64::kCssStageIconH;
constexpr int kSpriteSize = 68;
constexpr int kBitmapSize = 16;

struct AndroidCssStageDef {
    const char *name;
    uint32_t file_id;
    uint32_t sprite_off;
};

constexpr AndroidCssStageDef kAndroidCssStages[] = {
    { "final_destination", 96,   0x26C88 },
    { "metal_cavern",     0x62, 0x26C88 },
    { "battlefield",      0x61, 0x26C88 },
};

struct ParsedSprite {
    int16_t width;
    int16_t height;
    int16_t nbitmaps;
    int16_t bmheight;
    uint8_t bmfmt;
    uint8_t bmsiz;
};

struct ParsedBitmap {
    int16_t width;
    int16_t actual_height;
    uint32_t buf_off;
};

static uint16_t read_be_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);
}

static int16_t read_be_s16(const uint8_t *p) {
    return (int16_t)read_be_u16(p);
}

static uint32_t read_be_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static bool read_file_bytes(const std::filesystem::path &path, std::vector<uint8_t> &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size <= 0) {
        return false;
    }
    f.seekg(0, std::ios::beg);
    out.resize((size_t)size);
    return (bool)f.read(reinterpret_cast<char *>(out.data()), size);
}

static bool rom_has_magic(const std::vector<uint8_t> &rom,
                          uint8_t a, uint8_t b, uint8_t c, uint8_t d4) {
    return rom[0] == a && rom[1] == b && rom[2] == c && rom[3] == d4;
}

// Byte-swap every 16-bit word (v64 -> z64).
static void rom_bswap16(std::vector<uint8_t> &rom) {
    const size_t n = rom.size() & ~size_t{1};
    for (size_t i = 0; i < n; i += 2) {
        std::swap(rom[i], rom[i + 1]);
    }
}

// Reverse every 32-bit word (n64 -> z64).
static void rom_bswap32(std::vector<uint8_t> &rom) {
    const size_t n = rom.size() & ~size_t{3};
    for (size_t i = 0; i < n; i += 4) {
        std::swap(rom[i + 0], rom[i + 3]);
        std::swap(rom[i + 1], rom[i + 2]);
    }
}

static bool normalize_rom_byte_order(std::vector<uint8_t> &rom) {
    if (rom.size() < 4) {
        return false;
    }
    if (rom_has_magic(rom, 0x80, 0x37, 0x12, 0x40)) {
        return true;
    }
    if (rom_has_magic(rom, 0x37, 0x80, 0x40, 0x12)) {
        rom_bswap16(rom);
        return true;
    }
    if (rom_has_magic(rom, 0x40, 0x12, 0x37, 0x80)) {
        rom_bswap32(rom);
        return true;
    }
    return false;
}

static bool write_bytes(const std::filesystem::path &path, const unsigned char *data, int size) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        LOGE("CSS stage assets: mkdirs failed for %s: %s",
             path.parent_path().string().c_str(), ec.message().c_str());
        return false;
    }

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        return false;
    }
    f.write(reinterpret_cast<const char *>(data), size);
    return (bool)f;
}

static bool extract_reloc_file(const std::vector<uint8_t> &rom, uint32_t file_id,
                               std::vector<uint8_t> &out) {
    const size_t table_off = (size_t)kRelocTableRomAddr + (size_t)file_id * kRelocTableEntrySize;
    if (table_off + kRelocTableEntrySize > rom.size()) {
        LOGE("CSS stage assets: ROM too small for reloc table entry %u", file_id);
        return false;
    }

    const uint8_t *te = rom.data() + table_off;
    const uint32_t first_word = read_be_u32(te + 0);
    const bool is_compressed = (first_word >> 31) != 0;
    const uint32_t data_offset = first_word & 0x7FFFFFFFu;
    const uint32_t compressed_bytes = (uint32_t)read_be_u16(te + 6) * 4u;
    const uint32_t decompressed_bytes = (uint32_t)read_be_u16(te + 10) * 4u;
    const size_t data_rom_addr = (size_t)kRelocDataStart + data_offset;

    if (data_rom_addr + compressed_bytes > rom.size()) {
        LOGE("CSS stage assets: ROM too small for reloc file %u data", file_id);
        return false;
    }

    const uint8_t *file_data = rom.data() + data_rom_addr;
    out.resize(decompressed_bytes);
    if (is_compressed) {
        const uint32_t written = vpk0_decode(file_data, compressed_bytes,
                                             out.data(), decompressed_bytes);
        if (written == 0) {
            LOGE("CSS stage assets: VPK0 decode failed for reloc file %u", file_id);
            return false;
        }
        if (written < decompressed_bytes) {
            out.resize(written);
        }
    } else {
        std::memcpy(out.data(), file_data, decompressed_bytes);
    }
    return true;
}

static uint32_t resolve_reloc_ptr(const std::vector<uint8_t> &file_data, uint32_t field_off) {
    if (field_off + 4 > file_data.size()) {
        return UINT32_MAX;
    }
    return (read_be_u32(file_data.data() + field_off) & 0xFFFFu) * 4u;
}

static bool parse_sprite(const std::vector<uint8_t> &file_data, uint32_t sprite_off,
                         ParsedSprite &sp) {
    if (sprite_off + kSpriteSize > file_data.size()) {
        return false;
    }
    const uint8_t *d = file_data.data() + sprite_off;
    sp.width = read_be_s16(d + 0x04);
    sp.height = read_be_s16(d + 0x06);
    sp.nbitmaps = read_be_s16(d + 0x28);
    sp.bmheight = read_be_s16(d + 0x2C);
    sp.bmfmt = d[0x30];
    sp.bmsiz = d[0x31];
    return true;
}

static bool parse_bitmap(const std::vector<uint8_t> &file_data, uint32_t bm_off,
                         ParsedBitmap &bm) {
    if (bm_off + kBitmapSize > file_data.size()) {
        return false;
    }
    const uint8_t *d = file_data.data() + bm_off;
    bm.width = read_be_s16(d + 0x00);
    bm.actual_height = read_be_s16(d + 0x0C);
    bm.buf_off = resolve_reloc_ptr(file_data, bm_off + 0x08);
    return bm.buf_off != UINT32_MAX;
}


static rgba rgba16_pixel_to_rgba(uint8_t hi, uint8_t lo) {
    const uint16_t word = (uint16_t)((uint16_t)hi << 8 | (uint16_t)lo);
    const uint8_t r5 = (uint8_t)((word >> 11) & 0x1F);
    const uint8_t g5 = (uint8_t)((word >> 6) & 0x1F);
    const uint8_t b5 = (uint8_t)((word >> 1) & 0x1F);
    const uint8_t a1 = (uint8_t)(word & 0x01);
    return {
        (uint8_t)((r5 << 3) | (r5 >> 2)),
        (uint8_t)((g5 << 3) | (g5 >> 2)),
        (uint8_t)((b5 << 3) | (b5 >> 2)),
        (uint8_t)(a1 ? 255 : 0),
    };
}

static bool write_png_rgba(const std::filesystem::path &path,
                           const std::vector<rgba> &pixels,
                           int width, int height) {
    unsigned char *png = nullptr;
    int png_size = 0;
    if (rgba2png(&png, &png_size, pixels.data(), width, height) != 0 ||
        png == nullptr || png_size <= 0) {
        LOGE("CSS stage assets: PNG encode failed for %s", path.string().c_str());
        return false;
    }

    const bool ok = write_bytes(path, png, png_size);
    std::free(png);
    if (!ok) {
        LOGE("CSS stage assets: write failed for %s", path.string().c_str());
    }
    return ok;
}

/* Thin adapter over the shared filter (src/shared/icon_scale.h) — same
 * math the build-time icon bake uses, so derived icons match the baked
 * ones pixel-for-pixel. */
static std::vector<rgba> make_icon_bilinear(const std::vector<rgba> &src,
                                            int src_w, int src_h) {
    static_assert(sizeof(rgba) == 4, "rgba must be packed RGBA8");
    const std::vector<uint8_t> bytes = ssb64::IconDownscaleBilinear(
        reinterpret_cast<const uint8_t *>(src.data()), src_w, src_h,
        kIconW, kIconH);
    std::vector<rgba> dst((size_t)kIconW * kIconH);
    memcpy(dst.data(), bytes.data(), bytes.size());
    return dst;
}

/* Decode every bitmap strip of the sprite into a contiguous RGBA pixel
 * buffer (nbitmaps x bmwidth strips, rows capped at sp.bmheight). */
static bool decode_stage_pixels(const std::vector<uint8_t> &file_data,
                                const ParsedSprite &sp, uint32_t bm_array_start,
                                const char *name, std::vector<rgba> &pixels) {
    pixels.reserve((size_t)sp.width * sp.height);
    for (int i = 0; i < sp.nbitmaps; ++i) {
        ParsedBitmap bm{};
        const uint32_t bm_off = bm_array_start + (uint32_t)i * kBitmapSize;
        if (!parse_bitmap(file_data, bm_off, bm) || bm.width <= 0 || bm.actual_height <= 0) {
            return false;
        }
        const size_t strip_bytes = (size_t)bm.width * (size_t)bm.actual_height * 2u;
        if ((size_t)bm.buf_off + strip_bytes > file_data.size()) {
            LOGE("CSS stage assets: %s bitmap[%d] out of range", name, i);
            return false;
        }
        std::vector<uint8_t> strip(strip_bytes);
        std::memcpy(strip.data(), file_data.data() + bm.buf_off, strip_bytes);
        UnswizzleRgba16Strip(strip.data(), bm.width, bm.actual_height);
        const int rendered_rows = std::min<int>(sp.bmheight, bm.actual_height);
        for (int row = 0; row < rendered_rows; ++row) {
            for (int x = 0; x < bm.width; ++x) {
                const size_t off = ((size_t)row * bm.width + x) * 2u;
                pixels.push_back(rgba16_pixel_to_rgba(strip[off], strip[off + 1]));
            }
        }
    }
    const size_t expected_pixels = (size_t)sp.width * sp.height;
    if (pixels.size() < expected_pixels) {
        LOGE("CSS stage assets: %s decoded %zu pixels, expected %zu",
             name, pixels.size(), expected_pixels);
        return false;
    }
    if (pixels.size() > expected_pixels) {
        pixels.resize(expected_pixels);
    }
    return true;
}

/* Emit <name>_background.png (full sprite) + <name>_small.png (icon). */
static bool write_stage_pngs(const std::filesystem::path &out_dir,
                             const char *name,
                             const std::vector<rgba> &pixels,
                             int width, int height) {
    const std::filesystem::path bg_path = out_dir / (std::string(name) + "_background.png");
    const std::filesystem::path icon_path = out_dir / (std::string(name) + "_small.png");
    if (!write_png_rgba(bg_path, pixels, width, height)) {
        return false;
    }
    std::vector<rgba> icon = make_icon_bilinear(pixels, width, height);
    return write_png_rgba(icon_path, icon, kIconW, kIconH);
}

static bool derive_stage_asset(const std::vector<uint8_t> &rom,
                               const AndroidCssStageDef &stage,
                               const std::filesystem::path &out_dir) {
    std::vector<uint8_t> file_data;
    if (!extract_reloc_file(rom, stage.file_id, file_data)) {
        return false;
    }

    ParsedSprite sp{};
    if (!parse_sprite(file_data, stage.sprite_off, sp) ||
        sp.width <= 0 || sp.height <= 0 || sp.nbitmaps <= 0 ||
        sp.bmsiz != 2) {
        LOGE("CSS stage assets: unsupported sprite for %s", stage.name);
        return false;
    }

    const uint32_t bm_array_start = resolve_reloc_ptr(file_data, stage.sprite_off + 0x34);
    if (bm_array_start == UINT32_MAX) {
        return false;
    }

    std::vector<rgba> pixels;
    if (!decode_stage_pixels(file_data, sp, bm_array_start, stage.name, pixels)) {
        return false;
    }
    if (!write_stage_pngs(out_dir, stage.name, pixels, sp.width, sp.height)) {
        return false;
    }
    LOGI("CSS stage assets: derived %s background + icon", stage.name);
    return true;
}

static void derive_android_css_stage_assets(const char *rom_path, const char *dst_dir) {
    std::vector<uint8_t> rom;
    if (!read_file_bytes(rom_path, rom)) {
        LOGE("CSS stage assets: failed to read staged ROM %s", rom_path);
        return;
    }
    if (!normalize_rom_byte_order(rom)) {
        LOGE("CSS stage assets: staged ROM has unrecognized byte order; skipping");
        return;
    }

    const std::filesystem::path out_dir =
        std::filesystem::path(dst_dir) / "assets" / "css_icons";
    int ok_count = 0;
    for (const AndroidCssStageDef &stage : kAndroidCssStages) {
        if (derive_stage_asset(rom, stage, out_dir)) {
            ok_count++;
        }
    }
    LOGI("CSS stage assets: derived %d/%zu stage asset sets into %s",
         ok_count, sizeof(kAndroidCssStages) / sizeof(kAndroidCssStages[0]),
         out_dir.string().c_str());
}

} // namespace

extern "C" {

/**
 * torch_extract_o2r — produce BattleShip.o2r from a user-supplied ROM.
 *
 * @param rom_path    Absolute path to the user's baserom.us.{z64,n64,v64}.
 *                    Must be readable by the calling process. The first-run
 *                    Activity is expected to copy the SAF-picked content
 *                    into the app's filesDir before calling us, since
 *                    Torch's Cartridge::open() does plain fopen(2).
 * @param src_dir     Absolute path to the directory containing config.yml,
 *                    the US yaml files, and any other torch-config files.
 *                    BootActivity passes getExternalFilesDir(null), where
 *                    AssetExtractor stages them from APK assets on first run.
 * @param dst_dir     Absolute path where Torch will write BattleShip.o2r.
 *                    Typically the app's externalFilesDir (same dir as
 *                    src_dir in the current flow).
 *
 * @return  0 on success, non-zero on failure (see android logcat tag
 *          "ssb64.torch" for the failure reason):
 *          -1/-2 bad arguments / ROM file missing,
 *           1/2  exception during extraction,
 *           3    Torch bailed before configuring output — the ROM is not a
 *                supported dump (SHA-1 not in config.yml) or the staged
 *                config/yamls are missing,
 *           4    Torch finished but the archive was never written (disk
 *                full / write failure).
 *
 * Thread-safety: Companion uses a singleton (Companion::Instance), so
 * concurrent calls are NOT supported. The first-run flow is single-shot
 * anyway; the ROM picker doesn't allow re-entry while extraction is
 * running.
 */
int torch_extract_o2r(const char *rom_path, const char *src_dir, const char *dst_dir) {
    if (rom_path == nullptr || src_dir == nullptr || dst_dir == nullptr) {
        LOGE("torch_extract_o2r: null argument (rom=%p src=%p dst=%p)",
             rom_path, src_dir, dst_dir);
        return -1;
    }

    LOGI("torch_extract_o2r: rom=%s src=%s dst=%s", rom_path, src_dir, dst_dir);

    // Route Torch's spdlog diagnostics to logcat. spdlog's default logger is
    // a stdout sink and Android discards app stdout, so without this the one
    // message that explains a first-run failure ("ROM not recognized. Got
    // SHA-1: ..." + the supported hashes) is invisible. One-shot: the named
    // logger can only be registered once per process.
    static bool spdlog_routed = false;
    if (!spdlog_routed) {
        spdlog_routed = true;
        try {
            spdlog::set_default_logger(spdlog::android_logger_mt("torch", LOG_TAG));
        } catch (const std::exception &e) {
            LOGE("torch_extract_o2r: could not route spdlog to logcat: %s", e.what());
        }
    }

    try {
        // Defensive: confirm the ROM exists before Torch's exception path
        // takes over with a less-clear message.
        std::error_code ec;
        if (!std::filesystem::exists(rom_path, ec) || ec) {
            LOGE("torch_extract_o2r: ROM does not exist: %s (%s)",
                 rom_path, ec ? ec.message().c_str() : "missing");
            return -2;
        }

        Companion *instance = new Companion(
            std::filesystem::path(rom_path),
            ArchiveType::O2R,
            /*debug=*/false,
            std::string(src_dir),
            std::string(dst_dir)
        );
        Companion::Instance = instance;

        instance->Init(ExportType::Binary);

        // Companion::Process() has several log-and-return early exits that
        // throw nothing (unrecognized ROM SHA-1, missing config.yml, bad
        // config node) — "no exception" is NOT success. It assigns
        // gConfig.outputPath just before opening the archive writer, so an
        // empty path means it bailed before ever configuring output.
        const std::string out_path = instance->GetOutputPath();

        // Unlike main.cpp's o2r subcommand (where the process exits right
        // after), the game Activity runs in this same process — leaving the
        // singleton alive would leak the full ROM copy (~64 MB on a phone)
        // into the game's heap. Extraction is one-shot, so tear it down.
        Companion::Instance = nullptr;
        delete instance;

        if (out_path.empty()) {
            LOGE("torch_extract_o2r: Torch aborted before configuring output — "
                 "the ROM is not a supported dump (SHA-1 mismatch), or "
                 "config.yml/yamls are missing under %s", src_dir);
            return 3;
        }
        std::error_code out_ec;
        if (!std::filesystem::exists(out_path, out_ec) || out_ec) {
            LOGE("torch_extract_o2r: Torch finished but %s was not written "
                 "(disk full or write failure)", out_path.c_str());
            return 4;
        }

        LOGI("torch_extract_o2r: extraction completed (%s)", out_path.c_str());
        derive_android_css_stage_assets(rom_path, dst_dir);
        return 0;
    } catch (const std::exception &e) {
        LOGE("torch_extract_o2r: std::exception: %s", e.what());
        return 1;
    } catch (...) {
        LOGE("torch_extract_o2r: non-std exception");
        return 2;
    }
}

/* ========================================================================= */
/*  JNI bridge — called from Java RomImporter on a background thread        */
/* ========================================================================= */

/**
 * Java-callable wrapper around torch_extract_o2r. All three jstrings are
 * Java-owned UTF-16 internally; we acquire UTF-8 views with
 * GetStringUTFChars and release them after the C call returns.
 *
 * Java signature:
 *   public static native int extractO2R(String rom, String src, String dst);
 */
JNIEXPORT jint JNICALL
Java_com_jrickey_battleship_RomImporter_extractO2R(
    JNIEnv *env, jclass /*clazz*/,
    jstring rom_jstr, jstring src_jstr, jstring dst_jstr) {

    if (rom_jstr == nullptr || src_jstr == nullptr || dst_jstr == nullptr) {
        LOGE("RomImporter.extractO2R: null jstring (rom=%p src=%p dst=%p)",
             rom_jstr, src_jstr, dst_jstr);
        return -10;
    }

    const char *rom = env->GetStringUTFChars(rom_jstr, nullptr);
    const char *src = env->GetStringUTFChars(src_jstr, nullptr);
    const char *dst = env->GetStringUTFChars(dst_jstr, nullptr);

    jint result;
    if (rom == nullptr || src == nullptr || dst == nullptr) {
        LOGE("RomImporter.extractO2R: GetStringUTFChars failed");
        result = -11;
    } else {
        result = (jint) torch_extract_o2r(rom, src, dst);
    }

    if (rom) env->ReleaseStringUTFChars(rom_jstr, rom);
    if (src) env->ReleaseStringUTFChars(src_jstr, src);
    if (dst) env->ReleaseStringUTFChars(dst_jstr, dst);

    return result;
}

} // extern "C"

#endif // __ANDROID__
