/*
 * Locale.h — string-table i18n for the port UI.
 *
 * English source literals are the lookup keys (gettext-style): wrap a
 * string in TR("...") and it renders translated when a non-English
 * language is selected, verbatim otherwise. Translation tables live in
 * lang/<code>.json next to the binary (a flat {"English": "translated"}
 * map), so community translations drop in without a rebuild — a missing
 * file or key falls back to the English literal, never to a crash or a
 * blank string.
 *
 * In-game N64 text is asset-driven and out of scope: this covers the port
 * surface only (menus, toasts, prompts, dialogs).
 */

#pragma once

namespace PortI18n {

/* English-literal-keyed translation. Returns the loaded translation for
 * `key`, or `key` itself when none exists. The returned pointer is stable
 * for the lifetime of the active table (call it again after a language
 * switch rather than caching).
 *
 * Also hot-swaps: the gSettings.Language CVar is polled here, so changing
 * the language mid-session takes effect on the next draw without a
 * restart. */
const char* T(const char* key);

/* The code currently in effect ("en" when English or a failed load). */
const char* ActiveLanguage();

} // namespace PortI18n

#define TR(lit) ::PortI18n::T(lit)
