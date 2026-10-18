/* url_safety.h — shared URL vetting for the download backends.
 *
 * Mod-pack install accepts *user-pasted* URLs, so this is the last line
 * of defense at the sink (not the caller): restrict to http(s) and
 * reject control/whitespace/quote characters that could smuggle a
 * second URL or confuse a parser downstream. Written back when the
 * transport was a curl subprocess; kept with libcurl because a strict
 * scheme+charset allowlist is cheap defense regardless of transport. */
#pragma once

#include <string>

namespace ssb64 {

inline bool IsAllowedHttpUrl(const std::string& url) {
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
        return false;
    }
    for (char c : url) {
        if (c == '"' || c == '\'' || c == '\\' || c == '`' || c == '$' ||
            c <= 0x20 || c == 0x7F) {
            return false;
        }
    }
    return true;
}

} // namespace ssb64
