// dump_proc.cpp - MiniDumpWriteDump on a running PID (full threads+memory).
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { wprintf(L"usage: dump_proc <pid> <out.dmp>\n"); return 2; }
    DWORD pid = (DWORD)wcstoul(argv[1], nullptr, 10);
    HANDLE hp = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hp) { wprintf(L"OpenProcess failed %lu\n", GetLastError()); return 1; }
    HANDLE hf = CreateFileW(argv[2], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (hf == INVALID_HANDLE_VALUE) { wprintf(L"CreateFile failed\n"); return 1; }
    BOOL ok = MiniDumpWriteDump(hp, pid, hf,
        (MINIDUMP_TYPE)(MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo),
        nullptr, nullptr, nullptr);
    wprintf(L"MiniDumpWriteDump: %s\n", ok ? L"OK" : L"FAILED");
    CloseHandle(hf); CloseHandle(hp);
    return ok ? 0 : 1;
}
