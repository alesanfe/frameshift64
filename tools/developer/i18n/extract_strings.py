#!/usr/bin/env python3
"""Extract translatable UI strings into a JSON catalog for assets/lang/.

Sources of truth (all English literals are the lookup keys — gettext-style):

  * TR("...") call sites            — free-form strings already wrapped
  * AddWidget(path, "Name", ...)    — widget labels, translated at render
  * .Tooltip("...")                 — widget tooltips, translated at render
  * ComboMap({ n, "Label" }) pairs  — combobox option text
  * AddSidebarEntry/AddMenuEntry    — navigation labels

Usage:
    python tools/developer/i18n/extract_strings.py            # print stats
    python tools/developer/i18n/extract_strings.py --write    # write catalog
    python tools/developer/i18n/extract_strings.py --sync es  # add missing
        keys (key->key placeholders) to assets/lang/es.json

Strings that are pure ImGui IDs ("##x"), format-only ("%s", "{}"), or
identifier-like internals are skipped.
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCAN_DIRS = [ROOT / "src"]
LANG_DIR = ROOT / "assets" / "lang"

STR = r'"((?:[^"\\]|\\.)*)"'
PATTERNS = [
    re.compile(r"\bTR\(\s*" + STR),
    re.compile(r"\bAddWidget\([^,]+,\s*" + STR),
    re.compile(r"[.>]Tooltip\(\s*" + STR),
    re.compile(r"[.>]PlaceholderText\(\s*" + STR),
    re.compile(r"[.>]DisabledAction\(\s*" + STR),
    re.compile(r"[.>]disabledTooltip\s*=\s*" + STR),
    re.compile(r"\bAddMenuEntry\(\s*" + STR),
    # AddSidebarEntry(section, displayedName, cols) — the visible label is 2nd
    re.compile(r"\bAddSidebarEntry\(\s*" + STR + r"\s*,\s*" + STR),
    re.compile(r"\bsidebarName\s*=\s*" + STR),
    re.compile(r"\bToast(?:Success|Error|Info)?\(\s*" + STR),
    # label/hint locals feeding ImGui::Text*(var) draws
    re.compile(r"\bhints\s*=\s*padNav\s*\?\s*" + STR),
    re.compile(r":\s*" + STR + r"\s*;"),
]
COMBO_PAIR = re.compile(r"\{\s*[-\w.:+*() ]+,\s*" + STR + r"\s*\}")
# disabledMap = { {DISABLE_FOR_X, {check, "reason", ..., "action"}} } —
# the reason/action literals are TR()'d lazily at render, so they never
# appear inside a TR(...) call for the plain patterns to see.
DISABLED_MAP = re.compile(r"\bdisabledMap\s*=\s*\{(.*?)\n[ \t]*\};", re.DOTALL)
STR_RE = re.compile(STR)
# which capture group holds the literal per pattern (AddSidebarEntry → 2)
GROUP_IDX = [1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1]

ID_ONLY = re.compile(r"^#")
NO_LETTERS = re.compile(r"[A-Za-z]")
SPLIT_LITERAL = re.compile(r'"\s+"')  # adjacent literals inside a group
# Non-UI literals the loose patterns pick up: paths (a/b), filenames
# (x.txt), snake_case identifiers (mods_panel), lowercase log tokens
# ((null), heap, execute). UI text always has an uppercase letter, a
# space, or bracketed punctuation beyond a bare (word).
INTERNAL = re.compile(
    r"^(?:[\w.-]+[/\\][\w./\\-]*|[\w-]+\.\w{1,5}|[a-z]+_[\w]+|\([a-z]+\)|[a-z]+)$"
)


def unescape(lit: str) -> str:
    return lit.replace('\\"', '"').replace("\\n", "\n").replace("\\\\", "\\")


def wrappable(lit: str) -> bool:
    if not lit or ID_ONLY.match(lit) or INTERNAL.match(lit):
        return False
    return bool(NO_LETTERS.search(lit))


def extract(path: Path) -> set[str]:
    keys: set[str] = set()
    text = path.read_text(encoding="utf-8", errors="replace")
    for pat, gidx in zip(PATTERNS, GROUP_IDX, strict=True):
        for m in pat.finditer(text):
            # Merge immediately adjacent literals: TR("a " "b")
            tail = text[m.end(gidx) + 1 :]
            joined = m.group(gidx)
            mm = re.match(r"\s*", tail)
            rest = tail[mm.end() :] if mm else tail
            while rest.startswith('"'):
                nxt = re.match(STR, rest)
                if not nxt:
                    break
                joined += nxt.group(1)
                rest = rest[nxt.end() :].lstrip()
            # TR() is always an intentional wrap — skip the INTERNAL-ID
            # heuristic for it (genuine UI words like "direct"/"relayed"
            # match the lowercase-identifier rule but must be catalogued).
            if pat is PATTERNS[0]:
                if joined and not ID_ONLY.match(joined) and NO_LETTERS.search(joined):
                    keys.add(unescape(joined))
            elif wrappable(joined):
                keys.add(unescape(joined))
    for m in COMBO_PAIR.finditer(text):
        if wrappable(m.group(1)):
            keys.add(unescape(m.group(1)))
    for m in DISABLED_MAP.finditer(text):
        for s in STR_RE.finditer(m.group(1)):
            # ".Enabled"-style CVar suffixes also appear in these blocks —
            # dotted fragments are identifiers, never display text.
            if not s.group(1).startswith(".") and wrappable(s.group(1)):
                keys.add(unescape(s.group(1)))
    return keys


def main() -> int:
    keys: set[str] = set()
    for d in SCAN_DIRS:
        for f in sorted(d.rglob("*")):
            if f.suffix in (".cpp", ".h", ".hpp", ".c"):
                keys |= extract(f)

    print(f"{len(keys)} translatable keys")
    if "--write" in sys.argv:
        LANG_DIR.mkdir(parents=True, exist_ok=True)
        out = LANG_DIR / "catalog.json"
        out.write_text(
            json.dumps({k: "" for k in sorted(keys)}, ensure_ascii=False, indent=4) + "\n",
            encoding="utf-8",
        )
        print(f"wrote {out}")
    for arg in sys.argv[1:]:
        if arg.startswith("--sync="):
            code = arg.split("=", 1)[1]
            target = LANG_DIR / f"{code}.json"
            table = json.loads(target.read_text(encoding="utf-8")) if target.exists() else {}
            added = 0
            for k in sorted(keys):
                if k not in table:
                    table[k] = k  # placeholder = English
                    added += 1
            target.write_text(
                json.dumps(table, ensure_ascii=False, indent=4) + "\n", encoding="utf-8"
            )
            print(f"{target}: +{added} keys ({len(table)} total)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
