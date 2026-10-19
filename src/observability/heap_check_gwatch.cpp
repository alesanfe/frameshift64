/*
 * heap_check_gwatch.cpp — part of the optional CRT debug-heap verification for Debug
 * MSVC test builds (see heap_check_boot.cpp for the full design notes).
 */
#include "heap_check_internal.h"
#if defined(_MSC_VER) && defined(_DEBUG) && defined(SSB64_TEST_HOOKS)

/* Watch slots 0-1 own the globals watchpoint; 2-3 belong to the victim
 * watchpoint (heap_watchpoint.cpp). Shared with the probe walker. */
uintptr_t gGWAddr[4] = {0, 0, 0, 0};
const char* gGWName[4] = {nullptr, nullptr, nullptr, nullptr};

namespace {
/* Module table captured once at arm time — gwatchDescribeAddr runs inside
 * the vectored handler where GetModuleFileName/VirtualQuery are unsafe:
 * they take the loader lock, and armAllThreads may have a DLL-loading
 * thread suspended at that instant → self-inflicted deadlock. */
static ModEntry gModTable[kModTableMax];
static volatile long gModCount = 0;

void snapshotModuleTable() {
	gModCount = snapshotModules(gModTable);
}
} // namespace

void gwatchDescribeAddr(uintptr_t a, char* out, size_t outSize) {
	describeModuleAddr(gModTable, gModCount, a, out, outSize);
}

namespace {
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
	/* Slots 0-1 only: DR2/DR3 belong to the heap_watchpoint.cpp victim
	 * watch — the helper clears just our DR7 fields. */
	return armDebugWatchOnThread(tid, gGWAddr, 2, 0);
}

} // namespace

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
	/* Extra symbol search dirs (e.g. the build tree holding sibling .pdb
	 * files) come from the env — no machine-specific paths in source. */
	const char* extraDirs = std::getenv("SSB64_HEAP_GWATCH_SYMPATH");
	if (extraDirs != nullptr && extraDirs[0] != '\0') {
		if (std::strlen(watchDir) + 1 + std::strlen(extraDirs) < sizeof(watchDir)) {
			strcat_s(watchDir, ";");
			strcat_s(watchDir, extraDirs);
		}
	}
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

#endif
