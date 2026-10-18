/*
 * heap_check_hooks.cpp — part of the optional CRT debug-heap verification for Debug
 * MSVC test builds (see heap_check_boot.cpp for the full design notes).
 */
#include "heap_check_internal.h"
#if defined(_MSC_VER) && defined(_DEBUG) && defined(SSB64_TEST_HOOKS)

ShadowCrtHeader* gLastSeen = nullptr;

namespace {
void**   gRing = nullptr;
enum { kRingSize = 8192 };
unsigned gRingPos = 0;

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
} // namespace

/* Window flag: while nonzero, ucrtAllocHook logs every free's pointer and
 * header fields. O2rArchive::Close sets it around zip_close so the last
 * logged free before a crash identifies the victim block. */
extern "C" __declspec(dllexport) volatile int gHeapHookFreeTrace = 0;


namespace {
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
} // namespace

namespace { struct HookBoot { HookBoot() { installUcrtAllocHook(); } } gHookBoot; }

#endif
