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
#include "heap_check_internal.h"
#if defined(_MSC_VER) && defined(_DEBUG) && defined(SSB64_TEST_HOOKS)

namespace {

/* Env flag semantics shared by every SSB64_* toggle: set, non-empty,
 * and not literally "0". */
bool heapEnvOn(const char* name) {
	const char* v = std::getenv(name);
	return !(v == nullptr || v[0] == '\0' || (v[0] == '0' && v[1] == '\0'));
}

// The exe is /MTd (own static debug CRT); vendored DLLs (zip.dll,
// SDL2d...) are /MDd — their blocks live in ucrtbased's debug heap,
// which our _CrtSetDbgFlag never touches. The shutdown corruption is
// detected on THAT side, so enable check-always there too via its
// exported _CrtSetDbgFlag. Its _CrtDbgReport stream is DLL-local as
// well: without redirecting it, damage reports go to the debugger.
void ucrtSetupDebugHeap(bool always, FILE* rf) {
	HMODULE ucrt = GetModuleHandleW(L"ucrtbased.dll");
	if (ucrt == nullptr) {
		return;
	}
	using SetDbgFlagFn = int (*)(int);
	using SetReportFileFn = void* (*)(int, void*);
	using SetReportModeFn = int (*)(int, int);
	if (always) {
		if (auto setFlag = reinterpret_cast<SetDbgFlagFn>(
				GetProcAddress(ucrt, "_CrtSetDbgFlag"))) {
			setFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_CHECK_ALWAYS_DF);
		}
	}
	if (rf == nullptr) {
		return;
	}
	auto setMode = reinterpret_cast<SetReportModeFn>(
		GetProcAddress(ucrt, "_CrtSetReportMode"));
	auto setFile = reinterpret_cast<SetReportFileFn>(
		GetProcAddress(ucrt, "_CrtSetReportFile"));
	if (setMode == nullptr || setFile == nullptr) {
		return;
	}
	void* rh = reinterpret_cast<void*>(_get_osfhandle(_fileno(rf)));
	const int types[3] = {_CRT_ASSERT, _CRT_ERROR, _CRT_WARN};
	for (int i = 0; i < 3; ++i) {
		setMode(types[i], 0x1);
		setFile(types[i], rh);
	}
}

struct HeapCheckBoot {
	HeapCheckBoot() {
		const bool always = heapEnvOn("SSB64_HEAP_CHECK_ALWAYS");
		const bool cheap = heapEnvOn("SSB64_HEAP_CHECK");
		const bool hook = heapEnvOn("SSB64_HEAP_HOOK");
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
		ucrtSetupDebugHeap(always, rf);
	}
};
HeapCheckBoot gHeapCheckBoot;

/* The walk hit an unmapped node: report the edge that led there — the
 * last-good node's link identifies the corrupt predecessor (or the walk
 * start itself when there was none). */
void autopsyWalkFault(const char* tag, const ShadowCrtHeader* lastGood, int dir) {
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
		return;
	}
	DWORD w = 0;
	const char msg[] = "heap-probe: walk faulted at start node\n";
	WriteFile(gProbeLog, msg, (DWORD)sizeof(msg) - 1, &w, nullptr);
}

/* One direction of the bounded list walk. Returns 0 clean, 1 corrupt
 * node found, 2 faulted on an unmapped node. `walked` accumulates the
 * global step budget across both directions. */
int autopsyWalkDir(const char* tag, const ShadowCrtHeader* start,
                   int dir, bool watchArmed, long* walked) {
	const ShadowCrtHeader* cur = start;
	const ShadowCrtHeader* lastGood = nullptr;
	while (cur != nullptr && *walked < 4000000) {
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
			autopsyWalkFault(tag, lastGood, dir);
			return 2;
		}
		if (why != nullptr) {
			describeVictim(cur, why, tag);
			return 1;
		}
		lastGood = cur;
		cur = nxt;
		++*walked;
	}
	return 0;
}

/* Bounded bidirectional walk. Prefer the TRUE list head
 * (*exe.__acrt_first_block) — a node cached from an earlier free can
 * itself be freed and recycled, and walking from it then "finds" user
 * data misread as headers (false autopsy faults). Falling back to
 * gLastSeen only keeps coverage before the anchor global resolves.
 *
 * dir: +1 follow next (toward oldest), -1 follow prev (toward head).
 * Starting from the true anchor head, +1 already covers the whole
 * list; the -1 pass would walk `prev` off a node captured ~100k+
 * derefs ago — long freed and recycled — producing false faults.
 * It only earns its keep when the start node is mid-list (the
 * gLastSeen fallback before the anchor resolves). */
int autopsyWalk(const char* tag, bool watchArmed) {
	const ShadowCrtHeader* anchorHead = exeListHead();
	const ShadowCrtHeader* start = (anchorHead != nullptr) ? anchorHead : gLastSeen;
	long walked = 0;
	const int dirEnd = (anchorHead != nullptr) ? 0 : -1;
	int result = 0;
	for (int dir = 1; dir >= dirEnd && result == 0; dir -= 2) {
		result = autopsyWalkDir(tag, start, dir, watchArmed, &walked);
	}
	// Candidates collected this walk arm DR slots on the lowest reqs.
	portHeapWatchFlush();
	probeLog("heap-probe %s: DONE autopsy result=%d walked=%ld\n", tag, result, walked);
	return result;
}

} // namespace

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
		return autopsyWalk(tag, watchArmed);
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
