// heap_watchpoint.cpp — hardware data watchpoints on the heap-corruption
// victim candidates.
//
// The ucrtbased debug-list corruption observed in components-verify Debug
// builds repeatedly damages the same KIND of block: a 16-byte plain malloc
// (use=_NORMAL_BLOCK, file_name=null) made inside a /MDd DLL very early in
// startup. Its debug request number drifts slightly between builds (830,
// 860 observed) so we cannot key on one request — instead the probe walk
// hands us every node and we arm DR0-DR3 on the `next` link field of the
// first four live blocks matching the signature. The damage is a wild
// pointer store into a link field, so the moment it lands the data
// breakpoint fires as STATUS_SINGLE_STEP and the vectored handler logs the
// writing instruction's RIP + module offset + raw stack — the identity of
// the corruptor, which no post-mortem autopsy could produce.
//
// Gated by SSB64_HEAP_WATCH=1. Re-arms on later probe calls so threads
// spawned after the first arm get the debug registers too. Write
// breakpoints are traps (fire AFTER the write) — the handler logs and
// continues, no instruction needs skipping.

#if defined(_MSC_VER) && defined(_DEBUG) && defined(SSB64_TEST_HOOKS)

#include <windows.h>
#include <tlhelp32.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

#include "heap_check_internal.h"

namespace {

// Victim signature seen across runs: a long-lived block allocated early —
// a 16-byte file-less block at req ~830/860, and a CRT per-thread-data
// block (use=_CRT_BLOCK, size 128, per_thread_data.cpp:128) at req ~811.
// Drift between builds is real; take anything with a low request number
// that is not a tiny transient.
constexpr long kWatchReqMin = 400;
constexpr long kWatchReqMax = 2000;

/* Register budget: heap_check_boot.cpp's gwatch owns DR0/DR1 (ucrtbased's
 * __acrt_first_block/__acrt_last_block). Victim candidates get DR2/DR3 —
 * kDrBase shifts logical slot i onto debug register 2+i so both systems
 * coexist without reprogramming each other's slots. */
constexpr int kMaxWatch = 2;
constexpr int kDrBase   = 2;
volatile uintptr_t gWatchAddrs[kMaxWatch] = {0, 0};
volatile long      gWatchReqs[kMaxWatch] = {0, 0};
volatile long      gWatchCount = 0;

// Candidate buffer: the walk hands us every matching node; we retain the
// kMaxWatch whose request numbers sit CLOSEST to the observed victim
// cluster (~811-860). A fixed first-N buffer overflows — the walk visits
// newest-first, so it filled with high-req candidates and the victim was
// never stored. Dynamic best-4 replacement has no overflow failure mode.
struct WatchCand { uintptr_t hdr; long req; };
WatchCand gCand[kMaxWatch];
int       gCandCount = 0;

HANDLE gWatchLog = INVALID_HANDLE_VALUE;

void watchLog(const char* fmt, ...) {
	if (gWatchLog == INVALID_HANDLE_VALUE) {
		return;
	}
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n <= 0) {
		return;
	}
	if (n > (int)sizeof(buf) - 1) {
		n = sizeof(buf) - 1;
	}
	DWORD w = 0;
	WriteFile(gWatchLog, buf, (DWORD)n, &w, nullptr);
	FlushFileBuffers(gWatchLog);
}

/* Module table snapshotted once — describeAddr runs inside the vectored
 * handler where GetModuleFileName is unsafe (loader lock while
 * armAllThreads may hold a DLL-loading thread suspended → deadlock). */
static ModEntry gWModTable[kModTableMax];
static volatile long gWModCount = 0;
static uintptr_t gUcrtBase = 0, gUcrtEnd = 0;

void snapshotWModules() {
	gWModCount = snapshotModules(gWModTable, &gUcrtBase, &gUcrtEnd);
}

// Resolve an address to "module+0xoff" from the cached table — no locks.
void describeAddr(uintptr_t a, char* out, size_t outSize) {
	describeModuleAddr(gWModTable, gWModCount, a, out, outSize);
}

// STATUS_SINGLE_STEP: a write watchpoint fired. DR6 B0-B3 say which.
LONG CALLBACK watchVectoredHandler(PEXCEPTION_POINTERS ep) {
	if (ep->ExceptionRecord->ExceptionCode != STATUS_SINGLE_STEP) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	const unsigned fired = (unsigned)(ep->ContextRecord->Dr6 & 0xF);
	if (fired == 0) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	const uintptr_t rip = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
	int idx = -1;
	for (int i = 0; i < kMaxWatch; ++i) {
		if (fired & (1u << (kDrBase + i))) {
			idx = i;
		}
	}
	if (idx < 0) {
		return EXCEPTION_CONTINUE_SEARCH;   // gwatch's slots 0-1 — not ours
	}
	const uintptr_t watched = gWatchAddrs[idx];
	/* Classify the writer: a write from ucrtbased's own list code means the
	 * watched block was legitimately unlinked/re-linked — the victim slot is
	 * dead weight and its recycled address now floods every later alloc.
	 * Disarm it (clear our enable bits in the live context — other threads
	 * self-disarm on their own hits) and log once. ANY other RIP is the
	 * corruptor candidate — full dump. */
	const bool ucrtWriter =
	    (gUcrtBase != 0 && rip >= gUcrtBase && rip < gUcrtEnd);
	uintptr_t written = 0;
	__try {
		written = *(volatile uintptr_t*)watched;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	if (ucrtWriter) {
		watchLog("heap-watch: slot %d retired — %p relinked by ucrt itself "
		         "(wrote %p)\n", idx, (void*)watched, (void*)written);
		gWatchAddrs[idx] = 0;
		const int dr = kDrBase + idx;
		ep->ContextRecord->Dr7 &= ~(3ull << (dr * 2));  // Ln+Gn enable off
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	char mod[160];
	describeAddr(rip, mod, sizeof(mod));
	watchLog("*** WATCHPOINT HIT rip=%p %s watched=%p wrote=%p (slot %d) tid=%lu ***\n",
	         (void*)rip, mod, (void*)watched, (void*)written, idx,
	         GetCurrentThreadId());
	__try {
		uintptr_t sp = ep->ContextRecord->Rsp;
		for (int i = 0; i < 32; ++i) {
			uintptr_t v = *(uintptr_t*)(sp + i * 8);
			if (v < 0x10000 || v >= 0x0000800000000000ull) {
				continue;
			}
			MEMORY_BASIC_INFORMATION stkMbi;
			if (VirtualQuery((LPCVOID)v, &stkMbi, sizeof(stkMbi)) == sizeof(stkMbi) &&
			    stkMbi.Type == MEM_IMAGE) {
				char f[160];
				describeAddr(v, f, sizeof(f));
				watchLog("    [stk+%02x] %s\n", i * 8, f);
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	// Registers stay armed; the write breakpoint fires on every write.
	return EXCEPTION_CONTINUE_EXECUTION;
}

// Program our watch addresses on one thread's DR2/DR3.
int armThread(DWORD tid) {
	const bool self = (tid == GetCurrentThreadId());
	HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
	                           THREAD_SET_CONTEXT,
	                       FALSE, tid);
	if (th == nullptr) {
		return 0;
	}
	/* SuspendThread on the CURRENT thread suspends the caller before
	 * SetThreadContext/ResumeThread ever run — self-deadlock, and the
	 * reason earlier watchpoint runs froze silently right after arming.
	 * Our own DRs can be programmed without suspending ourselves. */
	if (!self && SuspendThread(th) == (DWORD)-1) {
		CloseHandle(th);
		return 0;
	}
	CONTEXT ctx;
	ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
	if (GetThreadContext(th, &ctx)) {
		/* Own only slots 2-3: clear L2/L3 enables (bits 4-7) and RW/LEN for
		 * DR2+DR3 (bits 24-31). Slots 0-1 (gwatch's ucrt globals) and the
		 * reserved bits pass through untouched. */
		DWORD64 dr7 = ctx.Dr7 & ~0xFF0000F0ull;
		for (int i = 0; i < kMaxWatch; ++i) {
			if (gWatchAddrs[i] == 0) {
				continue;
			}
			const int dr = kDrBase + i;
			(&ctx.Dr0)[dr] = (DWORD64)gWatchAddrs[i];
			dr7 |= (1ull << (dr * 2)) |            // Ln local enable
			       (1ull << (16 + dr * 4)) |       // RWn = 01 write
			       (3ull << (18 + dr * 4));        // LENn = 11 eight bytes
		}
		ctx.Dr7 = dr7;
		SetThreadContext(th, &ctx);
	}
	if (!self) {
		ResumeThread(th);
	}
	CloseHandle(th);
	return 1;
}

// Dump the loaded-module map once at arm time: the stomped link values
// observed point into a transient driver-DLL region that may be unloaded
// by the time the damage is found — this map resolves them post-mortem.
void dumpModuleMap() {
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE |
	                                     TH32CS_SNAPMODULE32, 0);
	if (snap == INVALID_HANDLE_VALUE) {
		return;
	}
	MODULEENTRY32 me;
	me.dwSize = sizeof(me);
	watchLog("heap-watch: module map at arm time\n");
	if (Module32First(snap, &me)) {
		do {
			watchLog("    mod %p-%-18p %s\n",
			         (void*)me.modBaseAddr,
			         (void*)((uintptr_t)me.modBaseAddr + me.modBaseSize),
			         me.szModule);
		} while (Module32Next(snap, &me));
	}
	CloseHandle(snap);
}

/* Threads already armed under the current watch generation are skipped —
 * a probe fires once per loaded file, and re-suspending ~90 threads each
 * time starved the resource workers enough to stall frames. The list is
 * reset whenever watch addresses change (see portHeapWatchFlush), so a
 * re-arm still reaches every thread; only steady-state probes go cheap. */
static DWORD gArmedTids[256];
static long  gArmedCount = 0;

void armAllThreads() {
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snap == INVALID_HANDLE_VALUE) {
		return;
	}
	THREADENTRY32 te;
	te.dwSize = sizeof(te);
	const DWORD self = GetCurrentProcessId();
	const DWORD myTid = GetCurrentThreadId();
	int armed = 0;
	if (Thread32First(snap, &te)) {
		do {
			if (te.th32OwnerProcessID != self || te.th32ThreadID == myTid) {
				continue;
			}
			bool known = false;
			for (long k = 0; k < gArmedCount; ++k) {
				if (gArmedTids[k] == te.th32ThreadID) {
					known = true;
					break;
				}
			}
			if (known) {
				continue;
			}
			if (armThread(te.th32ThreadID) && gArmedCount < 256) {
				gArmedTids[gArmedCount++] = te.th32ThreadID;
			}
			++armed;
		} while (Thread32Next(snap, &te));
	}
	CloseHandle(snap);
	for (long k = 0; k < gArmedCount; ++k) {
		if (gArmedTids[k] == myTid) {
			return;
		}
	}
	if (armThread(myTid) && gArmedCount < 256) {
		gArmedTids[gArmedCount++] = myTid;
	}
	++armed;
}

} // namespace

// Called from the heap probe walk for every live node; matching victims
// get buffered. portHeapWatchFlush() arms DR2/DR3 on each candidate's
// `prev` field (header+8) — in every observed break the stomped field was
// a `prev` pointer of an early long-lived node (next->prev == victim
// check fired on the node BEFORE the damaged one).
extern "C" __declspec(dllexport)
void portHeapWatchCheck(void* header, long requestNumber,
                        unsigned long long dataSize, const void* fileName) {
	if (gWatchLog == INVALID_HANDLE_VALUE) {
		if (std::getenv("SSB64_HEAP_WATCH") == nullptr) {
			return;
		}
		gWatchLog = CreateFileA("heap_watch.log", GENERIC_WRITE,
		                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
		                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (gWatchLog == INVALID_HANDLE_VALUE) {
			return;
		}
		AddVectoredExceptionHandler(1, &watchVectoredHandler);
	}
	if (header == nullptr) {
		// Re-arm request from an armed probe: cover late-spawned threads.
		if (gWatchCount > 0) {
			armAllThreads();
		}
		return;
	}
	if (requestNumber < kWatchReqMin || requestNumber >= kWatchReqMax) {
		return;
	}
	// Both observed victim classes: early plain mallocs and CRT per-thread
	// data (use=2, file=per_thread_data.cpp). Reject blocks with source
	// info from unrelated early inits only when they are not PTDs — the
	// req window is the discriminator, not the size.
	(void)dataSize; (void)fileName;
	const uintptr_t h = (uintptr_t)header;
	// We watch the `prev` field (header+8): both observed breaks stomped a
	// node's prev link, never its next.
	const uintptr_t field = h + 8;
	for (int i = 0; i < kMaxWatch; ++i) {
		if (gWatchAddrs[i] == field) {
			return;   // already armed on this header
		}
	}
	for (int i = 0; i < gCandCount; ++i) {
		if (gCand[i].hdr == h) {
			return;   // already buffered this walk
		}
	}
	/* Track the kMaxWatch candidates CLOSEST to the observed victim cluster
	 * (~835), both in the per-walk buffer AND in the armed slots — the
	 * victim is allocated AFTER the first probe, so slots must upgrade as
	 * better candidates appear in later walks. The old code stopped
	 * collecting once armed, which guaranteed the victim (born mid-window)
	 * was never watched. */
	constexpr long kVictimCenter = 835;
	const long d0 = requestNumber - kVictimCenter;
	const long dist0 = d0 < 0 ? -d0 : d0;
	if (gCandCount < kMaxWatch) {
		gCand[gCandCount].hdr = h;
		gCand[gCandCount].req = requestNumber;
		++gCandCount;
		return;
	}
	int worst = -1;
	long worstDist = -1;
	for (int i = 0; i < gCandCount; ++i) {
		const long d = gCand[i].req - kVictimCenter;
		const long dist = d < 0 ? -d : d;
		if (dist > worstDist) {
			worstDist = dist;
			worst = i;
		}
	}
	if (worst >= 0 && dist0 < worstDist) {
		gCand[worst].hdr = h;
		gCand[worst].req = requestNumber;
	}
}

extern "C" __declspec(dllexport) void portHeapWatchFlush() {
	if (gWatchLog == INVALID_HANDLE_VALUE) {
		return;
	}
	/* Handler-order fix: the game's portWindowsVectoredHandler registers
	 * with firstHandler=1 during PortInit — AFTER our static-init
	 * registration — so it sits ahead of us and sees every single-step hit
	 * (2 GB of flood per run). Re-prepending on EVERY flush keeps us ahead
	 * even if the engine re-registers later (late boot phases do). Remove
	 * the stale entry first so the handler list doesn't grow per probe. */
	static void* sHandle = nullptr;
	void* h = AddVectoredExceptionHandler(1, &watchVectoredHandler);
	if (sHandle != nullptr) {
		RemoveVectoredExceptionHandler(sHandle);
	}
	sHandle = h;
	if (gWModCount == 0) {
		snapshotWModules();
	}
	const bool firstArm = (gWatchCount == 0);
	// Merge this walk's best-4 into the armed set: for each candidate
	// closer to the victim cluster than a currently watched header, swap
	// the slot's target. Slots are stable unless a better victim appears —
	// re-arming only the changed slots keeps per-probe cost flat.
	constexpr long kVictimCenter = 835;
	bool changed = false;
	for (int c = 0; c < gCandCount; ++c) {
		const long cd = gCand[c].req - kVictimCenter;
		const long cdist = cd < 0 ? -cd : cd;
		// Find this candidate's best slot: empty slot first, else the
		// watched slot whose request is farthest from the cluster.
		int slot = -1;
		long slotWorstDist = -1;
		for (int i = 0; i < kMaxWatch; ++i) {
			if (gWatchAddrs[i] == gCand[c].hdr + 8) {
				slot = -2;   // already armed
				break;
			}
		}
		if (slot == -2) {
			continue;
		}
		for (int i = 0; i < kMaxWatch; ++i) {
			if (gWatchAddrs[i] == 0) {
				slot = i;
				break;
			}
		}
		if (slot < 0) {
			for (int i = 0; i < kMaxWatch; ++i) {
				// Distance of the armed header's own req — we don't store it,
				// so compare by candidate quality instead: swap only if the
				// slot's req tracking shows a worse match. Keep req per slot.
				const long sd = gWatchReqs[i] - kVictimCenter;
				const long sdist = sd < 0 ? -sd : sd;
				if (sdist > slotWorstDist) {
					slotWorstDist = sdist;
					slot = i;
				}
			}
			if (cdist >= slotWorstDist) {
				continue;   // new candidate not better than worst armed slot
			}
		}
		gWatchAddrs[slot] = gCand[c].hdr + 8;   // the node's `prev` field
		gWatchReqs[slot] = gCand[c].req;
		if (slot >= gWatchCount) {
			gWatchCount = slot + 1;
		}
		watchLog("heap-watch: slot %d %s on header %p (req=%ld)\n",
		         slot, firstArm ? "armed" : "re-armed",
		         (void*)gCand[c].hdr, gCand[c].req);
		changed = true;
	}
	gCandCount = 0;
	if (gWatchCount > 0 && (firstArm || changed)) {
		if (firstArm) {
			dumpModuleMap();
		}
		gArmedCount = 0;   // addresses changed → every thread re-arms
		armAllThreads();
		watchLog("heap-watch: threads armed, %ld watch slots live\n", gWatchCount);
	}
}

extern "C" __declspec(dllexport) int portHeapWatchArmed() {
	return gWatchCount > 0;
}

extern "C" __declspec(dllexport) int portHeapWatchSaturated() {
	/* All live slots already sit inside the observed victim cluster
	 * (|req - 835| <= 60). Once saturated, later probes gain nothing by
	 * re-walking the list — detection lives on the data breakpoints, and
	 * the O(list) walks were starving the resource workers. */
	if (gWatchCount == 0) {
		return 0;
	}
	constexpr long kVictimCenter = 835;
	for (long i = 0; i < gWatchCount; ++i) {
		if (gWatchAddrs[i] == 0) {
			return 0;
		}
		const long d = gWatchReqs[i] - kVictimCenter;
		const long dist = d < 0 ? -d : d;
		if (dist > 60) {
			return 0;
		}
	}
	return 1;
}

#else
extern "C" void portHeapWatchCheck(void*, long, unsigned long long, const void*) {}
extern "C" void portHeapWatchFlush() {}
extern "C" int portHeapWatchArmed() { return 0; }
extern "C" int portHeapWatchSaturated() { return 0; }
#endif
