/*
 * heap_check_boot.cpp — optional CRT debug-heap verification for Debug
 * MSVC test builds.
 *
 * SSB64_HEAP_CHECK=1 installs point-in-time integrity probes (the shared
 * ucrtbased heap via an alloc-hook-fed list walk — see below) at frame
 * boundaries and around zip_open/zip_close. SSB64_HEAP_CHECK_ALWAYS=1
 * additionally enables the CRT's own per-operation validation
 * (_CRTDBG_CHECK_ALWAYS_DF, very slow).
 *
 * SSB64_HEAP_HOOK=1 registers an _CrtSetAllocHook INTO ucrtbased's debug
 * heap: every vendored-DLL alloc/free runs a bounded link/gap check on
 * the affected block plus one rotating sample from a ring of live blocks,
 * so a wild write to a debug-block header is reported at the next heap
 * event — with the victim's allocation identity — instead of surfacing
 * as a far-away 0xC0000374 or as a crash inside the CRT's own damage
 * reporter (which faults formatting a stomped _file_name).
 *
 * Implemented as TU-local initializers so it installs before main() and
 * needs no edits to shared startup files (parallel sessions rewrite
 * port.cpp). Test-hook builds only — a shipped binary contains none of
 * this.
 */

#if defined(_MSC_VER) && defined(_DEBUG) && defined(SSB64_TEST_HOOKS)
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <crtdbg.h>
#include <malloc.h>
#include <intrin.h>
#include <io.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* heap_watchpoint.cpp — arms a hardware write-watchpoint on the
 * deterministic corruption victim's link fields once the probe walk finds
 * the block carrying the watched debug request number. */
extern "C" void portHeapWatchCheck(void* header, long requestNumber,
                                   unsigned long long dataSize, const void* fileName);
extern "C" void portHeapWatchFlush();
extern "C" int portHeapWatchArmed();
extern "C" int portHeapWatchSaturated();

namespace {
HANDLE gProbeLog = INVALID_HANDLE_VALUE;
char   gProbeBuf[1024];

void probeLog(const char* fmt, ...) {
	if (gProbeLog == INVALID_HANDLE_VALUE) {
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	const int n = vsnprintf(gProbeBuf, sizeof(gProbeBuf), fmt, ap);
	va_end(ap);
	if (n > 0) {
		DWORD written = 0;
		// WriteFile on a plain handle is already unbuffered — FlushFileBuffers
		// per message cost the earlier runs minutes of syscall traffic.
		WriteFile(gProbeLog, gProbeBuf,
		          (DWORD)(n < (int)sizeof(gProbeBuf) ? n : (int)sizeof(gProbeBuf) - 1),
		          &written, nullptr);
	}
}

struct HeapCheckBoot {
	HeapCheckBoot() {
		const char* hc = std::getenv("SSB64_HEAP_CHECK_ALWAYS");
		const char* hcCheap = std::getenv("SSB64_HEAP_CHECK");
		const char* hh = std::getenv("SSB64_HEAP_HOOK");
		const bool always = !(hc == nullptr || hc[0] == '\0' || (hc[0] == '0' && hc[1] == '\0'));
		const bool cheap = !(hcCheap == nullptr || hcCheap[0] == '\0' || (hcCheap[0] == '0' && hcCheap[1] == '\0'));
		const bool hook = !(hh == nullptr || hh[0] == '\0' || (hh[0] == '0' && hh[1] == '\0'));
		if (!always && !cheap && !hook) {
			return;
		}
		gProbeLog = CreateFileA("heap_probe.log", GENERIC_WRITE,
		                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
		                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		FILE* rf = std::fopen("heap_check_report.log", always ? "w" : "a");
		if (always) {
			_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_CHECK_ALWAYS_DF | _CRTDBG_LEAK_CHECK_DF);
		}
		if (rf != nullptr) {
			void* rh = reinterpret_cast<void*>(_get_osfhandle(_fileno(rf)));
			_CrtSetReportFile(_CRT_ASSERT, rh);
			_CrtSetReportFile(_CRT_ERROR, rh);
			_CrtSetReportFile(_CRT_WARN, rh);
		}
		// The exe is /MTd (own static debug CRT); vendored DLLs (zip.dll,
		// SDL2d...) are /MDd — their blocks live in ucrtbased's debug heap,
		// which our _CrtSetDbgFlag never touches. The shutdown corruption is
		// detected on THAT side, so enable check-always there too via its
		// exported _CrtSetDbgFlag. Its _CrtDbgReport stream is DLL-local as
		// well: without redirecting it, damage reports go to the debugger.
		if (HMODULE ucrt = GetModuleHandleW(L"ucrtbased.dll")) {
			using SetDbgFlagFn = int (*)(int);
			using SetReportFileFn = void* (*)(int, void*);
			using SetReportModeFn = int (*)(int, int);
			if (always) {
				if (auto setFlag = reinterpret_cast<SetDbgFlagFn>(
						GetProcAddress(ucrt, "_CrtSetDbgFlag"))) {
					setFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_CHECK_ALWAYS_DF);
				}
			}
			if (rf != nullptr) {
				auto setMode = reinterpret_cast<SetReportModeFn>(
					GetProcAddress(ucrt, "_CrtSetReportMode"));
				auto setFile = reinterpret_cast<SetReportFileFn>(
					GetProcAddress(ucrt, "_CrtSetReportFile"));
				if (setMode && setFile) {
					void* rh = reinterpret_cast<void*>(_get_osfhandle(_fileno(rf)));
					const int types[3] = {_CRT_ASSERT, _CRT_ERROR, _CRT_WARN};
					for (int i = 0; i < 3; ++i) {
						setMode(types[i], 0x1);
						setFile(types[i], rh);
					}
				}
			}
		}
	}
};
HeapCheckBoot gHeapCheckBoot;

/*
 * Shadow of ucrt's _CrtMemBlockHeader (x64: 48B header, 4B head gap,
 * user data, 4B tail gap). Letting our own code walk the list instead of
 * _CrtCheckMemory avoids the CRT damage reporter, which faults while
 * formatting a stomped _file_name with %hs — that crash hides the victim.
 */
struct ShadowCrtHeader {
	ShadowCrtHeader* next;
	ShadowCrtHeader* prev;
	char const*      file_name;
	int              line_number;
	int              block_use;
	size_t           data_size;
	long             request_number;
	unsigned char    gap[4];
};

bool addrLooksMapped(const void* p) {
	const uintptr_t a = (uintptr_t)p;
	if (a < 0x10000 || a >= 0x0000800000000000ull) {
		return false;
	}
	/* Range check alone is not enough: a list link can hold a pointer into
	 * a heap segment that was committed once but released back to the OS
	 * after the block was freed (decommitted page = valid address, MEM_FREE
	 * state). A later unlink then writes through it -> AV inside free_dbg.
	 * VirtualQuery proves the page is actually committed. */
	MEMORY_BASIC_INFORMATION mbi;
	return VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi) &&
	       mbi.State == MEM_COMMIT &&
	       (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
	                       PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
	                       PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Resolve a pointer to "module+0xoff" when it lands inside a loaded image —
// the stomped link values observed so far point into module space (a
// transient driver DLL region), so naming the module identifies the writer.
void describePtr(const void* p, char* out, size_t outSize) {
	const uintptr_t a = (uintptr_t)p;
	MEMORY_BASIC_INFORMATION mbi;
	if (VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) == sizeof(mbi) &&
	    mbi.Type == MEM_IMAGE && mbi.AllocationBase != nullptr) {
		char name[MAX_PATH] = {0};
		GetModuleFileNameA((HMODULE)mbi.AllocationBase, name, sizeof(name) - 1);
		const char* base = name;
		for (const char* q = name; *q; ++q) {
			if (*q == '\\' || *q == '/') {
				base = q + 1;
			}
		}
		snprintf(out, outSize, "%s+0x%llx", base,
		         (unsigned long long)(a - (uintptr_t)mbi.AllocationBase));
	} else {
		snprintf(out, outSize, "%p (not image%s)", p,
		         (mbi.State == MEM_COMMIT) ? ", committed" : ", unmapped");
	}
}

// Print the victim header's fields numerically — _file_name may itself be
// stomped, so it is rendered as a pointer and only dereferenced (bounded,
// SEH-guarded) after a plausibility check.
void describeVictim(const ShadowCrtHeader* h, const char* why, const char* tag) {
	probeLog("*** HEAP-AUTOPSY %s [%s]: header=%p user=%p next=%p prev=%p "
	         "file_name=%p line=%d use=%d size=%llu req=%ld ***\n",
	         why, tag, (const void*)h, (const void*)(h + 1),
	         (const void*)h->next, (const void*)h->prev,
	         (const void*)h->file_name, h->line_number, h->block_use,
	         (unsigned long long)h->data_size, h->request_number);
	if (addrLooksMapped(h->file_name)) {
		__try {
			char nameBuf[128];
			size_t i = 0;
			for (; i < sizeof(nameBuf) - 1 && h->file_name[i] != '\0'; ++i) {
				nameBuf[i] = h->file_name[i];
			}
			nameBuf[i] = '\0';
			probeLog("    alloc site: %s:%d\n", nameBuf, h->line_number);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			probeLog("    alloc site: (file_name %p unreadable)\n", (const void*)h->file_name);
		}
	}
	__try {
		const unsigned char* b = (const unsigned char*)h;
		probeLog("    header bytes:");
		for (int i = 0; i < 64; ++i) {
			if ((i % 16) == 0) {
				probeLog("\n    %04x:", i);
			}
			probeLog(" %02x", b[i]);
		}
		probeLog("\n");
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		probeLog("    (header partially unreadable)\n");
	}
	/* Cross-link truth: for a "next->prev broken"/"prev->next broken"
	 * report the value the neighbor ACTUALLY holds — that is the stomp
	 * payload (a foreign pointer, -1 fill, dead-land bytes) and often
	 * identifies the writer. */
	__try {
		if (addrLooksMapped(h->next)) {
			probeLog("    next(%p)->prev = %p%s\n", (const void*)h->next,
			         (const void*)h->next->prev,
			         h->next->prev == h ? " (consistent)" : "  <-- mismatch");
			if (h->next->prev != h) {
				char nb[160];
				describePtr(h->next->prev, nb, sizeof(nb));
				probeLog("    (mismatched prev resolves to %s)\n", nb);
				/* Also dump the next node's own identity — is it a real
				 * live block (=> its prev was stomped) or garbage memory
				 * (=> victim.next itself was stomped). */
				const ShadowCrtHeader* nn = h->next;
				probeLog("    next-node %p: next=%p prev=%p file=%p line=%d "
				         "use=%d size=%llu req=%ld\n",
				         (const void*)nn, (const void*)nn->next,
				         (const void*)nn->prev, (const void*)nn->file_name,
				         nn->line_number, nn->block_use,
				         (unsigned long long)nn->data_size, nn->request_number);
			}
			/* When next->prev points elsewhere, that "skip" node is the
			 * competing claimant for the list position — inspect whether it
			 * is still alive (use/gaps intact) or freed (0xDD fill). A live
			 * skip node means h->next was overwritten by a wild pointer
			 * store; a dead one means an unlink replayed stale links
			 * (double-free / free of an already-removed node). */
			const ShadowCrtHeader* skip = h->next->prev;
			if (skip != h && addrLooksMapped(skip)) {
				probeLog("    skip-node %p: next=%p prev=%p file=%p line=%d "
				         "use=%d size=%llu req=%ld\n",
				         (const void*)skip, (const void*)skip->next,
				         (const void*)skip->prev, (const void*)skip->file_name,
				         skip->line_number, skip->block_use,
				         (unsigned long long)skip->data_size, skip->request_number);
				if (addrLooksMapped(skip->file_name)) {
					char nb[128];
					size_t i = 0;
					for (; i < sizeof(nb) - 1 && skip->file_name[i] != '\0'; ++i) {
						nb[i] = skip->file_name[i];
					}
					nb[i] = '\0';
					probeLog("    skip-node alloc site: %s:%d\n", nb, skip->line_number);
				}
			}
		}
		if (addrLooksMapped(h->prev)) {
			probeLog("    prev(%p)->next = %p%s\n", (const void*)h->prev,
			         (const void*)h->prev->next,
			         h->prev->next == h ? " (consistent)" : "  <-- mismatch");
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		probeLog("    (neighbor links unreadable)\n");
	}
}

// Cheap plausibility for pointers: no VirtualQuery — a dereference of a
// non-committed address is caught by the caller's __try anyway, and this
// runs per free, so syscall-per-field is the difference between seconds
// and minutes during resource loading.
bool addrPlausible(const void* p) {
	const uintptr_t a = (uintptr_t)p;
	return a >= 0x10000 && a < 0x0000800000000000ull;
}

// Returns why-string if the header's list links or gaps are inconsistent.
// All dereferences run under the caller's __try — a faulted read means the
// header itself is the damage; the caller reports it.
const char* checkHeader(const ShadowCrtHeader* h) {
	if (!addrPlausible(h)) {
		return "header ptr implausible";
	}
	if (h->block_use != 1 && h->block_use != 2 && h->block_use != 4) {
		return "bad block_use";
	}
	if (h->next != nullptr) {
		if (!addrPlausible(h->next)) {
			return "next implausible";
		}
		if (h->next->prev != h) {
			return "next->prev broken";
		}
	}
	if (h->prev != nullptr) {
		if (!addrPlausible(h->prev)) {
			return "prev implausible";
		}
		if (h->prev->next != h) {
			return "prev->next broken";
		}
	}
	/* file_name/line/request live in the same header as the links — a wild
	 * write that hits only those fields (observed: file_name stomped to -1,
	 * which then kills _CrtDbgReport's strnlen) leaves links+gaps intact and
	 * would pass every check above. Validate the pointer too. */
	if (h->file_name != nullptr && !addrPlausible(h->file_name)) {
		return "file_name implausible";
	}
	if (h->file_name == nullptr && h->line_number != 0) {
		return "line set but file null";
	}
	for (int i = 0; i < 4; ++i) {
		if (h->gap[i] != 0xFD) {
			return "head gap";
		}
	}
	if (h->data_size <= 0x10000000ull) {
		const unsigned char* tail = (const unsigned char*)(h + 1) + h->data_size;
		for (int i = 0; i < 4; ++i) {
			if (tail[i] != 0xFD) {
				return "tail gap";
			}
		}
	}
	return nullptr;
}

void gwatchDescribeAddr(uintptr_t a, char* out, size_t outSize);
uintptr_t findListHeadGlobal(HMODULE m);

/* Address of the exe's own __acrt_first_block static, found by scanning
 * the exe image (no PDB dependency at runtime). Resolved lazily on the
 * first head-candidate free. */
uintptr_t gExeAnchorAddr = 0;
bool      gExeAnchorTried = false;

const ShadowCrtHeader* exeListHead() {
	if (!gExeAnchorTried) {
		gExeAnchorTried = true;
		gExeAnchorAddr = findListHeadGlobal(GetModuleHandleW(nullptr));
		if (gExeAnchorAddr != 0) {
			probeLog("heap-hook: exe __acrt_first_block global @ %p\n",
			         (const void*)gExeAnchorAddr);
		}
	}
	if (gExeAnchorAddr == 0) {
		return nullptr;
	}
	const ShadowCrtHeader* h = nullptr;
	__try {
		h = *(ShadowCrtHeader* const*)gExeAnchorAddr;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return h;
}

// Ring of live ucrtbased blocks seen by the alloc hook.
void**   gRing = nullptr;
enum { kRingSize = 8192 };
unsigned gRingPos = 0;
ShadowCrtHeader* gLastSeen = nullptr;

/* The debug_heap.cpp(996) assert `__acrt_first_block == header` fires when a
 * freed block's prev==nullptr but it is not the real list head. One prime
 * suspect is a cross-heap free: a block allocated by the exe's own /MTd CRT
 * (same header layout, self-consistent links) being freed through ucrtbased's
 * free(), where its prev happens to read null. The alloc hook cannot see
 * allocation results (ucrt calls it with userData=nullptr on _HOOK_ALLOC),
 * so list-head tracking is impossible — but HeapValidate() answers the
 * membership question directly: a ucrt-heap block validates only against
 * ucrtbased's private heap. We snapshot the process heaps at install, adopt
 * the first freed block's heap as "ours", and flag+veto any later free whose
 * block belongs to a different heap. */
HANDLE gHeaps[32];
int    gHeapCount = 0;
HANDLE gUcrtHeap  = nullptr;
/* ucrtbased prepends every debug block at the list head, so the hook can
 * track which block IS __acrt_first_block across events: alloc installs the
 * new block as head; freeing a prev==null block advances head to its next. */
ShadowCrtHeader* gExpectedHead = nullptr;

/* Window flag: while nonzero, ucrtAllocHook logs every free's pointer and
 * header fields. O2rArchive::Close sets it around zip_close so the last
 * logged free before a crash identifies the victim block. */
extern "C" __declspec(dllexport) volatile int gHeapHookFreeTrace = 0;

void ringRemove(void* userData) {
	// Only scrub the few most recent slots — full scans per free are too
	// costly; stale entries are weeded lazily by the re-check below.
	for (int i = 0; i < 64; ++i) {
		unsigned idx = (gRingPos - 1 - i) % kRingSize;
		if (gRing[idx] == userData) {
			gRing[idx] = nullptr;
		}
	}
}

/* Heap membership check for _HOOK_FREE: h is the candidate block header
 * (HeapValidate works on the heap block start, which IS the header for
 * debug blocks). Returns the owning heap if h is a block start in a heap
 * other than gUcrtHeap — i.e. a cross-heap free in progress. nullptr when
 * the block is ours or its address isn't a block start anywhere (aligned
 * allocs, interior pointers — those can't be judged). */
HANDLE foreignHeapOf(const ShadowCrtHeader* h) {
	if (gUcrtHeap != nullptr && HeapValidate(gUcrtHeap, 0, h)) {
		return nullptr;
	}
	/* Refresh the heap list — heaps can appear after install. */
	DWORD n = GetProcessHeaps(gHeapCount ? gHeapCount : 32, gHeaps);
	if (n > 32) n = 32;
	gHeapCount = (int)n;
	for (int i = 0; i < gHeapCount; ++i) {
		if (gHeaps[i] == gUcrtHeap) continue;
		if (HeapValidate(gHeaps[i], 0, h)) {
			if (gUcrtHeap == nullptr) {
				gUcrtHeap = gHeaps[i];   /* first validated block adopts its heap */
				return nullptr;
			}
			return gHeaps[i];
		}
	}
	return nullptr;
}

int __cdecl ucrtAllocHook(int allocType, void* userData, size_t size,
                          int blockType, long requestNumber,
                          const unsigned char* filename, int lineNumber) {
	(void)size; (void)blockType; (void)requestNumber; (void)filename; (void)lineNumber;
	__try {
		/* Instrumented window: while a caller (e.g. O2rArchive::Close around
		 * zip_close) has gHeapHookFreeTrace set, log every free's pointer and
		 * header fields — the last entry before a crash names the victim. */
		if (gHeapHookFreeTrace != 0 && userData != nullptr &&
		    (allocType == 3 || allocType == 2)) {
			ShadowCrtHeader* h = ((ShadowCrtHeader*)userData) - 1;
			probeLog("    zipwin free user=%p hdr=%p use=%d size=%llu req=%ld "
			         "next=%p prev=%p\n",
			         userData, (const void*)h, (int)h->block_use,
			         (unsigned long long)h->data_size, (long)h->request_number,
			         (const void*)h->next, (const void*)h->prev);
		}
		if (userData != nullptr) {
			ShadowCrtHeader* h = ((ShadowCrtHeader*)userData) - 1;
			if (allocType == 1) {              // _HOOK_ALLOC (userData is always null)
			} else if (allocType == 3 || allocType == 2) {  // FREE / REALLOC(old ptr)
				const char* why = nullptr;
				__try {
					why = checkHeader(h);
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					why = "check faulted (header or link target unmapped)";
				}
				if (why != nullptr) {
					describeVictim(h, why, "pre-free");
					probeLog("    (free vetoed to keep the process alive)\n");
					return 0;
				}
				/* Cross-heap free check — SSB64_HEAP_XCHECK only. HeapValidate
				 * is O(heap segment) and runs once per candidate heap here; on
				 * every ucrt free that is a quadratic stall that keeps the run
				 * from ever reaching the corruption window. The theory is
				 * already ruled out (mirrored hooks saw zero foreign frees),
				 * so the expensive path is opt-in. */
				static const int sXCheck =
				    std::getenv("SSB64_HEAP_XCHECK") != nullptr ? 1 : 0;
				if (sXCheck) {
					if (HANDLE owner = foreignHeapOf(h)) {
						describeVictim(h, "FOREIGN-HEAP FREE", "xheap-free");
						probeLog("    block lives in heap %p, not ucrt heap %p — vetoed\n",
						         owner, gUcrtHeap);
						return 0;
					}
				}
				/* ucrt never calls the hook with the allocated pointer, so the
				 * ring/autopsy anchor must come from the freed block's still-
				 * live neighbors (they stay linked after h is unlinked). */
				ShadowCrtHeader* live = h->next ? h->next : h->prev;
				if (live != nullptr) {
					gLastSeen = live;
					if (gRing != nullptr) {
						gRing[gRingPos % kRingSize] = (void*)(live + 1);
						gRingPos++;
					}
				}
				if (gRing != nullptr) {
					ringRemove(userData);
				}
				/* Diagnostic noise, capped: prev==null on free is routine
				 * (ucrt prepends allocs; a LIFO free frees the head). */
				if (h->prev == nullptr) {
					static volatile long headFrees = 0;
					long n = InterlockedIncrement(&headFrees);
					if (n <= 4) {
						describeVictim(h, "claimed-head free", "head-free");
						probeLog("    (claimed-head free #%ld, next=%p)\n",
						         n, (const void*)h->next);
					}
				}
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return 1;
}

/* Mirror hook on the EXE's own /MTd CRT. The ucrt hook cannot see frees
 * routed through the statically-linked CRT — yet a ucrt-based pointer freed
 * by exe code is exactly the kind of silent cross-heap corruption that later
 * surfaces inside ucrtbased's unlink. Same validation; heap membership is
 * judged against whichever process heap validates the block, so the first
 * exe-side free adopts the exe heap. */
HANDLE gExeHeap = nullptr;

int __cdecl exeAllocHook(int allocType, void* userData, size_t size,
                         int blockType, long requestNumber,
                         const unsigned char* filename, int lineNumber) {
	(void)size; (void)blockType; (void)requestNumber; (void)filename; (void)lineNumber;
	__try {
		if (userData != nullptr && (allocType == 3 || allocType == 2)) {
			ShadowCrtHeader* h = ((ShadowCrtHeader*)userData) - 1;
			/* Feed the walk anchor + ring from the exe list too: in the
			 * fully-static build ucrtbased is never loaded, so this hook is
			 * the only source. Neighbors of a freed node stay valid and are
			 * known members of the same list. */
			ShadowCrtHeader* live = nullptr;
			__try {
				live = h->next ? h->next : h->prev;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			if (live != nullptr) {
				gLastSeen = live;
				if (gRing != nullptr) {
					gRing[gRingPos % kRingSize] = (void*)(live + 1);
					gRingPos++;
				}
			}
			if (gRing != nullptr) {
				ringRemove(userData);
			}
			/* Same link/gap validation as the ucrt hook: the teardown crash
			 * may well be the EXE's own debug list, which the zipwin trace
			 * never logs because it lives behind our statically-linked CRT. */
			const char* why = nullptr;
			__try {
				why = checkHeader(h);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				why = "check faulted (header or link target unmapped)";
			}
			if (why != nullptr) {
				describeVictim(h, why, "exe-pre-free");
				probeLog("    (exe free vetoed to keep the process alive)\n");
				return 0;
			}
			/* THE braiding event (GWATCH BRAID proves both CRTs share
			 * GetProcessHeap, so heap handles can't separate ownership —
			 * LIST membership can): an exe-side free of a block whose
			 * prev==nullptr claims "list head". If it isn't the exe's
			 * head, it's a ucrt node — letting it unlink would write
			 * h->next into the exe's anchor (observed braid) and leave
			 * ucrt's anchor dangling at the corpse. Veto it and walk the
			 * EBP chain (frames are preserved under /Od) to name the
			 * offending free() callsite. */
			if (h->prev == nullptr && h != exeListHead()) {
				describeVictim(h, "FOREIGN HEAD FREE (not exe list head)", "xhead-free");
				uintptr_t* fp = (uintptr_t*)_AddressOfReturnAddress();
				__try {
					fp = (uintptr_t*)*(fp - 1);  /* our frame's saved EBP sits below retaddr */
					for (int f = 0; f < 12 && fp != nullptr; ++f) {
						if (!addrPlausible(fp) || ((uintptr_t)fp & 7) != 0) break;
						uintptr_t ret = fp[1];
						if (ret < 0x10000 || ret >= 0x0000800000000000ull) break;
						char rm[160];
						gwatchDescribeAddr(ret, rm, sizeof(rm));
						probeLog("    [frame%d] %s\n", f, rm);
						uintptr_t* nxt = (uintptr_t*)fp[0];
						if (nxt <= fp) break;
						fp = nxt;
					}
				} __except (EXCEPTION_EXECUTE_HANDLER) {
				}
				probeLog("    (braiding free vetoed)\n");
				return 0;
			}
			/* SSB64_HEAP_XCHECK only — HeapValidate per free is the dominant
			 * runtime cost of this instrument (mirrors the ucrt hook). */
			static const int sXCheckE =
			    std::getenv("SSB64_HEAP_XCHECK") != nullptr ? 1 : 0;
			if (sXCheckE) {
				DWORD nh = GetProcessHeaps(gHeapCount ? gHeapCount : 32, gHeaps);
				if (nh > 32) nh = 32;
				gHeapCount = (int)nh;
				HANDLE owner = nullptr;
				for (int i = 0; i < gHeapCount; ++i) {
					if (HeapValidate(gHeaps[i], 0, h)) { owner = gHeaps[i]; break; }
				}
				if (gExeHeap == nullptr && owner != nullptr) {
					gExeHeap = owner;
					probeLog("heap-hook: exe heap adopted = %p\n", (const void*)owner);
				} else if (owner != nullptr && owner != gExeHeap) {
					describeVictim(h, "CROSS-CRT FREE (ucrt block via exe CRT)", "xheap-exe");
					probeLog("    block lives in heap %p (ucrt=%p), not exe heap %p — vetoed\n",
					         (const void*)owner, (const void*)gUcrtHeap, (const void*)gExeHeap);
					return 0;
				}
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return 1;
}

void installGlobalsWatch();

void installUcrtAllocHook() {
	if (std::getenv("SSB64_HEAP_HOOK") == nullptr) {
		return;
	}
	_CrtSetAllocHook(exeAllocHook);
	probeLog("heap-hook: installed into exe /MTd CRT\n");
	HMODULE ucrt = GetModuleHandleW(L"ucrtbased.dll");
	if (ucrt == nullptr) {
		probeLog("heap-hook: ucrtbased.dll not loaded\n");
		return;
	}
	using SetAllocHookFn = void* (*)(void*);
	auto setHook = reinterpret_cast<SetAllocHookFn>(GetProcAddress(ucrt, "_CrtSetAllocHook"));
	if (setHook == nullptr) {
		probeLog("heap-hook: _CrtSetAllocHook export not found\n");
		return;
	}
	gRing = (void**)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, kRingSize * sizeof(void*));
	/* Tracing every free is opt-in (SSB64_HEAP_TRACE): a synchronous
	 * WriteFile per ucrt free turns a 20 s init into several minutes and
	 * shifts the corruption window. With it unset the zip_close window flag
	 * still works for callers that arm it explicitly. */
	if (std::getenv("SSB64_HEAP_TRACE") != nullptr) {
		gHeapHookFreeTrace = 1;
	}
	DWORD nh = GetProcessHeaps(32, gHeaps);
	gHeapCount = (int)(nh > 32 ? 32 : nh);
	setHook(reinterpret_cast<void*>(&ucrtAllocHook));
	installGlobalsWatch();
	/* Seed the walk anchor directly: the hook cannot observe allocations
	 * (ucrt calls it with userData=nullptr on _HOOK_ALLOC), and waiting for
	 * a free to expose a live neighbor delays the first real autopsy. A
	 * sentinel block allocated through ucrtbased's own _malloc_dbg sits in
	 * the list forever — never freed — giving the walk a fixed start node. */
	using MallocDbgFn = void* (*)(size_t, int, const char*, int);
	if (auto mdbg = reinterpret_cast<MallocDbgFn>(
			GetProcAddress(ucrt, "_malloc_dbg"))) {
		if (void* p = mdbg(8, 1, "heap_check_boot.cpp", 0)) {
			gLastSeen = ((ShadowCrtHeader*)p) - 1;
		}
	}
	probeLog("heap-hook: installed into ucrtbased (%d process heaps snapshotted)\n",
	         gHeapCount);
	/* Shared-heap test: the BRAID hits show exe's alloc path assigning a
	 * live ucrt-list node to exe's first_block — only possible if both
	 * CRTs HeapAlloc from the SAME NT heap (which also makes the
	 * HeapValidate cross-check blind). Log both __acrt_heap handles. */
	HANDLE exeH = reinterpret_cast<HANDLE>(_get_heap_handle());
	using GetHeapFn = uintptr_t (*)();
	HANDLE ucrtH = nullptr;
	if (auto gh = reinterpret_cast<GetHeapFn>(
			GetProcAddress(ucrt, "_get_heap_handle"))) {
		ucrtH = (HANDLE)gh();
	}
	probeLog("heap-hook: exe __acrt_heap=%p  ucrt __acrt_heap=%p  process=%p%s\n",
	         (const void*)exeH, (const void*)ucrtH,
	         (const void*)GetProcessHeap(),
	         (exeH == ucrtH) ? "  <<< SHARED — HeapValidate blind, lists can braid at NT level" : "");
}

/* ---------------- global-anchor watchpoints ----------------
 * The braided-list corruption needs ONE specific write: exe's
 * __acrt_first_block (or last_block) — or ucrtbased's — being set to a
 * node of the other CRT's list. Four globals, four DR registers: a write
 * watchpoint fires STATUS_SINGLE_STEP AFTER the store and the handler logs
 * the writing instruction's RIP — the braider itself, not a victim. */
uintptr_t gGWAddr[4] = {0, 0, 0, 0};
const char* gGWName[4] = {nullptr, nullptr, nullptr, nullptr};

/* Module table captured once at arm time — gwatchDescribeAddr runs inside
 * the vectored handler where GetModuleFileName/VirtualQuery are unsafe:
 * they take the loader lock, and armAllThreads may have a DLL-loading
 * thread suspended at that instant → self-inflicted deadlock. */
struct GWMod { uintptr_t base, end; char name[64]; };
static GWMod gModTable[64];
static volatile long gModCount = 0;

void snapshotModuleTable() {
	long n = 0;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
	if (snap == INVALID_HANDLE_VALUE) return;
	MODULEENTRY32W me;
	me.dwSize = sizeof(me);
	if (Module32FirstW(snap, &me)) {
		do {
			if (n >= 64) break;
			gModTable[n].base = (uintptr_t)me.modBaseAddr;
			gModTable[n].end = (uintptr_t)me.modBaseAddr + me.modBaseSize;
			int i = 0;
			for (; i < 62 && me.szModule[i]; ++i) {
				wchar_t c = me.szModule[i];
				gModTable[n].name[i] = (c < 128) ? (char)c : '?';
			}
			gModTable[n].name[i] = 0;
			++n;
		} while (Module32NextW(snap, &me));
	}
	CloseHandle(snap);
	gModCount = n;
}

void gwatchDescribeAddr(uintptr_t a, char* out, size_t outSize) {
	for (long i = 0; i < gModCount; ++i) {
		if (a >= gModTable[i].base && a < gModTable[i].end) {
			snprintf(out, outSize, "%s+0x%llx", gModTable[i].name,
			         (unsigned long long)(a - gModTable[i].base));
			return;
		}
	}
	snprintf(out, outSize, "0x%llx", (unsigned long long)a);
}

LONG CALLBACK gwatchVectored(PEXCEPTION_POINTERS ep) {
	if (ep->ExceptionRecord->ExceptionCode != STATUS_SINGLE_STEP) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	const unsigned fired = (unsigned)(ep->ContextRecord->Dr6 & 0xF);
	/* Own only slots 0-1 — bits 2-3 belong to the victim watchpoint's
	 * handler; swallowing them here would hide the corruptor's RIP. */
	if (fired == 0 || (fired & ~0x3u) != 0) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	int idx = 0;
	for (int i = 0; i < 4; ++i) {
		if (fired & (1u << i)) idx = i;
	}
	/* Braid detection beats RIP dedup: a legit CRT writer CAN still braid —
	 * e.g. a head-pop writing first_block = <node from another list>
	 * because the list was already tangled. Read the value just stored and
	 * inspect the new head's prev: a legit prepend publishes prev==nullptr;
	 * a head-pop leaves a TRANSIENT non-null prev pointing at the old head,
	 * but that node's next still points back — only a prev whose own next
	 * doesn't return to the head is genuinely foreign. */
	uintptr_t written = 0;
	__try {
		written = *(volatile uintptr_t*)gGWAddr[idx];
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	if (written != 0) {
		ShadowCrtHeader* wprev = nullptr;
		void* wprevNext = nullptr;
		__try {
			wprev = ((const ShadowCrtHeader*)written)->prev;
			if (wprev != nullptr) {
				wprevNext = wprev->next;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
		/* Legit head-pop leaves a transient non-null prev pointing at the
		 * old head — but that node's next still points back at the new
		 * head (unlink doesn't clear the removed node's links). A foreign
		 * node's next points at ITS own list's successor → mismatch =
		 * braid, topology-independent. */
		if (wprev != nullptr && wprevNext != (void*)written) {
			char rm[160];
			gwatchDescribeAddr(
			    (uintptr_t)ep->ExceptionRecord->ExceptionAddress, rm, sizeof(rm));
			probeLog("*** GWATCH BRAID %s <= %p (head.prev=%p whose "
			         "next=%p != head) by rip=%s tid=%lu ***\n",
			         gGWName[idx] ? gGWName[idx] : "?",
			         (void*)written, (void*)wprev, wprevNext, rm,
			         GetCurrentThreadId());
		}
	}
	/* Dedup by writer RIP: ucrt's own insert/unlink write first_block on
	 * EVERY alloc/free — logging each hit floods the log and costs a WriteFile
	 * per malloc. Only a previously-unseen writer instruction is the signal
	 * a wild store carries. */
	static uintptr_t sSeenRip[64] = {0};
	static volatile long sSeenCount = 0;
	const uintptr_t rip = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
	for (int i = 0; i < 64; ++i) {
		if (sSeenRip[i] == rip) {
			return EXCEPTION_CONTINUE_EXECUTION;
		}
	}
	if (sSeenCount < 64) {
		sSeenRip[InterlockedIncrement(&sSeenCount) - 1] = rip;
	}
	char ripMod[160];
	gwatchDescribeAddr(rip, ripMod, sizeof(ripMod));
	probeLog("*** GWATCH HIT %s (%p) written by rip=%s tid=%lu ***\n",
	         gGWName[idx] ? gGWName[idx] : "?", (void*)gGWAddr[idx],
	         ripMod, GetCurrentThreadId());
	__try {
		uintptr_t sp = ep->ContextRecord->Rsp;
		for (int i = 0; i < 24; ++i) {
			uintptr_t v = *(uintptr_t*)(sp + i * 8);
			if (v < 0x10000 || v >= 0x0000800000000000ull) continue;
			MEMORY_BASIC_INFORMATION mbi;
			if (VirtualQuery((LPCVOID)v, &mbi, sizeof(mbi)) == sizeof(mbi) &&
			    mbi.Type == MEM_IMAGE) {
				char f[160];
				gwatchDescribeAddr(v, f, sizeof(f));
				probeLog("    [stk+%02x] %s\n", i * 8, f);
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return EXCEPTION_CONTINUE_EXECUTION;
}

int gwatchArmThread(DWORD tid) {
	const bool self = (tid == GetCurrentThreadId());
	HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
	                           THREAD_SET_CONTEXT,
	                       FALSE, tid);
	if (th == nullptr) return 0;
	/* Suspending our own thread deadlocks the process (the earlier
	 * silent freezes right after "slots armed"). Self DR programming
	 * needs no suspend. */
	if (!self && SuspendThread(th) == (DWORD)-1) { CloseHandle(th); return 0; }
	CONTEXT ctx;
	ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
	if (GetThreadContext(th, &ctx)) {
		/* Slots 0-1 only: DR2/DR3 belong to the heap_watchpoint.cpp victim
		 * watch. Clear just L0/L1 enables (bits 0-3) and RW0/LEN0+RW1/LEN1
		 * (bits 16-23) so the other system survives re-arms. */
		DWORD64 dr7 = ctx.Dr7 & ~0x00FF000Full;
		for (int i = 0; i < 2; ++i) {
			if (gGWAddr[i] == 0) continue;
			(&ctx.Dr0)[i] = (DWORD64)gGWAddr[i];
			dr7 |= (1ull << (i * 2)) | (1ull << (16 + i * 4)) | (3ull << (18 + i * 4));
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

/* Same starvation fix as heap_watchpoint's armed-tid memoization: the
 * probe calls this once per loaded file, and re-suspending every thread
 * every time stalls the resource workers. The gwatch anchor addresses
 * never change after install, so a thread armed once stays correct. */
static DWORD gGArmedTids[256];
static long  gGArmedCount = 0;

void gwatchArmAll() {
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snap == INVALID_HANDLE_VALUE) return;
	THREADENTRY32 te; te.dwSize = sizeof(te);
	const DWORD self = GetCurrentProcessId();
	const DWORD myTid = GetCurrentThreadId();
	if (Thread32First(snap, &te)) {
		do {
			if (te.th32OwnerProcessID != self || te.th32ThreadID == myTid) {
				continue;
			}
			bool known = false;
			for (long k = 0; k < gGArmedCount; ++k) {
				if (gGArmedTids[k] == te.th32ThreadID) {
					known = true;
					break;
				}
			}
			if (known) {
				continue;
			}
			if (gwatchArmThread(te.th32ThreadID) && gGArmedCount < 256) {
				gGArmedTids[gGArmedCount++] = te.th32ThreadID;
			}
		} while (Thread32Next(snap, &te));
	}
	CloseHandle(snap);
	for (long k = 0; k < gGArmedCount; ++k) {
		if (gGArmedTids[k] == myTid) {
			return;
		}
	}
	if (gwatchArmThread(myTid) && gGArmedCount < 256) {
		gGArmedTids[gGArmedCount++] = myTid;
	}
}

/* Locate a CRT module's __acrt_first_block without a PDB: it is a static
 * pointer in the module's .data whose value is always the live list
 * head — a valid debug header with prev==nullptr. Scan the module's
 * committed image for any qword holding such a pointer, then verify the
 * target by following a few `next` links (a real head chains through
 * valid headers; a coincidental pointer to freed data does not). Works
 * for the exe's own /MTd image too. */
uintptr_t findListHeadGlobal(HMODULE ucrt) {
	MODULEINFO mi;
	if (!GetModuleInformation(GetCurrentProcess(), ucrt, &mi, sizeof(mi))) {
		return 0;
	}
	const uintptr_t lo = (uintptr_t)mi.lpBaseOfDll;
	const uintptr_t hi = lo + mi.SizeOfImage;
	for (uintptr_t q = lo; q + 8 <= hi; q += 8) {
		uintptr_t v = 0;
		__try {
			v = *(volatile uintptr_t*)q;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			continue;
		}
		if (!addrPlausible((void*)v)) {
			continue;
		}
		const ShadowCrtHeader* h = (const ShadowCrtHeader*)v;
		bool looks = false;
		__try {
			looks = (h->block_use == 1 || h->block_use == 2 || h->block_use == 4) &&
			        h->prev == nullptr &&
			        (h->next == nullptr || addrPlausible(h->next)) &&
			        h->gap[0] == 0xFD && h->gap[1] == 0xFD;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
		if (!looks) {
			continue;
		}
		/* Verify: follow next links; every hop must be a valid header.
		 * Three good hops from a prev==null node makes "this static points
		 * at the real list head" near-certain. */
		int hops = 0;
		const ShadowCrtHeader* cur = h;
		__try {
			while (cur->next != nullptr && hops < 3) {
				const ShadowCrtHeader* nx = cur->next;
				if (!addrPlausible(nx)) break;
				if (nx->block_use != 1 && nx->block_use != 2 &&
				    nx->block_use != 4) break;
				if (nx->gap[0] != 0xFD || nx->gap[1] != 0xFD) break;
				cur = nx;
				++hops;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
		if (hops >= 3) {
			return q;
		}
	}
	return 0;
}

void installGlobalsWatch() {
	if (std::getenv("SSB64_HEAP_GWATCH") == nullptr) {
		return;
	}
	/* Resolve the four anchor globals: exe's own /MTd copy plus the shared
	 * ucrtbased's. dbghelp is already linked for the crash handler. */
	HANDLE proc = GetCurrentProcess();
	SymSetOptions(SYMOPT_EXACT_SYMBOLS | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
	char exePath[MAX_PATH], watchDir[MAX_PATH];
	GetModuleFileNameA(nullptr, exePath, sizeof(exePath));
	strcpy_s(watchDir, exePath);
	char* slash = strrchr(watchDir, '\\');
	if (slash) slash[1] = 0;
	strcat_s(watchDir, ";C:\\Users\\alex0\\PycharmProjects\\smash-classic\\out");
	if (!SymInitialize(proc, watchDir, FALSE)) {
		probeLog("heap-gwatch: SymInitialize failed (%lu)\n", GetLastError());
		return;
	}
	const char* names[2] = {"__acrt_first_block", "__acrt_last_block"};
	HMODULE ucrt = GetModuleHandleW(L"ucrtbased.dll");

	/* The corrupted list is the EXE's /MTd debug list — the alloc-hook
	 * vetoes prove it: exe-pre-free reports show exe nodes whose `next`
	 * points at dead/foreign blocks, including a ucrt per-thread-data node
	 * that ended up AS exe's __acrt_first_block. Watch the exe anchors in
	 * DR0/DR1; DR2/DR3 stay reserved for the heap_watchpoint.cpp victims. */
	int slot = 0;
	uintptr_t exeMod = (uintptr_t)GetModuleHandleW(nullptr);
	DWORD64 exeBase = SymLoadModuleEx(proc, nullptr, exePath, nullptr,
	                                  exeMod, 0, nullptr, 0);
	if (exeBase != 0) {
		/* Only __acrt_first_block — slot 1 goes to ucrtbased's anchor, the
		 * list where every observed break actually landed. last_block
		 * changes only on tail frees; lowest signal of the four. */
		SYMBOL_INFO_PACKAGE sip;
		sip.si.SizeOfStruct = sizeof(SYMBOL_INFO);
		sip.si.MaxNameLen = 255;
		if (SymFromName(proc, names[0], &sip.si)) {
			gGWAddr[slot] = (uintptr_t)sip.si.Address;
			gGWName[slot] = "exe.__acrt_first_block";
			probeLog("heap-gwatch: slot %d -> %s @ %p\n", slot,
			         gGWName[slot], (void*)sip.si.Address);
			++slot;
		}
	}
	if (ucrt != nullptr && slot < 4) {
		uintptr_t uhead = findListHeadGlobal(ucrt);
		if (uhead != 0) {
			gGWAddr[slot] = uhead;
			gGWName[slot] = "ucrt.__acrt_first_block";
			probeLog("heap-gwatch: slot %d -> %s @ %p\n", slot,
			         gGWName[slot], (void*)uhead);
			++slot;
		} else {
			probeLog("heap-gwatch: ucrt head global not found by .data scan\n");
		}
	}
	if (slot == 0) {
		probeLog("heap-gwatch: no anchor symbols resolved\n");
		return;
	}
	AddVectoredExceptionHandler(1, &gwatchVectored);
	/* Do NOT arm here: this runs from a static ctor — before PortInit
	 * registers the game's vectored handler — so every anchor write until
	 * the first probe would reach that handler and flood ssb64.log.
	 * Arming is deferred to reregisterGwatchHandler (first probe), which
	 * first re-prepends our handler past the game's and only then programs
	 * the debug registers. */
	probeLog("heap-gwatch: %d anchor globals resolved (arming deferred to first probe)\n", slot);
}

/* The game's portWindowsVectoredHandler registers during PortInit — after
 * this file's static-init handler registration — and sits ahead of ours.
 * Re-registering from the first probe (always post-PortInit) puts ours at
 * the front so single-step hits get consumed before the game logs them;
 * the actual DR arming happens here too, once the order is safe. */
void reregisterGwatchHandler() {
	static volatile long sLogged = 0;
	static void* sHandle = nullptr;
	/* Not once-only: the game's handler (or anything else) may register a
	 * new front-of-chain entry AFTER our first probe — e.g. the engine
	 * boots late — so we re-prepend on every probe call to stay ahead. */
	void* h = AddVectoredExceptionHandler(1, &gwatchVectored);
	if (sHandle != nullptr) {
		RemoveVectoredExceptionHandler(sHandle);
	}
	sHandle = h;
	if (gModCount == 0) {
		snapshotModuleTable();
	}
	gwatchArmAll();
	if (InterlockedCompareExchange(&sLogged, 1, 0) == 0) {
		probeLog("heap-gwatch: anchors armed post-PortInit (handler re-prepended each probe)\n");
	}
}
} // namespace

namespace { struct HookBoot { HookBoot() { installUcrtAllocHook(); } } gHookBoot; }

/*
 * portSharedCrtHeapValidate(tag) — point-in-time autopsy of ucrtbased's
 * debug block list. With SSB64_HEAP_HOOK the walk starts from the most
 * recently seen live header and checks every node's forward/back links
 * and no-mans-land gaps; a corrupt node is reported in place with its
 * allocation identity. Without the hook it falls back to _CrtCheckMemory
 * (whose own reporter may crash on a stomped _file_name — still useful as
 * a corruption signal, since the fault happens only when damage exists).
 */
extern "C" __declspec(dllexport) int portSharedCrtHeapValidate(const char* tag)
{
	if (std::getenv("SSB64_HEAP_CHECK") == nullptr &&
	    std::getenv("SSB64_HEAP_HOOK") == nullptr) {
		return 0;
	}
	probeLog("heap-probe %s: ENTER\n", tag);

	if (gLastSeen != nullptr) {
		/* Front-of-chain fix for single-step hits: the game's vectored
		 * handler registers at PortInit (after our static init) and would
		 * otherwise see every watchpoint trap. Re-registering ours now —
		 * post-PortInit — puts us ahead so traps get consumed quietly. */
		if (gGWAddr[0] != 0) {
			reregisterGwatchHandler();
			/* Anchor watchpoints arm once at install; threads spawned later
			 * (the resource pool is born during Context init — exactly the
			 * threads whose teardown frees braid the lists) never got DR0/
			 * DR1 programmed. Re-arming per probe closes the hole. */
			gwatchArmAll();
		}
		/* Frame probes walk the live list while resource workers mutate
		 * it — a recycled node's "next" is user data, so reads fault; the
		 * vectored handler logs + dumps every first-chance AV (~50MB
		 * each). The corruption window is archive init/teardown anyway,
		 * so the walk is opt-in via SSB64_HEAP_FRAMES (the re-arm above
		 * still runs — late-spawned threads need their DRs). */
		if (std::strncmp(tag, "frame-", 6) == 0 &&
		    std::getenv("SSB64_HEAP_FRAMES") == nullptr) {
			return 0;
		}
		/* Once the hardware watchpoint is armed the DETECTION role moves to
		 * the data breakpoints — but the walk must keep running: the
		 * corruption victim is allocated AFTER the first probe (request
		 * numbers ~811-860, inside the archive-open window), so later
		 * walks are the only way to discover and arm it. Armed walks run
		 * in pointer-chase mode — no per-node checkHeader — which is cheap
		 * enough to keep discovering candidates every probe. */
		const bool watchArmed = (portHeapWatchArmed() != 0);
		if (watchArmed) {
			portHeapWatchCheck(nullptr, 0, 0, nullptr);  // re-arm new threads
			/* Saturated slots are already parked on the victim cluster —
			 * discovery is done and detection lives on the DRs. The
			 * O(list) walk below (150k+ nodes, one probe per loaded file)
			 * was starving the resource workers enough to stall frames,
			 * so it's skipped until a slot retires and needs rediscovery. */
			if (portHeapWatchSaturated() != 0) {
				probeLog("heap-probe %s: DONE heap_ok=1 (watch saturated)\n", tag);
				return 0;
			}
		}
		// Bounded bidirectional walk. Prefer the TRUE list head
		// (*exe.__acrt_first_block) — a node cached from an earlier free
		// can itself be freed and recycled, and walking from it then
		// "finds" user data misread as headers (false autopsy faults).
		// Falling back to gLastSeen only keeps coverage before the anchor
		// global resolves.
		const ShadowCrtHeader* anchorHead = exeListHead();
		const ShadowCrtHeader* start = anchorHead;
		if (start == nullptr) {
			start = gLastSeen;
		}
		long walked = 0;
		int result = 0;
		bool faulted = false;
		// dir: +1 follow next (toward oldest), -1 follow prev (toward head).
		// Starting from the true anchor head, +1 already covers the whole
		// list; the -1 pass would walk `prev` off a node captured ~100k+
		// derefs ago — long freed and recycled — producing false faults.
		// It only earns its keep when the start node is mid-list (the
		// gLastSeen fallback before the anchor resolves).
		const int dirEnd = (anchorHead != nullptr) ? 0 : -1;
		for (int dir = 1; dir >= dirEnd && result == 0 && !faulted; dir -= 2) {
			const ShadowCrtHeader* cur = start;
			const ShadowCrtHeader* lastGood = nullptr;
			while (cur != nullptr && walked < 4000000) {
				ShadowCrtHeader* nxt = nullptr;
				const char* why = nullptr;
				__try {
					if (!watchArmed) {
						why = checkHeader(cur);
					}
					nxt = (dir > 0) ? cur->next : cur->prev;
					/* The corruption victim is a 16-byte file-less early
					 * malloc; its request number drifts a little between
					 * builds. Hand every node to the watchpoint — it arms
					 * DR2-DR3 on the candidates closest to the ~835 cluster. */
					portHeapWatchCheck((void*)cur, cur->request_number,
					                   (unsigned long long)cur->data_size,
					                   (const void*)cur->file_name);
					/* ucrt prepends allocs: list order is descending request
					 * numbers. In armed chase-mode the walk exists only to
					 * refresh candidates — stop once past the window. */
					if (watchArmed) {
						if (dir > 0 && cur->request_number < 400) {
							break;
						}
						if (dir < 0 && cur->request_number >= 2000) {
							break;
						}
					}
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					faulted = true;
					if (lastGood != nullptr) {
						probeLog("heap-probe %s: FAULT walking %s from last-good node:\n",
						         tag, dir > 0 ? "next" : "prev");
						describeVictim(lastGood, "walk-fault edge", tag);
						__try {
							probeLog("    last-good->%s = %p (unmapped target)\n",
							         dir > 0 ? "next" : "prev",
							         (const void*)(dir > 0 ? lastGood->next : lastGood->prev));
						} __except (EXCEPTION_EXECUTE_HANDLER) {
						}
					} else {
						DWORD w = 0;
						const char msg[] = "heap-probe: walk faulted at start node\n";
						WriteFile(gProbeLog, msg, (DWORD)sizeof(msg) - 1, &w, nullptr);
					}
					result = 2;
					break;
				}
				if (why != nullptr) {
					describeVictim(cur, why, tag);
					result = 1;
					break;
				}
				lastGood = cur;
				cur = nxt;
				++walked;
			}
		}
		// Candidates collected this walk arm DR slots on the lowest reqs.
		portHeapWatchFlush();
		probeLog("heap-probe %s: DONE autopsy result=%d walked=%ld\n", tag, result, walked);
		return result;
	}

	HMODULE ucrt = GetModuleHandleW(L"ucrtbased.dll");
	int heapOk = -1;
	if (ucrt != nullptr) {
		using CheckMemFn = int (*)();
		if (auto checkMem = reinterpret_cast<CheckMemFn>(
				GetProcAddress(ucrt, "_CrtCheckMemory"))) {
			heapOk = checkMem();
		}
	}
	probeLog("heap-probe %s: DONE heap_ok=%d\n", tag, heapOk);
	return heapOk == 1 ? 0 : 1;
}
#endif
