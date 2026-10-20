/* Crash diagnostics — extracted from port.cpp.
 *
 * Windows-only: vectored first-chance logger, unhandled-exception filter
 * with minidump + symbolized frames, signal() hooks, CRT invalid-parameter
 * and std::terminate handlers, MSVC C++-throw decoder, and the
 * lastcrash.txt raw-write channel that works even with the CRT half-dead.
 *
 * port_install_crash_handlers() wires it all up from main(); on
 * non-Windows builds it is an empty stub so the call site stays
 * unconditional. */
#include "port_internal.h"

#include "crash_bundle.h"
#include "port_log.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <typeinfo>

#if defined(_WIN32) && !defined(BATTLESHIP_UWP)
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <crtdbg.h>
#include <signal.h>
#include <exception>
#include <ctime>
#pragma comment(lib, "dbghelp.lib")

static void portCrtInvalidParameter(const wchar_t* expr, const wchar_t* func,
                                    const wchar_t* file, unsigned line, uintptr_t)
{
	port_log("\n*** CRT INVALID PARAMETER ***\n");
	if (expr) port_log("    expr: %ls\n", expr);
	if (func) port_log("    func: %ls\n", func);
	if (file) port_log("    file: %ls:%u\n", file, line);
	port_log_close();
}

static volatile LONG sMinidumpWritten = 0;
static void portResolveSymbol(void* addr, char* out, size_t cap);
static void portWriteMinidump(EXCEPTION_POINTERS* info, const char* prefix);

static void portTerminateHandler()
{
	port_log("\n*** std::terminate called (uncaught C++ exception) ***\n");
	port_log_close();
	std::abort();
}

static void portSignalHandler(int sig)
{
	const char *name = "?";
	switch (sig) {
		case SIGABRT: name = "SIGABRT"; break;
		case SIGFPE:  name = "SIGFPE";  break;
		case SIGILL:  name = "SIGILL";  break;
		case SIGINT:  name = "SIGINT";  break;
		case SIGSEGV: name = "SIGSEGV"; break;
		case SIGTERM: name = "SIGTERM"; break;
	}
	port_log("\n*** SIGNAL %s (%d) raised - tid=%lu ***\n",
	         name, sig, (unsigned long)GetCurrentThreadId());

	/* signal() handlers get no EXCEPTION_POINTERS and abort() does not run the
	 * SEH crash filter, so without this the abort path leaves no stack in the
	 * log (and writes no dump). Capture + symbolize it here so a re-crash
	 * self-reports the faulting frames. An uncaught C++ exception routes here
	 * too (terminate -> abort -> SIGABRT), so the throw site is still on the
	 * stack below. */
	void* frames[48];
	WORD nframes = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
	for (WORD i = 0; i < nframes; i++) {
		char fsym[768] = {0};
		portResolveSymbol(frames[i], fsym, sizeof(fsym));
		port_log("    [%2u] %p %s\n", i, frames[i], fsym[0] ? fsym : "(no sym)");
	}

	port_diag_write_crash_marker(name, 0);
	if (InterlockedCompareExchange(&sMinidumpWritten, 1, 0) == 0) {
		portWriteMinidump(nullptr, "signal");
	}
	port_log_close();
}

static void portResolveSymbol(void* addr, char* out, size_t cap)
{
	out[0] = '\0';
	HMODULE mod = nullptr;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       (LPCSTR)addr, &mod) && mod) {
		char modPath[MAX_PATH] = {0};
		GetModuleFileNameA(mod, modPath, sizeof(modPath));
		const char *modName = std::strrchr(modPath, '\\');
		modName = modName ? modName + 1 : modPath;

		// Try SymFromAddr for the function name. The mod loader's
		// SymbolResolver::Init runs SymInitialize during port startup,
		// so by the time a crash filter fires, symbols are loaded.
		// Fall back to module+RVA if the lookup fails.
		constexpr size_t kBufSize = sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR);
		char symBuf[kBufSize];
		SYMBOL_INFO* si = reinterpret_cast<SYMBOL_INFO*>(symBuf);
		std::memset(si, 0, kBufSize);
		si->SizeOfStruct = sizeof(SYMBOL_INFO);
		si->MaxNameLen   = MAX_SYM_NAME;
		DWORD64 disp = 0;
		if (SymFromAddr(GetCurrentProcess(), (DWORD64)addr, &disp, si) && si->Name[0] != '\0') {
			std::snprintf(out, cap, "%s+0x%llx %s+0x%llx",
				modName,
				(unsigned long long)((uintptr_t)addr - (uintptr_t)mod),
				si->Name, (unsigned long long)disp);
		} else {
			std::snprintf(out, cap, "%s+0x%llx", modName,
				(unsigned long long)((uintptr_t)addr - (uintptr_t)mod));
		}
	}
}

static void portWriteMinidump(EXCEPTION_POINTERS* info, const char* prefix)
{
	char dumpPath[MAX_PATH];
	std::time_t t = std::time(nullptr);
	std::snprintf(dumpPath, sizeof(dumpPath), "crash_%lld.dmp", (long long)t);
	HANDLE hf = CreateFileA(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
	                        FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hf == INVALID_HANDLE_VALUE) {
		port_log("    %s minidump create failed (err=%lu)\n", prefix, GetLastError());
		return;
	}

	MINIDUMP_EXCEPTION_INFORMATION mei = {};
	mei.ThreadId = GetCurrentThreadId();
	mei.ExceptionPointers = info;
	mei.ClientPointers = FALSE;
	MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hf,
	                  (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithThreadInfo |
	                                  MiniDumpWithIndirectlyReferencedMemory),
	                  info ? &mei : nullptr, nullptr, nullptr);
	CloseHandle(hf);
	port_log("    %s minidump = %s\n", prefix, dumpPath);
}

// Decode an MSVC C++ throw (exception code 0xE06D7363) into a type-info name
// and, if the thrown object derives from std::exception, its what() string.
//
// MSVC ABI on x64: ExceptionInformation[] holds:
//   [0] magic = 0x19930520 (or 0x19930521/22)
//   [1] pointer to thrown object
//   [2] pointer to ThrowInfo (image-relative on x64)
//   [3] HMODULE base for the image-relative offsets in ThrowInfo
//
// ThrowInfo -> CatchableTypeArray -> CatchableType[0] -> std::type_info* + offsets.
//
// We only decode the *primary* catchable type (CatchableType[0]). That's
// the most-derived type the thrower used, which matches what's actually
// thrown (not a base of it). It's enough to identify the exception in 99%
// of cases — std::filesystem::filesystem_error, std::bad_alloc,
// std::system_error, etc.
struct PortMsvcThrowInfo {
	uint32_t attributes;
	int32_t  pmfnUnwind;            // image-relative
	int32_t  pForwardCompat;        // image-relative
	int32_t  pCatchableTypeArray;   // image-relative
};
struct PortMsvcCatchableTypeArray {
	uint32_t nCatchableTypes;
	int32_t  arrayOfCatchableTypes[1]; // image-relative array
};
struct PortMsvcCatchableType {
	uint32_t  properties;
	int32_t   pType;                // image-relative; std::type_info*
	struct { int32_t mdisp, pdisp, vdisp; } thisDisplacement;
	int32_t   sizeOrOffset;
	int32_t   copyFunction;
};

struct MsvcThrowImage {
	uintptr_t base;
	uintptr_t end;
	bool contains(const void* p) const {
		const uintptr_t a = (uintptr_t)p;
		return a >= base && a < end;
	}
	const void* rva(uint32_t off) const { return (const void*)(base + off); }
};

// End of the throwing module image — every metadata pointer must live
// inside it. `end==0` when the module info cannot be read.
static MsvcThrowImage msvcImageOf(uintptr_t imgBase) {
	MsvcThrowImage img = {imgBase, 0};
	MODULEINFO mi = {0};
	if (GetModuleInformation(GetCurrentProcess(), (HMODULE)imgBase, &mi, sizeof(mi))) {
		img.end = (uintptr_t)mi.lpBaseOfDll + mi.SizeOfImage;
	}
	return img;
}

// Attempt std::exception::what() only when offset 0 looks safe per
// CatchableType properties. Multi-inherit / virtual-base types would need
// adjustments; keep it conservative and report the type name regardless.
// Catch any access violation — vectored handlers must not throw.
static const char* msvcTryWhat(void* thrownObj, const PortMsvcCatchableType* ct) {
	if (!thrownObj || !ct || (ct->properties & 0x4) != 0) {
		return nullptr;
	}
	const char* what = nullptr;
	__try {
		const std::exception* ex = (const std::exception*)thrownObj;
		what = ex->what();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		what = nullptr;
	}
	return what;
}

// Walk ThrowInfo → CatchableTypeArray → CatchableType[0] → type_info and
// report the decoded throw. Every deref is image-bounds-checked; the whole
// call runs under the caller's __try.
static void msvcLogCatchable(const MsvcThrowImage& img,
                             const PortMsvcThrowInfo* throwInfo,
                             void* thrownObj) {
	auto* cta = (const PortMsvcCatchableTypeArray*)
	            img.rva((uint32_t)throwInfo->pCatchableTypeArray);
	if (!img.contains(cta) || cta->nCatchableTypes == 0) {
		port_log("    C++ throw: thrown=%p (empty/out-of-image CatchableTypeArray)\n", thrownObj);
		return;
	}

	auto* ct = (const PortMsvcCatchableType*)
	           img.rva((uint32_t)cta->arrayOfCatchableTypes[0]);
	const std::type_info* ti = img.contains(ct)
	                           ? (const std::type_info*)img.rva((uint32_t)ct->pType)
	                           : nullptr;
	const char* tname = (ti && img.contains(ti)) ? ti->name() : "(null)";
	const char* what = msvcTryWhat(thrownObj, ct);
	port_log("    C++ throw: type=\"%s\" thrown=%p%s%s\n",
	         tname, thrownObj,
	         what ? " what=" : "",
	         what ? what     : "");
}

static void portLogMsvcCxxThrow(EXCEPTION_POINTERS* info)
{
	/* Decoder dereferences throw-record metadata supplied by the runtime.
	 * On fibers / mid-teardown / fundamental thrown types those pointers can
	 * be stale — this diagnostic path must never fault while reporting, so
	 * every deref is image-bounds-checked and the whole body sits in SEH. */
	__try {
	const EXCEPTION_RECORD* er = info->ExceptionRecord;
	if (er->NumberParameters < 3) {
		port_log("    C++ throw: (no parameters)\n");
		return;
	}

	uintptr_t imgBase = er->NumberParameters >= 4
	                  ? (uintptr_t)er->ExceptionInformation[3]
	                  : (uintptr_t)GetModuleHandleA(nullptr);
	void* thrownObj = (void*)er->ExceptionInformation[1];
	auto* throwInfo = (const PortMsvcThrowInfo*)er->ExceptionInformation[2];
	if (!throwInfo || !imgBase) {
		port_log("    C++ throw: thrown=%p (no ThrowInfo/imgBase)\n", thrownObj);
		return;
	}

	const MsvcThrowImage img = msvcImageOf(imgBase);
	if (img.end == 0 || !img.contains(throwInfo)) {
		port_log("    C++ throw: thrown=%p (ThrowInfo %p outside image)\n",
		         thrownObj, (const void*)throwInfo);
		return;
	}
	msvcLogCatchable(img, throwInfo, thrownObj);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		port_log("    C++ throw: (metadata decode faulted)\n");
	}
}


// Noisy / non-actionable exception codes that get no log entry.
static bool portIsSuppressedCode(DWORD code) {
	return code == EXCEPTION_BREAKPOINT ||
	       code == DBG_PRINTEXCEPTION_C || code == DBG_PRINTEXCEPTION_WIDE_C ||
	       code == 0x406D1388;
}

// First-chance logging of C++ throws (see issue #58 note at call site).
static void portHandleCxxThrow(EXCEPTION_POINTERS* info) {
	static volatile LONG sCxxThrowsLogged = 0;
	// Cap to first 8 to avoid log spam if a hot path throws.
	if (InterlockedIncrement(&sCxxThrowsLogged) <= 8) {
		port_log("\n*** C++ THROW (first-chance) tid=%lu ***\n",
		         GetCurrentThreadId());
		portLogMsvcCxxThrow(info);
	}
}

static LONG CALLBACK portWindowsVectoredHandler(EXCEPTION_POINTERS* info)
{
	DWORD code = info->ExceptionRecord->ExceptionCode;
	// Suppress noisy / non-actionable codes after capturing C++ throw
	// details. Issue #58: a heap corruption fault was reported in the
	// log but the *triggering* C++ exception was suppressed silently —
	// the unwind from that throw is what hit the heap-corruption
	// detector, so the throw site is the actual root cause we need.
	// Decode and log it, then keep returning EXCEPTION_CONTINUE_SEARCH
	// so SEH still gets a chance to catch it normally.
	if (code == 0xE06D7363) {
		portHandleCxxThrow(info);
		return EXCEPTION_CONTINUE_SEARCH;
	}
	if (portIsSuppressedCode(code)) {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	void* addr = info->ExceptionRecord->ExceptionAddress;
	char sym[768] = {0};
	portResolveSymbol(addr, sym, sizeof(sym));
	port_log("\n*** VECTORED EXCEPTION (first-chance) tid=%lu code=0x%08X addr=%p %s ***\n",
	         GetCurrentThreadId(), (unsigned)code, addr, sym[0] ? sym : "(no sym)");
	if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
		const char* op = info->ExceptionRecord->ExceptionInformation[0] == 0 ? "read" :
		                 info->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : "execute";
		port_log("    AV: %s at %p\n", op, (void*)info->ExceptionRecord->ExceptionInformation[1]);
	}

	void* frames[32];
	WORD nframes = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
	for (WORD i = 0; i < nframes; i++) {
		char fsym[768] = {0};
		portResolveSymbol(frames[i], fsym, sizeof(fsym));
		port_log("    [%2u] %p %s\n", i, frames[i], fsym[0] ? fsym : "(no sym)");
	}

	// First-chance faults are often benign (SEH may still handle them);
	// they get their own counter so a swallowed fault cannot starve the
	// dump for the actually-fatal unhandled exception.
	static volatile LONG sFirstChanceMinidumpWritten = 0;
	if (InterlockedCompareExchange(&sFirstChanceMinidumpWritten, 1, 0) == 0) {
		portWriteMinidump(info, "first-chance");
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI portWindowsCrashFilter(EXCEPTION_POINTERS* info)
{
	const EXCEPTION_RECORD* er = info->ExceptionRecord;
	void* addr = er->ExceptionAddress;

	HMODULE mod = nullptr;
	char modname[MAX_PATH] = {0};
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       (LPCSTR)addr, &mod)) {
		GetModuleFileNameA(mod, modname, sizeof(modname));
	}

	uintptr_t base = (uintptr_t)mod;
	uintptr_t rva  = (uintptr_t)addr - base;

	port_log("\n*** UNHANDLED EXCEPTION ***\n");
	port_log("  code     = 0x%08X\n", (unsigned)er->ExceptionCode);
	port_log("  address  = %p\n", addr);
	port_log("  module   = %s (base=%p, rva=0x%llx)\n",
	         modname[0] ? modname : "(unknown)", (void*)base, (unsigned long long)rva);
	if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
		const char* op = er->ExceptionInformation[0] == 0 ? "read" :
		                 er->ExceptionInformation[0] == 1 ? "write" :
		                 er->ExceptionInformation[0] == 8 ? "execute" : "?";
		port_log("  AV: %s at %p\n", op, (void*)er->ExceptionInformation[1]);
	}
#if defined(_M_X64) || defined(_M_AMD64)
	const CONTEXT* c = info->ContextRecord;
	port_log("  RIP=%p RSP=%p RBP=%p\n", (void*)c->Rip, (void*)c->Rsp, (void*)c->Rbp);
	port_log("  RAX=%016llx RBX=%016llx RCX=%016llx RDX=%016llx\n",
	         (unsigned long long)c->Rax, (unsigned long long)c->Rbx,
	         (unsigned long long)c->Rcx, (unsigned long long)c->Rdx);
	port_log("  RSI=%016llx RDI=%016llx R8 =%016llx R9 =%016llx\n",
	         (unsigned long long)c->Rsi, (unsigned long long)c->Rdi,
	         (unsigned long long)c->R8,  (unsigned long long)c->R9);
#endif

	port_diag_write_crash_marker("unhandled", (uintptr_t)addr);
	if (InterlockedCompareExchange(&sMinidumpWritten, 1, 0) == 0) {
		portWriteMinidump(info, "unhandled");
	}
	port_log("*** END EXCEPTION ***\n");
	port_log_close();
	return EXCEPTION_EXECUTE_HANDLER;
}
#endif

#if defined(_WIN32) && !defined(BATTLESHIP_UWP)
void port_install_crash_handlers() {
	SetUnhandledExceptionFilter(portWindowsCrashFilter);
	AddVectoredExceptionHandler(1, portWindowsVectoredHandler);
	std::atexit([]() {
		port_log("\n*** atexit reached — process is shutting down voluntarily ***\n");
		port_log_close();
	});
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
	_CrtSetReportMode(_CRT_ERROR,  _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ERROR,  _CRTDBG_FILE_STDERR);
	_CrtSetReportMode(_CRT_WARN,   _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_WARN,   _CRTDBG_FILE_STDERR);
	_set_invalid_parameter_handler(portCrtInvalidParameter);
	std::set_terminate(portTerminateHandler);
	signal(SIGABRT, portSignalHandler);
	signal(SIGFPE,  portSignalHandler);
	signal(SIGILL,  portSignalHandler);
	signal(SIGSEGV, portSignalHandler);
	signal(SIGTERM, portSignalHandler);
}
#else
void port_install_crash_handlers() {}
#endif
