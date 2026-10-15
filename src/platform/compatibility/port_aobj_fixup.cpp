/**
 * port_aobj_fixup.cpp — see port_aobj_fixup.h for architecture notes.
 *
 * The opcode size table below mirrors the decomp parser's switch in
 * src/sys/objanim.c — gcParseDObjAnimJoint (opcodes 0-17) and
 * gcParseMObjMatAnimJoint (opcodes 0-14, 18-22).  Extending the parser
 * with a new opcode means extending this walker too.
 */

#include "port_aobj_fixup.h"
#include "port_log.h"
#include "resources/relocation/RelocPointerTable.h"

#include <cstdint>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace {

/* Fixup bookkeeping. The walker's verdicts are keyed on raw addresses inside
 * fighter figatree heaps:
 *   kHeads    - stream heads the walker un-halfswapped (incl. recursion targets)
 *   kRejected - heads it decided were not halfswapped EVENT32
 *   kFixed    - every u32 it has un-halfswapped. The parser calls the walker
 *               with the stream's CURRENT position, so later calls land
 *               inside already-converted streams; head-keyed sets can't see
 *               that and the raw-vs-unswapped heuristic accepted 1-command
 *               tails, double-swapping them. A walk now stops at the first
 *               already-native word: idempotent per word, independent of
 *               which joint or tick reached the data first.
 *   kVisited  - one-shot in-place fixups (interp.c spline data).
 *
 * Inside a registered figatree range the verdicts live in per-range bitmaps
 * (1 bit per byte) so rollback can snapshot heap bytes and their fixup state
 * together (port_aobj_fixup_snapshot_*); anything else falls back to sets. */
enum Kind { kHeads, kRejected, kFixed, kVisited, kKindNum };

struct Range {
    uintptr_t base;
    uintptr_t end;
    std::vector<uint8_t> bits[kKindNum];
};

/* Registered fighter-figatree file ranges. The walker only operates on head
 * pointers inside one of these, so a stream from a non-halfswapped file is
 * never un-halfswapped. Heaps are reloaded at the same base on every status
 * change; a re-registration resets that range in place. */
std::vector<Range> sHalfswappedRanges;
std::unordered_set<uintptr_t> sOutOfRange[kKindNum];

Range *find_range(uintptr_t addr) {
    for (auto &r : sHalfswappedRanges) {
        if (addr >= r.base && addr < r.end) return &r;
    }
    return nullptr;
}

bool is_in_halfswapped_range(const void *p) {
    return find_range(reinterpret_cast<uintptr_t>(p)) != nullptr;
}

bool has(Kind kind, uintptr_t addr) {
    Range *r = find_range(addr);
    if (r == nullptr) return sOutOfRange[kind].count(addr) != 0;
    uintptr_t off = addr - r->base;
    return (r->bits[kind][off >> 3] >> (off & 7)) & 1;
}

/* Returns true when newly added. */
bool add(Kind kind, uintptr_t addr) {
    Range *r = find_range(addr);
    if (r == nullptr) return sOutOfRange[kind].insert(addr).second;
    uintptr_t off = addr - r->base;
    uint8_t mask = static_cast<uint8_t>(1u << (off & 7));
    uint8_t &byte = r->bits[kind][off >> 3];
    if (byte & mask) return false;
    byte |= mask;
    return true;
}

/* Safety cap per stream: long-form figatree animations rarely exceed a
 * few hundred events.  65k u32s = 256 KB of stream data — far beyond
 * any realistic authored animation and well short of "walk forever". */
constexpr int kStreamStepLimit = 65536;

inline uint32_t unhalfswap(uint32_t v) {
    return (v << 16) | (v >> 16);
}

inline uint32_t cmd_opcode(uint32_t cmd) {
    /* AObjEvent32.command layout matches src/sys/objtypes.h LE variant:
     * payload:15 (bits 0..14), flags:10 (bits 15..24), opcode:7 (25..31). */
    return (cmd >> 25) & 0x7F;
}

inline uint32_t cmd_flags(uint32_t cmd) {
    return (cmd >> 15) & 0x3FF;
}

inline int popcount_bits(uint32_t v, int first_bit, int count) {
    int c = 0;
    for (int i = 0; i < count; i++) {
        if (v & (1u << (first_bit + i))) c++;
    }
    return c;
}

/* Un-halfswap a data u32 in place and advance. */
inline void unswap_data(uint32_t *&ev) {
    *ev = unhalfswap(*ev);
    ev++;
}

/* Advance past a token slot without modifying it.  Reloc chain wrote the
 * token index in native u32 byte order; portRelocFixupFighterFigatree
 * skipped that slot, so it is already correct. */
inline void skip_token(uint32_t *&ev) {
    ev++;
}

/* Walker uses a two-phase approach: first phase validates the stream by
 * simulating the walk without modifying data (it computes the
 * un-halfswapped u32 and reads the opcode from it but doesn't write
 * anywhere).  It records the set of u32 addresses that WOULD be
 * modified.  If the simulation completes — hits a proper terminator
 * (End, Jump, SetAnim) with only valid opcodes — the second phase
 * applies the halfswap fix to every collected address.  If the
 * simulation hits an unknown opcode or runs away, the stream is added
 * to the kRejected verdicts and left untouched.
 *
 * This protects streams that aren't truly EVENT32 from silent
 * corruption: gcParseDObjAnimJoint gets called with some pointers
 * that point to EVENT16 data or non-figatree data (because the same
 * call site handles multiple animation types). */

struct ScanCtx {
    std::vector<uint32_t *> pending;   /* u32 addresses to un-halfswap (only populated when unhalfswap=true) */
    std::unordered_set<uintptr_t> visited;
    int total_steps;
    bool unhalfswap;       /* interpret each command as unhalfswap(raw); collect pending addresses */
    bool follow_recurse;   /* follow Jump/SetAnim tokens into sub-streams */
};

bool scan(uint32_t *head, ScanCtx &ctx);

bool scan_recurse(uint32_t token, ScanCtx &ctx) {
    if (!ctx.follow_recurse) return true;
    if (token == 0) return true;
    void *target = portRelocTryResolvePointer(token);
    if (target == nullptr) return true; /* bad token — ignore, let parser deal */
    return scan(static_cast<uint32_t *>(target), ctx);
}

bool scan(uint32_t *head, ScanCtx &ctx) {
    if (head == nullptr) return true;
    if (!is_in_halfswapped_range(head)) return true; /* not our data; skip silently */
    uintptr_t key = reinterpret_cast<uintptr_t>(head);
    if (has(kHeads, key)) return true;
    if (has(kRejected, key)) return false;
    if (!ctx.visited.insert(key).second) return true; /* already scanning this head */

    uint32_t *ev = head;

    while (true) {
        if (++ctx.total_steps > kStreamStepLimit) {
            return false;
        }
        /* Rest of this stream was converted by an earlier walk. */
        if (ctx.unhalfswap && has(kFixed, reinterpret_cast<uintptr_t>(ev))) {
            return true;
        }

        uint32_t cmd_raw = *ev;
        uint32_t cmd = ctx.unhalfswap ? unhalfswap(cmd_raw) : cmd_raw;
        if (ctx.unhalfswap) {
            ctx.pending.push_back(ev);
        }
        ev++;

        uint32_t opcode = cmd_opcode(cmd);
        uint32_t flags_in_cmd = cmd_flags(cmd);

        switch (opcode) {
        case 0:  /* End */
            return true;

        case 1:  /* Jump — [token] */
        case 14: /* SetAnim — [token] */
            if (!scan_recurse(*ev, ctx)) return false;
            ev++;
            return true;

        case 2:  /* Wait */
        case 15: /* SetFlags */
        case 16: /* ANIM_CMD_16 */
            break;

        case 13: /* SetInterp — [token] (no recurse) */
            ev++;
            break;

        /* Flags come from the command word itself (cmd.flags) — the
         * decomp reads them via AObjAnimAdvance (post-increment) so
         * what looks like "advance past header to get flags from next"
         * is actually just reading the just-consumed command's flags.
         * All of the opcodes below consume the command plus zero or
         * more f32 payload slots. */

        case 3: case 4: /* SetValBlock, SetVal */
        case 7:         /* SetTargetRate */
        case 8: case 9: /* SetVal0RateBlock, SetVal0Rate */
        case 10: case 11: /* SetValAfterBlock, SetValAfter */
        case 17:        /* ANIM_CMD_17 */
        case 18: case 19: case 20: case 21: /* SetExtVal* (MObj) */
        {
            int n = popcount_bits(flags_in_cmd, 0, 10);
            if (n > 10) return false;
            for (int k = 0; k < n; k++) {
                if (ctx.unhalfswap) ctx.pending.push_back(ev);
                ev++;
            }
            break;
        }

        case 5: case 6: /* SetValRate(Block) — value+rate pairs */
        {
            int n = popcount_bits(flags_in_cmd, 0, 10);
            if (n > 10) return false;
            for (int k = 0; k < 2 * n; k++) {
                if (ctx.unhalfswap) ctx.pending.push_back(ev);
                ev++;
            }
            break;
        }

        case 12: /* ANIM_CMD_12 — no f values, flags-only effect */
            break;

        case 22: /* ANIM_CMD_22 (MObj) — N u32 from flag bits [0..4] */
        {
            int n = popcount_bits(flags_in_cmd, 0, 5);
            if (n > 5) return false;
            for (int k = 0; k < n; k++) {
                if (ctx.unhalfswap) ctx.pending.push_back(ev);
                ev++;
            }
            break;
        }

        default:
            return false; /* invalid opcode — reject the whole stream */
        }
    }
}

void walk(uint32_t *head) {
    if (head == nullptr) return;
    if (!is_in_halfswapped_range(head)) return;
    uintptr_t key = reinterpret_cast<uintptr_t>(head);
    if (has(kHeads, key)) return;
    if (has(kRejected, key)) return;
    if (has(kFixed, key)) return; /* position inside an already-converted stream */

    /* Phase 1: measure main-stream length under the un-halfswap
     * interpretation (no recursion yet — we just need the count for
     * the length comparison in phase 2).  If the un-halfswap scan
     * can't even validate the main stream, the stream isn't
     * halfswap-corrupted EVENT32 — reject. */
    ScanCtx unswap_main;
    unswap_main.total_steps = 0;
    unswap_main.unhalfswap = true;
    unswap_main.follow_recurse = false;
    bool unswap_valid = scan(head, unswap_main);

    if (!unswap_valid) {
        add(kRejected, key);
        return;
    }

    /* Phase 2: measure main-stream length under the raw (halfswap-
     * applied) interpretation.  If raw validates AND walks >= 2
     * commands, the stream is genuinely ambiguous and we leave it
     * alone (sibling fixup passes may have rewritten it to native
     * form — see c6cc310).
     *
     * The length threshold is the key heuristic.  A halfswap-
     * corrupted stream's first u32 often has bits [31:25] = 0 when
     * the original upper 16 bits held a non-End opcode: halfswap
     * moves the original opcode bits to [15:9] and places the
     * original low payload bits into [31:16], which are frequently
     * zero.  Bits [31:25] then read as opcode 0 (End) and the raw
     * scan validates in a single step.  A genuine native stream
     * carries real animation commands (SetVal, Wait, etc.) before
     * reaching End, so a raw scan of length >= 2 is a strong signal
     * that the stream is actually native; length 1 is almost always
     * a halfswap-coincidence false positive.
     *
     * Recursion is disabled for both comparison scans because
     * Jump/SetAnim targets may have been un-halfswapped by an earlier
     * walk and so aren't in raw form anymore. */
    ScanCtx raw_main;
    raw_main.total_steps = 0;
    raw_main.unhalfswap = false;
    raw_main.follow_recurse = false;
    bool raw_valid = scan(head, raw_main);

    if (raw_valid && raw_main.total_steps >= 2) {
        add(kRejected, key);
        return;
    }

    /* Un-halfswap wins.  Do the full scan WITH recursion so we
     * collect the complete pending slot list across Jump/SetAnim
     * sub-streams. */
    ScanCtx full;
    full.total_steps = 0;
    full.unhalfswap = true;
    full.follow_recurse = true;
    if (!scan(head, full)) {
        /* Main stream validated but a sub-stream failed.  Don't
         * partially un-halfswap; reject the whole tree. */
        add(kRejected, key);
        return;
    }

    for (uint32_t *p : full.pending) {
        if (add(kFixed, reinterpret_cast<uintptr_t>(p))) {
            *p = unhalfswap(*p);
        }
    }
    for (uintptr_t k : full.visited) {
        add(kHeads, k);
    }
}

} // namespace

extern "C" void port_aobj_event32_unhalfswap_stream(void *head) {
    walk(static_cast<uint32_t *>(head));
}

extern "C" void port_aobj_register_halfswapped_range(void *base, unsigned long size) {
    if (base == nullptr || size == 0) return;
    uintptr_t b = reinterpret_cast<uintptr_t>(base);
    uintptr_t e = b + static_cast<uintptr_t>(size);
    /* Evict every cache keyed on an address inside the new range —
     * fighter figatree heaps are bump-reset between status changes, so a
     * later load lands fresh halfswap-corrupted bytes at addresses a
     * previous load already populated.  Any cached "already processed"
     * verdict from the previous load is stale once the bytes change.
     *
     * Every verdict kind resets in lockstep (the range's bitmaps are
     * re-zeroed below):
     *   - kVisited: per-block one-shot fixup tracker (interp.c spline data).
     *   - kHeads / kFixed: streams and words the walker converted. Stale
     *     entries make the walker short-circuit on a fresh load's stream
     *     that lands at the same address.
     *   - kRejected: streams the walker decided not to touch. A stream that
     *     was non-event32 in the previous load would otherwise block a real
     *     event32 stream in the new load from being un-halfswapped.
     *
     * Symptom that surfaced the missing head/rejected eviction: Master Hand's okutsubushi /
     * okupunch / drill animations parsed garbage — gcParseDObjAnimJoint
     * would hit a halfswap-corrupted command word with an out-of-range
     * opcode (typically 64) and bail with "UNHANDLED opcode=64 — ending
     * anim", leaving TransN.translate at (0,0,0) for the rest of the
     * attack.  Same shape as project_fixup_idempotency_heap_reuse: cache
     * keyed on pointer, heap reuses addresses, fresh bytes skip fixup. */
    for (auto &set : sOutOfRange) {
        for (auto it = set.begin(); it != set.end(); ) {
            if (*it >= b && *it < e) it = set.erase(it);
            else ++it;
        }
    }
    /* A range overlapping the new one at a different base held other data;
     * drop it (its verdicts are stale once these bytes change). */
    for (auto it = sHalfswappedRanges.begin(); it != sHalfswappedRanges.end(); ) {
        if (it->base != b && it->base < e && b < it->end) it = sHalfswappedRanges.erase(it);
        else ++it;
    }
    Range *range = nullptr;
    for (auto &r : sHalfswappedRanges) {
        if (r.base == b) range = &r;
    }
    if (range == nullptr) {
        sHalfswappedRanges.push_back({b, e, {}});
        range = &sHalfswappedRanges.back();
    }
    range->end = e;
    for (auto &bits : range->bits) {
        bits.assign((size + 7) / 8, 0);
    }
}

extern "C" int port_aobj_unhalfswap_visit(const void *p) {
    if (p == nullptr) return 1;
    return add(kVisited, reinterpret_cast<uintptr_t>(p)) ? 0 : 1;
}

extern "C" void port_aobj_event32_unhalfswap_reset(void) {
    sHalfswappedRanges.clear();
    for (auto &set : sOutOfRange) {
        set.clear();
    }
}

/* Rollback (sys/netrollback.c): serialize every range and its bitmaps.
 * Layout: u32 count, then per range {u64 base, u64 end, bitmaps...}. */
extern "C" size_t port_aobj_fixup_snapshot_size(void) {
    size_t size = sizeof(uint32_t);
    for (const auto &r : sHalfswappedRanges) {
        size += 2 * sizeof(uint64_t) + kKindNum * r.bits[0].size();
    }
    return size;
}

extern "C" size_t port_aobj_fixup_snapshot_save(void *dst, size_t capacity) {
    size_t size = port_aobj_fixup_snapshot_size();
    if (dst == nullptr || capacity < size) return 0;
    uint8_t *out = static_cast<uint8_t *>(dst);
    uint32_t count = static_cast<uint32_t>(sHalfswappedRanges.size());
    memcpy(out, &count, sizeof(count));
    out += sizeof(count);
    for (const auto &r : sHalfswappedRanges) {
        uint64_t bounds[2] = { r.base, r.end };
        memcpy(out, bounds, sizeof(bounds));
        out += sizeof(bounds);
        for (const auto &bits : r.bits) {
            memcpy(out, bits.data(), bits.size());
            out += bits.size();
        }
    }
    return size;
}

extern "C" void port_aobj_fixup_snapshot_load(const void *src, size_t size) {
    if (src == nullptr || size < sizeof(uint32_t)) return;
    const uint8_t *in = static_cast<const uint8_t *>(src);
    const uint8_t *in_end = in + size;
    uint32_t count = 0;
    memcpy(&count, in, sizeof(count));
    in += sizeof(count);
    sHalfswappedRanges.clear();
    for (uint32_t i = 0; i < count && in + 2 * sizeof(uint64_t) <= in_end; i++) {
        uint64_t bounds[2];
        memcpy(bounds, in, sizeof(bounds));
        in += sizeof(bounds);
        Range r{ static_cast<uintptr_t>(bounds[0]), static_cast<uintptr_t>(bounds[1]), {} };
        size_t bytes = (static_cast<size_t>(r.end - r.base) + 7) / 8;
        for (auto &bits : r.bits) {
            if (in + bytes > in_end) return;
            bits.assign(in, in + bytes);
            in += bytes;
        }
        sHalfswappedRanges.push_back(std::move(r));
    }
}

extern "C" int port_aobj_is_in_halfswapped_range(const void *p) {
    return is_in_halfswapped_range(p) ? 1 : 0;
}
