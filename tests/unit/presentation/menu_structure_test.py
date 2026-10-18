"""Structural usability checks for the port menu widget registry.

Parses the AddSidebarEntry/AddWidget call sequences in
src/presentation/PortMenu*.cpp and enforces the navigation and
self-documentation invariants the UI relies on (audit §52/§53):

  1. Every registered sidebar section receives at least one widget — an
     empty section renders as a dead sidebar entry users can navigate
     into but never use.
  2. No duplicate literal widget names inside one sidebar — ImGui IDs are
     derived from the visible label, so duplicates collide and steal
     focus (keyboard/controller nav jumps between the twins).
  3. Interactive widgets (buttons, cvar controls, inputs, color pickers,
     window buttons) must carry a .Tooltip() or .DisabledTooltip() —
     every control a player can focus must explain itself.
  4. WIDGET_BUTTON blocks must set .Callback() — a button with no action
     is a dead control.
  5. Widgets must not be placed in a column beyond the sidebar's declared
     columnCount — such a widget never draws and is unreachable.
  6. Every WIDGET_* enum member in MenuTypes.h has a `case` in the
     presentation draw path — a new widget type with no renderer would
     compile but draw nothing (Menu.cpp/UIWidgets.cpp coverage).
  7. Every user-facing literal is a key in assets/lang/catalog.json —
     TR() strings are scanned across all of src/ (menus, first-run
     wizard, netlobby status), plus AddWidget/AddSidebarEntry/
     AddMenuEntry names and .Tooltip() text in presentation/. New UI
     text must enter the translation table or it silently ships
     English-only. C adjacent-literal concatenation is joined before
     lookup.
  8. Set parity: every catalog key exists in es/fr/de/pt (missing keys
     render as raw English); every lang key exists in catalog (stale
     keys get flagged rather than silently rotting).
  9. Every WIDGET_CVAR_* widget with a .CVar() binding declares its
     default explicitly (.DefaultValue()/.DefaultIndex()) — the option
     structs carry implicit fallbacks (checkbox=false, int slider=1,
     float slider=1.0, combobox=map key 0) that do NOT necessarily match
     the runtime CVar default, and both "Restore section defaults" and
     the Modified panel trust the declared value.

Widget attachment follows the WidgetPath state (path.sidebarName,
path.column) exactly like the runtime does — AddSidebarEntry only
registers the section; widgets bind to whichever sidebar `path` names.

Run:  python tests/unit/presentation/menu_structure_test.py
Exit code 0 = pass, 1 = failures printed to stdout.
"""

import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
PRESENTATION = REPO / "src" / "presentation"
SOURCES = sorted(PRESENTATION.glob("PortMenu*.cpp"))
ALL_PRESENTATION = sorted(PRESENTATION.glob("*.cpp"))
CATALOG = json.loads((REPO / "assets" / "lang" / "catalog.json").read_text(encoding="utf-8"))
MENU_TYPES = (REPO / "src" / "presentation" / "MenuTypes.h").read_text(
    encoding="utf-8", errors="replace")

SIDEBAR_RE = re.compile(
    r'AddSidebarEntry\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*(\d+)\s*\)')
PATH_INIT_RE = re.compile(
    r'WidgetPath\s+path\s*=\s*\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*SECTION_COLUMN_(\d+)')
SIDENAME_RE = re.compile(r'path\.sidebarName\s*=\s*"([^"]+)"')
SECNAME_RE = re.compile(r'path\.sectionName\s*=\s*"([^"]+)"')
WIDGET_RE = re.compile(r'AddWidget\(\s*path\s*,')
NAME_STR_RE = re.compile(r'AddWidget\(\s*path\s*,\s*"([^"]+)"')
TYPE_RE = re.compile(r'AddWidget\(\s*path\s*,[^,]+,\s*(WIDGET_[A-Z_]+)')
COLUMN_RE = re.compile(r'path\.column\s*=\s*SECTION_COLUMN_(\d+)')

EVENT_RE = re.compile(
    r'AddSidebarEntry\('
    r'|WidgetPath\s+path\s*='
    r'|path\.sidebarName\s*='
    r'|path\.sectionName\s*='
    r'|path\.column\s*=\s*SECTION_COLUMN_'
    r'|AddWidget\(\s*path\s*,')

NON_INTERACTIVE = {
    "WIDGET_SEPARATOR", "WIDGET_SEPARATOR_TEXT", "WIDGET_TEXT",
    "WIDGET_SEARCH", "WIDGET_CUSTOM",
}


def statement_end(text, pos):
    """Index just past the ';' terminating the statement at pos.

    Counts () only — braces inside lambdas stay inside the enclosing
    parens; string literals and comments are skipped."""
    depth = 0
    i = pos
    n = len(text)
    in_str = in_line_comment = in_block_comment = False
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ''
        if in_line_comment:
            if ch == '\n':
                in_line_comment = False
        elif in_block_comment:
            if ch == '*' and nxt == '/':
                in_block_comment = False
                i += 1
        elif in_str:
            if ch == '\\':
                i += 1
            elif ch == '"':
                in_str = False
        elif ch == '"':
            in_str = True
        elif ch == '/' and nxt == '/':
            in_line_comment = True
            i += 1
        elif ch == '/' and nxt == '*':
            in_block_comment = True
            i += 1
        elif ch == '(':
            depth += 1
        elif ch == ')':
            depth = max(0, depth - 1)
        elif ch == ';' and depth == 0:
            return i + 1
        i += 1
    return n


LIT_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')

def literal_seq(text, pos):
    """Decode a run of adjacent C string literals starting at text[pos]=='"'.
    Returns (decoded_text, end_pos); '' if pos is not a literal."""
    end, parts = pos, []
    while True:
        m = LIT_RE.match(text, end)
        if not m:
            break
        parts.append(m.group(1))
        end = m.end()
        nxt = end
        while nxt < len(text) and text[nxt] in ' \t\n\r':
            nxt += 1
        if nxt >= len(text) or text[nxt] != '"':
            break
        end = nxt
    decoded = ''.join(parts)
    for esc, rep in (('\\\\', ''), ('\\n', '\n'), ('\\t', '\t'),
                     ('\\"', '"'), ("\\'", "'")):
        decoded = decoded.replace(esc, rep)
    return decoded, end


def literals_after(text, call_re):
    """Literal sequence immediately following each call_re match."""
    out = []
    for m in call_re.finditer(text):
        p = m.end()
        while p < len(text) and text[p] in ' \t\n\r':
            p += 1
        if p < len(text) and text[p] == '"':
            decoded, _ = literal_seq(text, p)
            out.append(decoded)
    return out


TR_RE = re.compile(r'\bTR\(')
MENU_ENTRY_RE = re.compile(r'AddMenuEntry\(')
SIDEBAR_NAME_RE = re.compile(r'AddSidebarEntry\("[^"]*"\s*,\s*')
WIDGET_NAME_RE = re.compile(r'AddWidget\([^,()]+,\s*')
TOOLTIP_RE = re.compile(r'\.Tooltip\(')


def check_widget_enum_coverage(failures):
    """Every WIDGET_* member must appear as `case WIDGET_*:` somewhere in
    presentation — otherwise the type draws nothing."""
    enum_block = re.search(
        r'typedef\s+enum\s*\{(.*?)\}\s*WidgetType', MENU_TYPES, re.DOTALL)
    if not enum_block:
        failures.append("MenuTypes.h: WidgetType enum not found")
        return
    members = set(re.findall(r'\b(WIDGET_[A-Z_]+)\b', enum_block.group(1)))
    handled = set()
    for src in ALL_PRESENTATION:
        handled.update(re.findall(
            r'case\s+(WIDGET_[A-Z_]+)\s*:', src.read_text(encoding="utf-8",
                                                          errors="replace")))
    # WidgetInfo::HasInputFocus-style switches in the header itself also count.
    handled.update(re.findall(r'case\s+(WIDGET_[A-Z_]+)\s*:', MENU_TYPES))
    for w in sorted(members - handled):
        failures.append(f"MenuTypes.h: {w} has no case in the presentation "
                        "draw path")


def check_i18n_coverage(failures):
    """Every user-facing literal must exist as a catalog key, and the five
    language tables must carry the same key set."""
    missing = []
    # TR() literals anywhere under src/ (menus, first_run, netlobby, ...)
    for src in sorted(REPO.glob("src/**/*.cpp")):
        text = src.read_text(encoding="utf-8", errors="replace")
        if 'TR(' not in text:
            continue
        rel = src.relative_to(REPO)
        for lit in literals_after(text, TR_RE):
            if lit and lit not in CATALOG:
                missing.append((rel, lit))
    # Widget-structure literals (names/tooltips) in presentation only
    for src in ALL_PRESENTATION:
        text = src.read_text(encoding="utf-8", errors="replace")
        rel = src.relative_to(REPO)
        for call_re in (MENU_ENTRY_RE, SIDEBAR_NAME_RE,
                        WIDGET_NAME_RE, TOOLTIP_RE):
            for lit in literals_after(text, call_re):
                if lit and lit not in CATALOG:
                    missing.append((rel, lit))
    for rel, lit in sorted(set(missing)):
        failures.append(f"{rel}: UI literal missing from assets/lang/"
                        f"catalog.json: '{lit[:80]}'")

    # Set parity across the five tables
    lang_dir = REPO / "assets" / "lang"
    cat_keys = set(CATALOG)
    for code in ("es", "fr", "de", "pt"):
        table = json.loads((lang_dir / f"{code}.json").read_text(encoding="utf-8"))
        for k in sorted(cat_keys - set(table)):
            failures.append(f"lang/{code}.json: missing key '{k[:80]}'")
        for k in sorted(set(table) - cat_keys):
            failures.append(f"lang/{code}.json: key not in catalog.json "
                            f"'{k[:80]}'")


def main():
    failures = []
    total_widgets = 0
    total_sidebars = 0

    for src in SOURCES:
        text = src.read_text(encoding="utf-8", errors="replace")
        rel = src.relative_to(REPO)

        declared = {}           # (section, sidebar) -> columnCount
        counts = {}             # (section, sidebar) -> widgets attached
        seen = {}               # sidebar ctx -> set of literal names
        cur_section = None
        cur_sidebar = None
        column = 1

        for m in EVENT_RE.finditer(text):
            tok = m.group(0)
            if tok.startswith('AddSidebarEntry'):
                sm = SIDEBAR_RE.match(text, m.start())
                if sm:
                    declared[(sm.group(1), sm.group(2))] = int(sm.group(3))
                    counts.setdefault((sm.group(1), sm.group(2)), 0)
                    seen.setdefault((sm.group(1), sm.group(2)), set())
            elif tok.startswith('WidgetPath'):
                pm = PATH_INIT_RE.match(text, m.start())
                if pm:
                    cur_section, cur_sidebar = pm.group(1), pm.group(2)
                    column = int(pm.group(3))
            elif tok.startswith('path.sidebarName'):
                sm = SIDENAME_RE.match(text, m.start())
                if sm:
                    cur_sidebar = sm.group(1)
            elif tok.startswith('path.sectionName'):
                sm = SECNAME_RE.match(text, m.start())
                if sm:
                    cur_section = sm.group(1)
            elif tok.startswith('path.column'):
                cm = COLUMN_RE.match(text, m.start())
                if cm:
                    column = int(cm.group(1))
            else:  # AddWidget
                end = statement_end(text, m.start())
                stmt = text[m.start():end]
                key = (cur_section, cur_sidebar)

                if cur_sidebar is None:
                    failures.append(
                        f"{rel}: AddWidget with no active sidebar "
                        f"@ byte {m.start()}")
                    continue

                counts[key] = counts.get(key, 0) + 1
                seen.setdefault(key, set())

                if key not in declared:
                    failures.append(
                        f"{rel}: widget bound to sidebar "
                        f"'{cur_section}/{cur_sidebar}' that has no "
                        f"AddSidebarEntry yet — runtime assert in "
                        f"AddWidget aborts on this")

                declared_cols = declared.get(key)
                if declared_cols is not None and column > declared_cols:
                    failures.append(
                        f"{rel}: widget in column {column} but sidebar "
                        f"'{cur_sidebar}' declared {declared_cols} column(s)")

                nm = NAME_STR_RE.search(stmt)
                label = nm.group(1) if nm else "<dynamic>"
                if nm:
                    if nm.group(1) in seen[key]:
                        failures.append(
                            f"{rel}: duplicate widget name '{nm.group(1)}' "
                            f"in '{cur_sidebar}' (ImGui ID collision)")
                    seen[key].add(nm.group(1))

                tm = TYPE_RE.search(stmt)
                wtype = tm.group(1) if tm else None

                if wtype == "WIDGET_BUTTON" and ".Callback(" not in stmt:
                    failures.append(
                        f"{rel}: WIDGET_BUTTON '{label}' has no .Callback()")

                if (wtype and wtype.startswith("WIDGET_CVAR")
                        and ".CVar(" in stmt
                        and ".DefaultValue(" not in stmt
                        and ".DefaultIndex(" not in stmt):
                    failures.append(
                        f"{rel}: CVar widget '{label}' ({wtype}) declares no "
                        "explicit default — Restore/Modified use the option "
                        "struct's implicit fallback, not the runtime CVar "
                        "default")

                if (wtype and wtype not in NON_INTERACTIVE
                        and ".Tooltip(" not in stmt
                        and ".DisabledTooltip(" not in stmt):
                    failures.append(
                        f"{rel}: interactive widget '{label}' ({wtype}) "
                        "has no tooltip")

        for (sec, sb), n in counts.items():
            if n == 0:
                failures.append(
                    f"{rel}: sidebar '{sec}/{sb}' declared but never populated")
        total_widgets += sum(counts.values())
        total_sidebars += len(counts)

    check_widget_enum_coverage(failures)
    check_i18n_coverage(failures)

    if total_widgets == 0:
        failures.append("no AddWidget calls found in PortMenu*.cpp")

    if failures:
        print("menu_structure_test: FAIL")
        for f in failures:
            print(f"  - {f}")
        return 1

    print(f"menu_structure_test: PASS "
          f"({total_sidebars} sidebars, {total_widgets} widgets, "
          f"{len(SOURCES)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
