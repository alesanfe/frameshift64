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
extern "C" void portHeapWatchCheck(void* header, long requestNumber,
                                   unsigned long long dataSize, const void* fileName);
extern "C" void portHeapWatchFlush();
extern "C" int portHeapWatchArmed();
extern "C" int portHeapWatchSaturated();

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
