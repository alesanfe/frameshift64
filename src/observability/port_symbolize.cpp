/* Address -> symbol name for diagnostics (rollback SyncTest memory diffs).
 * Windows resolves through DbgHelp against BattleShip.pdb; elsewhere it
 * returns NULL and callers print the raw section offset instead. */
#include <cstdint>

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#include <mutex>

#include "port_log.h"

extern "C" const char *port_symbolize(const void *addr, uint32_t *offset)
{
	static std::mutex sMutex;
	static bool sInitialized = false;
	static bool sAvailable = false;
	static char sName[256];

	std::lock_guard<std::mutex> lock(sMutex);

	if (!sInitialized) {
		sInitialized = true;
		SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
		sAvailable = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
		/* libultraship's crash handler may already own the process's DbgHelp
		 * session; a second SymInitialize then fails with
		 * ERROR_INVALID_PARAMETER, and the existing session is usable. */
		if (!sAvailable && GetLastError() == ERROR_INVALID_PARAMETER) {
			sAvailable = true;
			SymRefreshModuleList(GetCurrentProcess());
		}
		if (!sAvailable) {
			port_log("SSB64: port_symbolize SymInitialize failed err=%lu\n", GetLastError());
		}
	}
	if (offset != nullptr) {
		*offset = 0;
	}
	if (!sAvailable) {
		return nullptr;
	}
	alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + sizeof(sName)];
	SYMBOL_INFO *info = reinterpret_cast<SYMBOL_INFO *>(buffer);
	info->SizeOfStruct = sizeof(SYMBOL_INFO);
	info->MaxNameLen = sizeof(sName) - 1;

	DWORD64 displacement = 0;
	if (!SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(addr), &displacement, info)) {
		static int sFailLogs = 0;
		if (sFailLogs++ < 3) {
			port_log("SSB64: port_symbolize SymFromAddr(%p) failed err=%lu\n", addr, GetLastError());
		}
		return nullptr;
	}
	strncpy_s(sName, info->Name, _TRUNCATE);
	if (offset != nullptr) {
		*offset = static_cast<uint32_t>(displacement);
	}
	return sName;
}
#else
extern "C" const char *port_symbolize(const void *, uint32_t *offset)
{
	if (offset != nullptr) {
		*offset = 0;
	}
	return nullptr;
}
#endif
