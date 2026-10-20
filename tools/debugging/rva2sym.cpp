// rva2sym.cpp - resolve image RVAs to symbol names + source lines using DIA SDK.
// Usage: rva2sym <pdb-or-module> <rva-hex> [...]
// No COM registration required (uses NoRegCoCreate).
#include <windows.h>
#include <dia2.h>
#include <diacreate.h>
#include <cstdio>
#include <cstdlib>

static void print_line_info(IDiaSession* sess, DWORD rva) {
    IDiaEnumLineNumbers* en = nullptr;
    if (FAILED(sess->findLinesByRVA(rva, 1, &en)) || !en) return;
    IDiaLineNumber* ln = nullptr; ULONG got = 0;
    if (SUCCEEDED(en->Next(1, &ln, &got)) && got && ln) {
        DWORD lineno = 0; IDiaSourceFile* sf = nullptr; BSTR fn = nullptr;
        ln->get_lineNumber(&lineno);
        if (SUCCEEDED(ln->get_sourceFile(&sf)) && sf) { sf->get_fileName(&fn); sf->Release(); }
        wprintf(L"  %s:%lu", fn ? fn : L"", lineno);
        if (fn) SysFreeString(fn);
        ln->Release();
    }
    en->Release();
}

static void resolve_rva(IDiaSession* sess, DWORD rva) {
    IDiaSymbol* sym = nullptr;
    if (FAILED(sess->findSymbolByRVA(rva, SymTagNull, &sym)) || !sym) {
        wprintf(L"+0x%-8x ?\n", rva);
        return;
    }
    BSTR name = nullptr; DWORD symRva = 0;
    sym->get_name(&name); sym->get_relativeVirtualAddress(&symRva);
    wprintf(L"+0x%-8x %s+0x%x", rva, name ? name : L"?", rva - symRva);
    if (name) SysFreeString(name);
    sym->Release();
    // line info
    print_line_info(sess, rva);
    wprintf(L"\n");
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { wprintf(L"usage: rva2sym <pdb> <rva-hex>...\n"); return 2; }
    HRESULT hr = CoInitialize(nullptr);
    IDiaDataSource* src = nullptr;
    hr = NoRegCoCreate(L"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/DIA SDK/bin/amd64/msdia140.dll", CLSID_DiaSource, IID_IDiaDataSource, (void**)&src);
    if (FAILED(hr)) { wprintf(L"NoRegCoCreate failed %#x\n", hr); return 1; }
    hr = src->loadDataFromPdb(argv[1]);
    if (FAILED(hr)) { wprintf(L"loadDataFromPdb failed %#x\n", hr); return 1; }
    IDiaSession* sess = nullptr;
    if (FAILED(src->openSession(&sess))) { wprintf(L"openSession failed\n"); return 1; }
    for (int i = 2; i < argc; i++) {
        DWORD rva = (DWORD)wcstoul(argv[i], nullptr, 16);
        resolve_rva(sess, rva);
    }
    return 0;
}
