/* heap_check_internal.h — shared declarations for the heap-check
 * translation units. Test-hook builds only; every consumer wraps its
 * contents in the same guard so shipped binaries contain none of this.
 *
 * Split layout:
 *   heap_check_report.cpp  probe log + shadow-CRT describe/check + exe anchor
 *   heap_check_hooks.cpp   ucrt/exe alloc hooks, live-block ring, HookBoot
 *   heap_check_gwatch.cpp  globals watchpoint (DRx + vectored handler)
 *   heap_check_boot.cpp    env-gated boot installer + portSharedCrtHeapValidate
 */
#pragma once

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
extern "C" __declspec(dllexport) void portHeapWatchCheck(void* header,
    long requestNumber, unsigned long long dataSize, const void* fileName);
extern "C" __declspec(dllexport) void portHeapWatchFlush();
extern "C" __declspec(dllexport) int portHeapWatchArmed();
extern "C" __declspec(dllexport) int portHeapWatchSaturated();

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

/* Module-table snapshot shared by the gwatch/watchpoint describe paths —
 * captured once at arm time because describe* runs inside the vectored
 * handler where GetModuleFileName/VirtualQuery are unsafe (loader lock
 * while armAllThreads may hold a DLL-loading thread suspended). When
 * ucrtBase is non-null the ucrtbased module bounds are also captured. */
struct ModEntry { uintptr_t base, end; char name[64]; };
constexpr int kModTableMax = 64;

inline long snapshotModules(ModEntry* table, uintptr_t* ucrtBase = nullptr,
                            uintptr_t* ucrtEnd = nullptr) {
	long n = 0;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
	if (snap == INVALID_HANDLE_VALUE) return 0;
	MODULEENTRY32W me;
	me.dwSize = sizeof(me);
	if (Module32FirstW(snap, &me)) {
		do {
			if (n >= kModTableMax) break;
			table[n].base = (uintptr_t)me.modBaseAddr;
			table[n].end = (uintptr_t)me.modBaseAddr + me.modBaseSize;
			int i = 0;
			for (; i < 62 && me.szModule[i]; ++i) {
				wchar_t c = me.szModule[i];
				table[n].name[i] = (c < 128) ? (char)c : '?';
			}
			table[n].name[i] = 0;
			if (ucrtBase != nullptr && wcsstr(me.szModule, L"ucrtbased") != nullptr) {
				*ucrtBase = table[n].base;
				*ucrtEnd = table[n].end;
			}
			++n;
		} while (Module32NextW(snap, &me));
	}
	CloseHandle(snap);
	return n;
}

/* Lock-free "module+0xoff" lookup over the cached table. */
inline void describeModuleAddr(const ModEntry* table, long count, uintptr_t a,
                               char* out, size_t outSize) {
	for (long i = 0; i < count; ++i) {
		if (a >= table[i].base && a < table[i].end) {
			snprintf(out, outSize, "%s+0x%llx", table[i].name,
			         (unsigned long long)(a - table[i].base));
			return;
		}
	}
	snprintf(out, outSize, "0x%llx", (unsigned long long)a);
}

/* ---- heap_check_report.cpp ---- */
extern HANDLE gProbeLog;
void probeLog(const char* fmt, ...);
bool addrLooksMapped(const void* p);
void describePtr(const void* p, char* out, size_t outSize);
void describeVictim(const ShadowCrtHeader* h, const char* why, const char* tag);
bool addrPlausible(const void* p);
const char* checkHeader(const ShadowCrtHeader* h);
const ShadowCrtHeader* exeListHead();

/* ---- heap_check_hooks.cpp ---- */
extern ShadowCrtHeader* gLastSeen;

/* ---- heap_check_gwatch.cpp ---- */
extern uintptr_t gGWAddr[4];
extern const char* gGWName[4];
void gwatchDescribeAddr(uintptr_t a, char* out, size_t outSize);
void gwatchArmAll();
uintptr_t findListHeadGlobal(HMODULE m);
void installGlobalsWatch();
void reregisterGwatchHandler();

#endif
