/* Resolve BattleShip.exe RVAs to file:line via dbghelp + the local PDB.
 *   symrva.exe <exe_dir> <rva> [rva...]
 */
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: symrva <exe_dir> <rva...>\n"); return 2; }
    char exe[MAX_PATH], pdbdir[MAX_PATH];
    snprintf(exe, MAX_PATH, "%s\\BattleShip.exe", argv[1]);
    snprintf(pdbdir, MAX_PATH, "%s", argv[1]);

    HANDLE h = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_AUTO_PUBLICS);
    char search[MAX_PATH * 2];
    snprintf(search, sizeof(search), "%s;SRV*%s\\symstore", pdbdir, pdbdir);
    if (!SymInitialize(h, search, FALSE)) { fprintf(stderr, "SymInit failed %lu\n", GetLastError()); return 1; }

    /* load at a fixed base; RVAs are image-relative so any base works */
    DWORD64 base = SymLoadModule64(h, NULL, exe, NULL, 0x140000000ull, 0);
    if (!base) { fprintf(stderr, "SymLoadModule64 failed %lu\n", GetLastError()); return 1; }
    fprintf(stderr, "loaded at 0x%I64x\n", base);
    IMAGEHLP_MODULE64 mi; mi.SizeOfStruct = sizeof(mi);
    if (SymGetModuleInfo64(h, base, &mi))
        fprintf(stderr, "symtype=%d pdb=%s\n", mi.SymType, mi.LoadedPdbName);

    {   /* force eager symbol load + sanity check with a known symbol */
        char tb[sizeof(SYMBOL_INFO) + 512];
        SYMBOL_INFO* tsi = (SYMBOL_INFO*)tb;
        tsi->SizeOfStruct = sizeof(SYMBOL_INFO);
        tsi->MaxNameLen = 512;
        fprintf(stderr, "SymFromName(main): %s\n",
                SymFromName(h, "main", tsi) ? tsi->Name : "(none)");
        IMAGEHLP_MODULE64 mi2; mi2.SizeOfStruct = sizeof(mi2);
        if (SymGetModuleInfo64(h, base, &mi2))
            fprintf(stderr, "after touch: symtype=%d pdb=%s\n", mi2.SymType, mi2.LoadedPdbName);
    }

    char sbuf[sizeof(SYMBOL_INFO) + 512];
    SYMBOL_INFO* si = (SYMBOL_INFO*)sbuf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 512;
    IMAGEHLP_LINE64 line; DWORD disp;
    DWORD64 sdisp;

    for (int i = 2; i < argc; i++) {
        DWORD64 addr = base + strtoull(argv[i], NULL, 16);
        line.SizeOfStruct = sizeof(line);
        if (SymFromAddr(h, addr, &sdisp, si)) {
            printf("0x%s -> %s+0x%llx", argv[i], si->Name, addr - si->Address);
            if (SymGetLineFromAddr64(h, addr, &disp, &line))
                printf("  %s:%lu", line.FileName, line.LineNumber);
            printf("\n");
        } else {
            printf("0x%s -> <no symbol> (err %lu)\n", argv[i], GetLastError());
        }
    }
    SymCleanup(h);
    return 0;
}
