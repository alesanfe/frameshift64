"""Tests for tools/analysis/lint/check_complexity.py.

Covers the checker's own contract (post-audit plan item 17):
threshold boundaries (CCN 15 / NLOC 100 are violations, one below is
not), priority classification, baseline parsing, and the regression/
improvement split that decides FAIL vs NOTE vs OK.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "analysis" / "lint"))

import check_complexity as cc  # noqa: E402


def _xml_item(func: str, path: str, line: int, nloc: int, ccn: int) -> str:
    """cppncss-style lizard item: values are (Nr, NLOC, CCN)."""
    return (
        f'<item name="{func} at {path}:{line}">'
        f"<value>1</value><value>{nloc}</value><value>{ccn}</value>"
        f"</item>"
    )


def _write_xml(tmp_path: Path, items: list[str]) -> Path:
    p = tmp_path / "lizard.xml"
    p.write_text(
        '<?xml version="1.0" ?><cppncss><measure><labels>'
        "<label>Nr</label><label>NCSS</label><label>CCN</label>"
        "</labels>" + "".join(items) + "</measure></cppncss>",
        encoding="utf-8")
    return p


# ── threshold boundaries ───────────────────────────────────────────────

def test_ccn_14_passes_ccn_15_violates(tmp_path):
    xml = _write_xml(tmp_path, [
        _xml_item("under()", "src/x.cpp", 1, nloc=10, ccn=cc.CCN_LIMIT - 1),
        _xml_item("at_limit()", "src/x.cpp", 20, nloc=10, ccn=cc.CCN_LIMIT),
    ])
    v = cc.parse_lizard_xml(xml)
    assert "src/x.cpp" in v
    assert [f for f, c, n in v["src/x.cpp"]] == ["at_limit()"]


def test_nloc_99_passes_nloc_100_violates(tmp_path):
    xml = _write_xml(tmp_path, [
        _xml_item("under()", "src/x.cpp", 1, nloc=cc.NLOC_LIMIT - 1, ccn=1),
        _xml_item("at_limit()", "src/x.cpp", 100, nloc=cc.NLOC_LIMIT, ccn=1),
    ])
    v = cc.parse_lizard_xml(xml)
    assert [f for f, c, n in v["src/x.cpp"]] == ["at_limit()"]


def test_malformed_items_skipped(tmp_path):
    xml = _write_xml(tmp_path, [
        '<item name="not-a-loc"><value>1</value><value>2</value></item>',
        '<item name="f() at a/b.cpp:9"><value>1</value></item>',
    ])
    assert cc.parse_lizard_xml(xml) == {}


# ── priority classification ────────────────────────────────────────────

@pytest.mark.parametrize("path,prio", [
    ("src/game/engine/relocation/reloc.cpp", "P0"),
    ("src/persistence/save.cpp", "P0"),
    ("src/networking/netplay/rollback/sync.cpp", "P0"),
    ("src/networking/netplay/session/netlobby.cpp", "P1"),
    ("src/application/app.cpp", "P1"),
    ("src/platform/window/wnd.cpp", "P1"),
    ("src/presentation/Menu.cpp", "P2"),
    ("src/observability/log.cpp", "P2"),
    ("tools/fuzz/x.py", "P3"),
    ("platforms/switch/main.cpp", "P3"),
    ("src/unclassified_leaf.cpp", "P2"),
])
def test_classify(path, prio):
    assert cc.classify(path) == prio


def test_classify_first_match_wins():
    # netplay/rollback matches P0 before the broader src/networking/ P1.
    assert cc.classify("src/networking/netplay/rollback/x.cpp") == "P0"
    assert cc.classify("src/networking/netplay/session/x.cpp") == "P1"


# ── baseline parsing ───────────────────────────────────────────────────

def test_load_baseline_missing_file(tmp_path):
    assert cc.load_baseline(tmp_path / "nope.txt") == {}


def test_load_baseline_comments_and_blanks(tmp_path):
    p = tmp_path / "baseline.txt"
    p.write_text(
        "# header comment\n"
        "src/a.cpp 3\n"
        "\n"
        "src/b.cpp 1  # trailing comment\n",
        encoding="utf-8")
    assert cc.load_baseline(p) == {"src/a.cpp": 3, "src/b.cpp": 1}


# ── regression / improvement split ─────────────────────────────────────

def _v(path: str, n: int) -> dict[str, list[tuple[str, int, int]]]:
    return {path: [(f"f{i}()", 20, 10) for i in range(n)]} if n else {}


def test_new_violation_is_regression():
    reg, imp = cc.compare_to_baseline(_v("src/new.cpp", 1), {})
    assert reg == [("src/new.cpp", 1, 0)]
    assert imp == []


def test_over_quota_is_regression():
    reg, _ = cc.compare_to_baseline(_v("src/a.cpp", 4), {"src/a.cpp": 3})
    assert reg == [("src/a.cpp", 4, 3)]


def test_at_quota_passes():
    reg, imp = cc.compare_to_baseline(_v("src/a.cpp", 3), {"src/a.cpp": 3})
    assert reg == [] and imp == []


def test_cleaned_file_is_improvement():
    reg, imp = cc.compare_to_baseline({}, {"src/a.cpp": 2})
    assert reg == []
    assert imp == [("src/a.cpp", 0, 2)]


def test_below_quota_not_flagged_as_improvement():
    # partial improvement keeps its baseline entry (the NOTE path only
    # fires when the file is fully clean) — mirrors main()'s contract.
    reg, imp = cc.compare_to_baseline(_v("src/a.cpp", 1), {"src/a.cpp": 3})
    assert reg == [] and imp == []


# ── live check still passes after refactor ─────────────────────────────

def test_ratchet_against_repo_baseline():
    """The refactored pipeline against the real tree + real baseline.

    Needs lizard on PATH; skips where it isn't installed (CI runs the
    real checker separately).
    """
    if not cc.shutil.which("lizard"):
        pytest.skip("lizard not installed")
    violations = cc.collect_violations()
    reg, _ = cc.compare_to_baseline(violations, cc.load_baseline())
    assert reg == [], f"new over-budget functions: {reg}"
