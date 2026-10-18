#include "native_dialog.h"
#include "port_log.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shlwapi.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ssb64 {

namespace {

#ifndef _WIN32
// fork/execvp helper: run argv with no shell in the middle, so title and
// filter strings reach the dialog process verbatim — nothing to quote.
// Captures the first non-empty line of stdout (trimmed of trailing
// newline); child stderr goes to /dev/null. strip_ld_env drops
// LD_LIBRARY_PATH/LD_PRELOAD in the child so bundled AppImage libs can't
// poison the system dialog's loader path. Returns "" on any error,
// including nonzero exit (which is also the cancel path).
std::string RunAndCaptureFirstLine(const std::vector<std::string>& argv,
                                   bool strip_ld_env) {
    if (argv.empty() || argv[0].empty()) return {};
    int fds[2];
    if (pipe(fds) != 0) return {};
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return {};
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        if (strip_ld_env) {
            unsetenv("LD_LIBRARY_PATH");
            unsetenv("LD_PRELOAD");
        }
        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const std::string& a : argv) {
            cargv.push_back(const_cast<char*>(a.c_str()));
        }
        cargv.push_back(nullptr);
        execvp(cargv[0], cargv.data());
        _exit(127);
    }
    close(fds[1]);
    std::string line;
    if (FILE* f = fdopen(fds[0], "r")) {
        char buf[1024];
        while (std::fgets(buf, sizeof(buf), f) != nullptr) {
            line = buf;
            while (!line.empty() &&
                   (line.back() == '\n' || line.back() == '\r')) {
                line.pop_back();
            }
            if (!line.empty()) break;
        }
        fclose(f);
    } else {
        close(fds[0]);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return {};
    return line;
}

// Quote a string for embedding inside an AppleScript double-quoted literal.
// AppleScript only requires escaping " and \.
std::string AppleScriptQuote(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}
#endif // !_WIN32

} // namespace

std::string OpenFileDialog(const std::string& title,
                           const std::vector<std::string>& extensions) {
#ifdef __APPLE__
    // Build a `{"z64","n64","v64"}` extension list.
    std::string typesList;
    for (size_t i = 0; i < extensions.size(); ++i) {
        if (i) typesList += ", ";
        typesList += "\"" + extensions[i] + "\"";
    }

    std::string script = "choose file with prompt \"" +
                         AppleScriptQuote(title) + "\"";
    if (!typesList.empty()) {
        script += " of type {" + typesList + "}";
    }
    // POSIX path of (choose file ...) — gives a Unix path without "Macintosh HD:..."
    script = "POSIX path of (" + script + ")";

    return RunAndCaptureFirstLine({"osascript", "-e", script},
                                  /*strip_ld_env=*/false);

#elif defined(__linux__) || defined(__unix__)
    // Inside an AppImage, our AppRun sets LD_LIBRARY_PATH to the
    // bundled $APPDIR/usr/lib so the game's own .so deps resolve
    // against versions matching the build host. That same path
    // poisons system tools like zenity / kdialog which load the
    // distro's libssl/libcrypto/libcurl/etc. — the bundled
    // libcrypto.so.3 from the build host is older than the system
    // libssl.so.3 expects, so the dialog dies with `version
    // OPENSSL_3.X.0 not found` before it can show a window. The
    // strip_ld_env flag unsets those variables in the spawned child
    // so it only sees the system loader path.

    // zenity first.
    {
        std::vector<std::string> argv{
            "zenity", "--file-selection", "--title=" + title};
        if (!extensions.empty()) {
            std::string filter;
            for (size_t i = 0; i < extensions.size(); ++i) {
                filter += (i ? " " : "") + std::string("*.") + extensions[i];
            }
            argv.push_back("--file-filter=" + filter);
        }
        std::string r = RunAndCaptureFirstLine(argv, /*strip_ld_env=*/true);
        if (!r.empty()) return r;
    }
    // Fall back to kdialog if zenity isn't installed or returned empty.
    {
        // No shell expansion under execvp — resolve ~ ourselves.
        const char* home = std::getenv("HOME");
        std::vector<std::string> argv{
            "kdialog", "--getopenfilename", (home && *home) ? home : "~"};
        if (!extensions.empty()) {
            std::string filter;
            for (size_t i = 0; i < extensions.size(); ++i) {
                filter += (i ? " " : "") + std::string("*.") + extensions[i];
            }
            filter += "|" + title;
            argv.push_back(filter);
        }
        return RunAndCaptureFirstLine(argv, /*strip_ld_env=*/true);
    }

#elif defined(_WIN32)
    // Build a Windows commdlg filter:  "Title\0*.z64;*.n64;*.v64\0\0".
    std::wstring filterDesc;
    for (wchar_t c : title) filterDesc.push_back(c);
    std::wstring filterPat;
    for (size_t i = 0; i < extensions.size(); ++i) {
        if (i) filterPat += L";";
        filterPat += L"*.";
        for (char c : extensions[i]) filterPat.push_back((wchar_t)c);
    }
    if (filterPat.empty()) filterPat = L"*.*";

    std::wstring filter = filterDesc + L'\0' + filterPat + L'\0' + L'\0';

    wchar_t fileBuf[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    std::wstring titleW;
    for (char c : title) titleW.push_back((wchar_t)c);
    ofn.lpstrTitle = titleW.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) {
        return {};
    }
    // UTF-16 -> UTF-8 conversion for the returned path.
    int len = WideCharToMultiByte(CP_UTF8, 0, fileBuf, -1, nullptr, 0,
                                  nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, fileBuf, -1, out.data(), len,
                        nullptr, nullptr);
    return out;

#elif defined(__SWITCH__)
    // Switch has no native file dialog. The first-run wizard falls back
    // to manual path entry; the Browse button is simply a no-op.
    return {};
#else
    (void)title;
    (void)extensions;
    return {};
#endif
}

} // namespace ssb64
