/*
 * Locale.cpp — implementation of the gettext-style string table.
 * See Locale.h for the contract.
 */

#include "core/i18n/Locale.h"
#include "app_paths.h"
#include "port_log.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <string>
#include <unordered_map>

namespace PortI18n {

/* gSettings.Language index → ISO code. The combo widget in
 * PortMenu.cpp (kMenuLanguageOptions) mirrors this table — keep them in
 * sync. "en" never loads a file; it is the identity table. */
static const char* const kLangCodes[] = {
    "en",
    "es",
    "fr",
    "de",
    "pt",
};

static std::unordered_map<std::string, std::string> sTable;
static int sLang = -1; /* forces a load on first T() */
static const char* sCode = "en";

static void LoadLanguage(int index) {
    sTable.clear();

    const size_t count = sizeof(kLangCodes) / sizeof(kLangCodes[0]);
    if (index < 0 || index >= (int)count) {
        index = 0;
    }
    sLang = index;
    sCode = kLangCodes[index];

    if (sCode[0] == 'e' && sCode[1] == 'n' && sCode[2] == '\0') {
        return; /* English is the identity table */
    }

    const std::string rel = std::string("lang/") + sCode + ".json";
    const std::string path = ssb64::LocateExistingFile(rel);
    if (path.empty()) {
        port_log("i18n: no %s found — UI stays in English\n", rel.c_str());
        return;
    }

    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        port_log("i18n: cannot open %s\n", path.c_str());
        return;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string text((size_t)size, '\0');
    const size_t read = fread(text.data(), 1, (size_t)size, f);
    fclose(f);
    text.resize(read);

    try {
        const nlohmann::json j = nlohmann::json::parse(text);
        if (!j.is_object()) {
            port_log("i18n: %s is not a flat key->string object\n", rel.c_str());
            return;
        }
        for (auto& [k, v] : j.items()) {
            if (v.is_string()) {
                sTable.emplace(k, v.get<std::string>());
            }
        }
        port_log("i18n: loaded %s (%zu strings)\n", rel.c_str(), sTable.size());
    } catch (const nlohmann::json::exception& e) {
        port_log("i18n: %s parse error: %s — UI stays in English\n", rel.c_str(), e.what());
    }
}

const char* T(const char* key) {
    if (key == nullptr) {
        return "";
    }

    const int lang = CVarGetInteger("gSettings.Language", 0);
    if (lang != sLang) {
        LoadLanguage(lang);
    }

    if (sTable.empty()) {
        return key;
    }
    const auto it = sTable.find(key);
#ifdef SSB64_TEST_HOOKS
    static int sDiagCount = 0;
    if (std::getenv("SSB64_I18N_DIAG") != nullptr && sDiagCount < 8) {
        port_log("i18n-diag: key='%s' lang=%d table=%zu %s\n",
                 key, lang, sTable.size(), (it != sTable.end()) ? "HIT" : "MISS");
        sDiagCount++;
    }
#endif
    return (it != sTable.end()) ? it->second.c_str() : key;
}

const char* ActiveLanguage() {
    if (sLang < 0) {
        (void)T(""); /* resolve the CVar once */
    }
    return sCode;
}

} // namespace PortI18n
